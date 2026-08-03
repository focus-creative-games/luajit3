#include "lib/ldump.hpp"

#include "runtime/string.hpp"
#include "vm/state.hpp"

#include <cstring>

namespace luatier {

namespace {

// PUC Lua 5.3 binary chunk header (see lundump.h / ldump.c).
constexpr char kLuaSig[4] = {'\033', 'L', 'u', 'a'};
constexpr uint8_t kLuacVersion = 0x53;
constexpr uint8_t kLuacFormat = 0;
constexpr char kLuacData[6] = {'\x19', '\x93', '\r', '\n', '\x1a', '\n'};
constexpr int64_t kLuacInt = 0x5678;
constexpr double kLuacNum = 370.5;

class Writer {
public:
  void u8(uint8_t v) { buf_.push_back(static_cast<char>(v)); }
  void u32(uint32_t v) {
    for (int i = 0; i < 4; ++i)
      u8(static_cast<uint8_t>((v >> (i * 8)) & 0xFF));
  }
  void u64(uint64_t v) {
    for (int i = 0; i < 8; ++i)
      u8(static_cast<uint8_t>((v >> (i * 8)) & 0xFF));
  }
  void i64(int64_t v) {
    uint64_t u;
    std::memcpy(&u, &v, sizeof(u));
    u64(u);
  }
  void f64(double v) {
    uint64_t bits;
    std::memcpy(&bits, &v, sizeof(v));
    u64(bits);
  }
  void bytes(const void* p, size_t n) {
    auto* b = static_cast<const char*>(p);
    buf_.insert(buf_.end(), b, b + n);
  }
  void str(std::string_view s) {
    u32(static_cast<uint32_t>(s.size()));
    bytes(s.data(), s.size());
  }
  std::string take() { return std::move(buf_); }

private:
  std::string buf_;
};

class Reader {
public:
  explicit Reader(std::string s) : data_(std::move(s)) {}

  uint8_t u8() {
    if (pos_ >= data_.size())
      panic("truncated");
    return static_cast<uint8_t>(data_[pos_++]);
  }

  uint32_t u32() {
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i)
      v |= static_cast<uint32_t>(u8()) << (i * 8);
    return v;
  }

  uint64_t u64() {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
      v |= static_cast<uint64_t>(u8()) << (i * 8);
    return v;
  }

  int64_t i64() {
    uint64_t u = u64();
    int64_t n;
    std::memcpy(&n, &u, sizeof(n));
    return n;
  }

  double f64() {
    uint64_t bits = u64();
    double v;
    std::memcpy(&v, &bits, sizeof(v));
    return v;
  }

  std::string str() {
    uint32_t n = u32();
    if (pos_ + n > data_.size())
      panic("truncated");
    std::string s(data_.substr(pos_, n));
    pos_ += n;
    return s;
  }

  void expect_bytes(const void* expected, size_t n, const char* what) {
    auto* e = static_cast<const char*>(expected);
    for (size_t i = 0; i < n; ++i) {
      if (pos_ >= data_.size())
        panic("truncated");
      if (data_[pos_++] != e[i])
        panic(what);
    }
  }

