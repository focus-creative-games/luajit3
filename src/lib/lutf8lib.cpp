#include "lib/libs.hpp"

#include "lib/lib_util.hpp"

#include <string>

namespace lj3 {
using namespace lib;

static size_t utf8_decode(std::string_view s, size_t i, uint32_t& out) {
  if (i >= s.size())
    return 0;
  unsigned char c = static_cast<unsigned char>(s[i]);
  if (c < 0x80) {
    out = c;
    return 1;
  }
  if ((c & 0xE0) == 0xC0 && i + 1 < s.size()) {
    out = ((c & 0x1F) << 6) | (static_cast<unsigned char>(s[i + 1]) & 0x3F);
    return 2;
  }
  if ((c & 0xF0) == 0xE0 && i + 2 < s.size()) {
    out = ((c & 0x0F) << 12) | ((static_cast<unsigned char>(s[i + 1]) & 0x3F) << 6) |
          (static_cast<unsigned char>(s[i + 2]) & 0x3F);
    return 3;
  }
  if ((c & 0xF8) == 0xF0 && i + 3 < s.size()) {
    out = ((c & 0x07) << 18) | ((static_cast<unsigned char>(s[i + 1]) & 0x3F) << 12) |
          ((static_cast<unsigned char>(s[i + 2]) & 0x3F) << 6) |
          (static_cast<unsigned char>(s[i + 3]) & 0x3F);
    return 4;
  }
  out = c;
  return 1;
}

static size_t utf8_encode(uint32_t cp, char* buf) {
  if (cp < 0x80) {
    buf[0] = static_cast<char>(cp);
    return 1;
  }
  if (cp < 0x800) {
    buf[0] = static_cast<char>(0xC0 | (cp >> 6));
    buf[1] = static_cast<char>(0x80 | (cp & 0x3F));
    return 2;
  }
  if (cp < 0x10000) {
    buf[0] = static_cast<char>(0xE0 | (cp >> 12));
    buf[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    buf[2] = static_cast<char>(0x80 | (cp & 0x3F));
    return 3;
  }
  buf[0] = static_cast<char>(0xF0 | (cp >> 18));
  buf[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
  buf[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
  buf[3] = static_cast<char>(0x80 | (cp & 0x3F));
  return 4;
}

static int utf8_len(State* L) {
  std::string_view s = check_string(L, 1)->view();
  int64_t i = L->gettop() >= 2 ? check_int(L, 2) : 1;
  int64_t j = L->gettop() >= 3 ? check_int(L, 3) : static_cast<int64_t>(s.size());
  if (i < 0)
    i = static_cast<int64_t>(s.size()) + i + 1;
  if (j < 0)
    j = static_cast<int64_t>(s.size()) + j + 1;
  size_t pos = static_cast<size_t>(std::max<int64_t>(0, i - 1));
  size_t end = static_cast<size_t>(std::min<int64_t>(static_cast<int64_t>(s.size()), j));
  int count = 0;
  while (pos < end) {
    uint32_t cp;
    size_t n = utf8_decode(s, pos, cp);
    if (n == 0)
      break;
    pos += n;
    ++count;
  }
  L->settop(0);
  L->push(TValue::integer(count));
  return 1;
}

static int utf8_char(State* L) {
  std::string out;
  char buf[4];
  for (int i = 1; i <= L->gettop(); ++i) {
    uint32_t cp = static_cast<uint32_t>(check_int(L, i));
    size_t n = utf8_encode(cp, buf);
    out.append(buf, n);
  }
  L->settop(0);
  push_string(L, out);
  return 1;
}

static int utf8_codes_iter(State* L) {
  std::string s = std::string(L->at(1)->as_string()->view());
  int64_t pos = L->at(2)->is_number() ? static_cast<int64_t>(L->at(2)->to_number()) : 0;
  if (pos >= static_cast<int64_t>(s.size())) {
    L->settop(0);
    L->push(TValue::nil());
    return 1;
  }
  uint32_t cp;
  size_t n = utf8_decode(s, static_cast<size_t>(pos), cp);
  L->settop(0);
  L->push(TValue::integer(static_cast<int64_t>(pos + n)));
  L->push(TValue::integer(cp));
  return 2;
}

static int utf8_codes(State* L) {
  std::string s(check_string(L, 1)->view());
  L->settop(0);
  L->push(TValue::obj(ValueTag::Function, closure_new_c(L, utf8_codes_iter)));
  push_string(L, s);
  L->push(TValue::integer(0));
  return 1;
}

static int utf8_codepoint(State* L) {
  std::string_view s = check_string(L, 1)->view();
  int64_t i = L->gettop() >= 2 ? check_int(L, 2) : 1;
  int64_t j = L->gettop() >= 3 ? check_int(L, 3) : i;
  if (i < 0)
    i = static_cast<int64_t>(s.size()) + i + 1;
  if (j < 0)
    j = static_cast<int64_t>(s.size()) + j + 1;
  size_t pos = 0;
  int64_t ci = 1;
  L->settop(0);
  while (pos < s.size() && ci <= j) {
    uint32_t cp;
    size_t n = utf8_decode(s, pos, cp);
    if (ci >= i)
      L->push(TValue::integer(cp));
    pos += n;
    ++ci;
  }
  return L->gettop();
}

static int utf8_offset(State* L) {
  std::string_view s = check_string(L, 1)->view();
  int64_t n = check_int(L, 2);
  int64_t pos = L->gettop() >= 3 ? check_int(L, 3) : (n >= 0 ? 1 : static_cast<int64_t>(s.size()) + 1);
  if (pos < 1)
    pos = static_cast<int64_t>(s.size()) + pos + 1;
  size_t byte = static_cast<size_t>(pos - 1);
  if (n == 0) {
    while (byte > 0 && (static_cast<unsigned char>(s[byte]) & 0xC0) == 0x80)
      --byte;
    L->settop(0);
    L->push(TValue::integer(static_cast<int64_t>(byte + 1)));
    return 1;
  }
  int dir = n > 0 ? 1 : -1;
  int count = std::abs(static_cast<int>(n));
  while (count > 0 && byte < s.size()) {
    uint32_t cp;
    size_t sz = utf8_decode(s, byte, cp);
    if (sz == 0)
      break;
    byte += sz;
    if (dir > 0 && --count == 0)
      break;
    if (dir < 0) {
      do {
        --byte;
      } while (byte > 0 && (static_cast<unsigned char>(s[byte]) & 0xC0) == 0x80);
      if (--count == 0)
        break;
    }
  }
  L->settop(0);
  if (byte > s.size())
    L->push(TValue::nil());
  else
    L->push(TValue::integer(static_cast<int64_t>(byte + 1)));
  return 1;
}

void open_utf8_lib(State* L) {
  Table* u = new_lib(L, 8);
  set_field(L, u, "len", utf8_len);
  set_field(L, u, "char", utf8_char);
  set_field(L, u, "codes", utf8_codes);
  set_field(L, u, "codepoint", utf8_codepoint);
  set_field(L, u, "offset", utf8_offset);
  set_field_value(L, u, "charpattern", TValue::obj(ValueTag::String, L->intern("[%z\\1-\\127\\194-\\244][\\128-\\191]*")));
  set_global_value(L, "utf8", TValue::obj(ValueTag::Table, u));
}

} // namespace lj3
