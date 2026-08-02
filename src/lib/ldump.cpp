#include "lib/ldump.hpp"

#include "runtime/string.hpp"
#include "vm/state.hpp"

#include <cstring>

namespace lj3 {

namespace {

constexpr char kMagic[] = {'L', 'J', '3', '\0'};
constexpr uint8_t kVersion = 1;

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
      panic("undump: truncated input at " + std::to_string(pos_) + "/" +
            std::to_string(data_.size()));
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

  double f64() {
    uint64_t bits = u64();
    double v;
    std::memcpy(&v, &bits, sizeof(v));
    return v;
  }

  std::string str() {
    uint32_t n = u32();
    if (pos_ + n > data_.size())
      panic("undump: truncated string");
    std::string s(data_.substr(pos_, n));
    pos_ += n;
    return s;
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
    w.u64(static_cast<uint64_t>(v.as_int()));
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
  case ValueTag::Int: {
    uint64_t u = r.u64();
    int64_t n;
    std::memcpy(&n, &u, sizeof(n));
    return TValue::integer(n);
  }
  case ValueTag::Float:
    return TValue::number(r.f64());
  case ValueTag::String:
    return TValue::obj(ValueTag::String, L->intern(r.str()));
  default:
    panic("undump: bad constant tag");
  }
}

void write_proto(Writer& w, Proto* p, bool strip) {
  w.u32(static_cast<uint32_t>(p->numparams));
  w.u8(p->is_vararg ? 1 : 0);
  w.u32(static_cast<uint32_t>(p->maxstack));

  w.u32(static_cast<uint32_t>(p->code.size()));
  for (auto ins : p->code)
    w.u32(ins);

  w.u32(static_cast<uint32_t>(p->constants.size()));
  for (auto& k : p->constants)
    write_const(w, k);

  w.u32(static_cast<uint32_t>(p->protos.size()));
  for (auto* ch : p->protos)
    write_proto(w, ch, strip);

  w.u32(static_cast<uint32_t>(p->upvalues.size()));
  for (auto& uv : p->upvalues) {
    w.u8(uv.instack ? 1 : 0);
    w.u8(uv.idx);
    w.str(uv.name);
  }

  if (strip) {
    w.str("");
    w.u32(0);
    w.u32(0);
    return;
  }

  w.str(p->source);
  w.u32(static_cast<uint32_t>(p->linedefined));
  w.u32(static_cast<uint32_t>(p->lastlinedefined));
  w.u32(static_cast<uint32_t>(p->lineinfo.size()));
  for (int li : p->lineinfo)
    w.u32(static_cast<uint32_t>(li));

  w.u32(static_cast<uint32_t>(p->locvars.size()));
  for (auto& lv : p->locvars) {
    w.str(lv.name);
    w.u32(static_cast<uint32_t>(lv.startpc));
    w.u32(static_cast<uint32_t>(lv.endpc));
  }
}

Proto* read_proto(State* L, Reader& r, const std::string& name, bool strip) {
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

  uint32_t nproto = r.u32();
  p->protos.resize(nproto);
  for (uint32_t i = 0; i < nproto; ++i)
    p->protos[i] = read_proto(L, r, name, strip);

  uint32_t nup = r.u32();
  p->upvalues.resize(nup);
  for (uint32_t i = 0; i < nup; ++i) {
    p->upvalues[i].instack = r.u8() != 0;
    p->upvalues[i].idx = r.u8();
    p->upvalues[i].name = r.str();
  }

  if (strip) {
    (void)r.str();
    (void)r.u32();
    (void)r.u32();
    p->source = name;
    return p;
  }

  p->source = r.str();
  p->linedefined = static_cast<int>(r.u32());
  p->lastlinedefined = static_cast<int>(r.u32());
  uint32_t nline = r.u32();
  p->lineinfo.resize(nline);
  for (uint32_t i = 0; i < nline; ++i)
    p->lineinfo[i] = static_cast<int>(r.u32());

  uint32_t nloc = r.u32();
  p->locvars.resize(nloc);
  for (uint32_t i = 0; i < nloc; ++i) {
    p->locvars[i].name = r.str();
    p->locvars[i].startpc = static_cast<int>(r.u32());
    p->locvars[i].endpc = static_cast<int>(r.u32());
  }
  return p;
}

} // namespace

bool is_proto_dump(std::string_view blob) {
  return blob.size() >= 4 && std::memcmp(blob.data(), kMagic, 4) == 0;
}

std::string dump_proto(Proto* p, bool strip) {
  Writer w;
  w.bytes(kMagic, 4);
  w.u8(kVersion);
  w.u8(strip ? 1 : 0);
  write_proto(w, p, strip);
  return w.take();
}

Proto* undump_proto(State* L, const std::string& blob, const std::string& name) {
  if (!is_proto_dump(blob))
    panic("bad binary chunk");
  Reader r(blob.substr(4)); // skip magic
  (void)r.u8();             // version
  bool strip = r.u8() != 0;
  return read_proto(L, r, name, strip);
}

} // namespace lj3