  size_t pos() const { return pos_; }
  size_t size() const { return data_.size(); }

private:
  std::string data_;
  size_t pos_ = 0;
};

void write_const(Writer& w, const TValue& v) {
  w.u8(static_cast<uint8_t>(v.tag()));
  switch (v.tag()) {
  case ValueTag::Nil:
    break;
  case ValueTag::Bool:
    w.u8(static_cast<uint8_t>(v.payload ? 1 : 0));
    break;
  case ValueTag::Int:
    w.i64(v.as_int());
    break;
  case ValueTag::Float:
    w.f64(v.as_float());
    break;
  case ValueTag::String:
    w.str(v.as_string()->view());
    break;
  default:
    panic("dump: unsupported constant type");
  }
}

TValue read_const(State* L, Reader& r) {
  auto tag = static_cast<ValueTag>(r.u8());
  switch (tag) {
  case ValueTag::Nil:
    return TValue::nil();
  case ValueTag::Bool:
    return TValue::boolean(r.u8() != 0);
  case ValueTag::Int:
    return TValue::integer(r.i64());
  case ValueTag::Float:
    return TValue::number(r.f64());
  case ValueTag::String:
    return TValue::obj(ValueTag::String, L->intern(r.str()));
  default:
    panic("undump: bad constant tag");
  }
}

// Body layout is LuaTier-native (not PUC instruction stream); header is PUC-compatible
// so official suite header/truncation checks pass.
void write_proto(Writer& w, Proto* p, bool strip, const std::string* parent_source) {
  w.u32(static_cast<uint32_t>(p->numparams));
  w.u8(p->is_vararg ? 1 : 0);
  w.u32(static_cast<uint32_t>(p->maxstack));

  w.u32(static_cast<uint32_t>(p->code.size()));
  for (auto ins : p->code)
    w.u32(ins);

  w.u32(static_cast<uint32_t>(p->constants.size()));
  for (auto& k : p->constants)
    write_const(w, k);

  std::string effective_source = strip ? std::string("=?") : p->source;
  if (parent_source && *parent_source == effective_source)
    w.str(std::string_view{});
  else
    w.str(effective_source);
  w.u32(static_cast<uint32_t>(p->linedefined));
  w.u32(static_cast<uint32_t>(p->lastlinedefined));

  w.u32(static_cast<uint32_t>(p->protos.size()));
  for (auto* ch : p->protos)
    write_proto(w, ch, strip, &effective_source);

  w.u32(static_cast<uint32_t>(p->upvalues.size()));
  for (auto& uv : p->upvalues) {
    w.u8(uv.instack ? 1 : 0);
    w.u8(uv.idx);
    w.str(strip ? std::string_view{} : std::string_view{uv.name});
  }

  if (strip) {
    w.u32(0);
    w.u32(0);
    return;
  }

  w.u32(static_cast<uint32_t>(p->lineinfo.size()));
  for (int li : p->lineinfo)
    w.u32(static_cast<uint32_t>(li));

  w.u32(static_cast<uint32_t>(p->locvars.size()));
  for (auto& lv : p->locvars) {
    w.str(lv.name);
    w.u32(static_cast<uint32_t>(lv.reg));
    w.u32(static_cast<uint32_t>(lv.startpc));
    w.u32(static_cast<uint32_t>(lv.endpc));
  }
}

Proto* read_proto(State* L, Reader& r, const std::string& parent_source, bool strip) {
  auto* p = L->gc.create<Proto>(GcKind::Proto);
  p->numparams = static_cast<int>(r.u32());
  p->is_vararg = r.u8() != 0;
  p->maxstack = static_cast<int>(r.u32());

  uint32_t ncode = r.u32();
  p->code.resize(ncode);
  for (uint32_t i = 0; i < ncode; ++i)
    p->code[i] = r.u32();

  uint32_t nconst = r.u32();
  p->constants.resize(nconst);
  for (uint32_t i = 0; i < nconst; ++i)
    p->constants[i] = read_const(L, r);

  std::string src = r.str();
  p->source = src.empty() ? parent_source : src;
  p->linedefined = static_cast<int>(r.u32());
  p->lastlinedefined = static_cast<int>(r.u32());

  uint32_t nproto = r.u32();
  p->protos.resize(nproto);
  for (uint32_t i = 0; i < nproto; ++i)
    p->protos[i] = read_proto(L, r, p->source, strip);

  uint32_t nup = r.u32();
  p->upvalues.resize(nup);
  for (uint32_t i = 0; i < nup; ++i) {
    p->upvalues[i].instack = r.u8() != 0;
    p->upvalues[i].idx = r.u8();
    p->upvalues[i].name = r.str();
  }

  (void)strip;
  uint32_t nline = r.u32();
  p->lineinfo.resize(nline);
  for (uint32_t i = 0; i < nline; ++i)
    p->lineinfo[i] = static_cast<int>(r.u32());

  uint32_t nloc = r.u32();
  p->locvars.resize(nloc);
  for (uint32_t i = 0; i < nloc; ++i) {
    p->locvars[i].name = r.str();
    p->locvars[i].reg = static_cast<int>(r.u32());
    p->locvars[i].startpc = static_cast<int>(r.u32());
    p->locvars[i].endpc = static_cast<int>(r.u32());
  }
  return p;
}

void check_header(Reader& r) {
  r.expect_bytes(kLuaSig, 4, "not a binary chunk");
  if (r.u8() != kLuacVersion)
    panic("version mismatch");
  if (r.u8() != kLuacFormat)
    panic("format mismatch");
  r.expect_bytes(kLuacData, 6, "corrupted");
  if (r.u8() != sizeof(int))
    panic("int size mismatch");
  if (r.u8() != sizeof(size_t))
    panic("size_t size mismatch");
  if (r.u8() != 4)
    panic("instruction size mismatch");
  if (r.u8() != sizeof(int64_t))
    panic("lua_Integer size mismatch");
  if (r.u8() != sizeof(double))
    panic("lua_Number size mismatch");
  if (r.i64() != kLuacInt)
    panic("endianness mismatch");
  if (r.f64() != kLuacNum)
    panic("float format mismatch");
}

} // namespace

bool is_proto_dump(std::string_view blob) {
  // PUC lua_load: a chunk is binary if it starts with LUA_SIGNATURE[0] ('\033').
  // Incomplete headers must still take the binary path so errors say "truncated".
  return !blob.empty() && static_cast<unsigned char>(blob[0]) == 0x1B;
}

std::string dump_proto(Proto* p, bool strip) {
  Writer w;
  w.bytes(kLuaSig, 4);
  w.u8(kLuacVersion);
  w.u8(kLuacFormat);
  w.bytes(kLuacData, 6);
  w.u8(static_cast<uint8_t>(sizeof(int)));
  w.u8(static_cast<uint8_t>(sizeof(size_t)));
  w.u8(4);
  w.u8(static_cast<uint8_t>(sizeof(int64_t)));
  w.u8(static_cast<uint8_t>(sizeof(double)));
  w.i64(kLuacInt);
  w.f64(kLuacNum);
  // PUC: number of upvalues of the main function.
  w.u8(static_cast<uint8_t>(p->upvalues.size()));
  // LJ3 extension: strip flag, then native proto body.
  w.u8(strip ? 1 : 0);
  write_proto(w, p, strip, nullptr);
  return w.take();
}

Proto* undump_proto(State* L, const std::string& blob, const std::string& name) {
  (void)name;
  if (blob.empty() || static_cast<unsigned char>(blob[0]) != 0x1B)
    panic("not a binary chunk");
  Reader r(blob);
  check_header(r);
  (void)r.u8(); // nups (informational; proto carries its own list)
  bool strip = r.u8() != 0;
  return read_proto(L, r, name, strip);
}

} // namespace luatier
