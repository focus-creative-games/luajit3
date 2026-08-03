#include "lib/libs.hpp"

#include "lib/lib_util.hpp"

#include <climits>
#include <cstdlib>
#include <string>

namespace luatier {
using namespace lib;

constexpr uint32_t kMaxUnicode = 0x10FFFF;

static bool iscont(unsigned char c) { return (c & 0xC0) == 0x80; }

// Relative position: negative means from end. Matches PUC u_posrelat.
static int64_t u_posrelat(int64_t pos, size_t len) {
  if (pos >= 0)
    return pos;
  if (static_cast<size_t>(-pos) > len)
    return 0;
  return static_cast<int64_t>(len) + pos + 1;
}

// Decode one UTF-8 sequence at `i`. Returns byte length, or 0 if invalid.
static size_t utf8_decode(std::string_view s, size_t i, uint32_t* out) {
  static const unsigned int limits[] = {0xFF, 0x7F, 0x7FF, 0xFFFF};
  if (i >= s.size())
    return 0;
  unsigned int c = static_cast<unsigned char>(s[i]);
  unsigned int res = 0;
  size_t count = 0;
  if (c < 0x80) {
    res = c;
  } else {
    while (c & 0x40) {
      if (i + 1 + count >= s.size())
        return 0;
      int cc = static_cast<unsigned char>(s[i + 1 + count]);
      if ((cc & 0xC0) != 0x80)
        return 0;
      res = (res << 6) | (static_cast<unsigned int>(cc) & 0x3F);
      c <<= 1;
      ++count;
    }
    res |= ((c & 0x7F) << (count * 5));
    if (count > 3 || res > kMaxUnicode || res <= limits[count])
      return 0;
  }
  if (out)
    *out = res;
  return count + 1;
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
  int64_t posi = u_posrelat(L->gettop() >= 2 ? check_int(L, 2) : 1, s.size());
  int64_t posj = u_posrelat(L->gettop() >= 3 ? check_int(L, 3) : -1, s.size());
  if (!(1 <= posi && --posi <= static_cast<int64_t>(s.size())))
    panic("initial position out of string");
  if (!(--posj < static_cast<int64_t>(s.size())))
    panic("final position out of string");
  int n = 0;
  while (posi <= posj) {
    size_t sz = utf8_decode(s, static_cast<size_t>(posi), nullptr);
    if (sz == 0) {
      L->settop(0);
      L->push(TValue::nil());
      L->push(TValue::integer(posi + 1));
      return 2;
    }
    posi += static_cast<int64_t>(sz);
    ++n;
  }
  L->settop(0);
  L->push(TValue::integer(n));
  return 1;
}

static int utf8_char(State* L) {
  int narg = L->gettop();
  std::string out;
  char buf[4];
  for (int i = 1; i <= narg; ++i) {
    int64_t code = check_int(L, i);
    if (code < 0 || static_cast<uint32_t>(code) > kMaxUnicode)
      panic("value out of range");
    size_t n = utf8_encode(static_cast<uint32_t>(code), buf);
    out.append(buf, n);
  }
  L->settop(0);
  push_string(L, out);
  return 1;
}

static int utf8_codes_iter(State* L) {
  std::string_view s = check_string(L, 1)->view();
  int64_t n = L->at(2)->is_number() ? static_cast<int64_t>(L->at(2)->to_number()) - 1 : -1;
  if (n < 0)
    n = 0;
  else if (n < static_cast<int64_t>(s.size())) {
    ++n;
    while (n < static_cast<int64_t>(s.size()) && iscont(static_cast<unsigned char>(s[static_cast<size_t>(n)])))
      ++n;
  }
  if (n >= static_cast<int64_t>(s.size())) {
    L->settop(0);
    return 0;
  }
  uint32_t code = 0;
  size_t sz = utf8_decode(s, static_cast<size_t>(n), &code);
  if (sz == 0)
    panic("invalid UTF-8 code");
  L->settop(0);
  L->push(TValue::integer(n + 1));
  L->push(TValue::integer(static_cast<int64_t>(code)));
  return 2;
}

static int utf8_codes(State* L) {
  check_string(L, 1);
  L->push(TValue::obj(ValueTag::Function, closure_new_c(L, utf8_codes_iter)));
  L->push(*L->at(1));
  L->push(TValue::integer(0));
  return 3;
}

static int utf8_codepoint(State* L) {
  std::string_view s = check_string(L, 1)->view();
  int64_t posi = u_posrelat(L->gettop() >= 2 ? check_int(L, 2) : 1, s.size());
  int64_t pose = u_posrelat(L->gettop() >= 3 ? check_int(L, 3) : posi, s.size());
  if (posi < 1)
    panic("out of range");
  if (pose > static_cast<int64_t>(s.size()))
    panic("out of range");
  if (posi > pose) {
    L->settop(0);
    return 0;
  }
  if (pose - posi >= INT_MAX)
    panic("string slice too long");
  L->settop(0);
  size_t p = static_cast<size_t>(posi - 1);
  size_t end = static_cast<size_t>(pose);
  int n = 0;
  while (p < end) {
    uint32_t code = 0;
    size_t sz = utf8_decode(s, p, &code);
    if (sz == 0)
      panic("invalid UTF-8 code");
    L->push(TValue::integer(static_cast<int64_t>(code)));
    p += sz;
    ++n;
  }
  return n;
}

static int utf8_offset(State* L) {
  std::string_view s = check_string(L, 1)->view();
  int64_t n = check_int(L, 2);
  int64_t posi = (n >= 0) ? 1 : static_cast<int64_t>(s.size()) + 1;
  posi = u_posrelat(L->gettop() >= 3 ? check_int(L, 3) : posi, s.size());
  if (!(1 <= posi && --posi <= static_cast<int64_t>(s.size())))
    panic("position out of range");
  if (n == 0) {
    while (posi > 0 && iscont(static_cast<unsigned char>(s[static_cast<size_t>(posi)])))
      --posi;
  } else {
    if (posi < static_cast<int64_t>(s.size()) &&
        iscont(static_cast<unsigned char>(s[static_cast<size_t>(posi)])))
      panic("initial position is a continuation byte");
    // Empty string: posi==0 after --, and s[posi] must not be read; PUC checks iscont on
    // the final '\0' which is never a continuation — OK. For empty s, posi becomes 0.
    if (n < 0) {
      while (n < 0 && posi > 0) {
        do {
          --posi;
        } while (posi > 0 && iscont(static_cast<unsigned char>(s[static_cast<size_t>(posi)])));
        ++n;
      }
    } else {
      --n; // do not move for 1st character
      while (n > 0 && posi < static_cast<int64_t>(s.size())) {
        do {
          ++posi;
        } while (posi < static_cast<int64_t>(s.size()) &&
                 iscont(static_cast<unsigned char>(s[static_cast<size_t>(posi)])));
        --n;
      }
    }
  }
  L->settop(0);
  if (n == 0)
    L->push(TValue::integer(posi + 1));
  else
    L->push(TValue::nil());
  return 1;
}

void open_utf8_lib(State* L) {
  Table* u = new_lib(L, 8);
  set_field(L, u, "len", utf8_len);
  set_field(L, u, "char", utf8_char);
  set_field(L, u, "codes", utf8_codes);
  set_field(L, u, "codepoint", utf8_codepoint);
  set_field(L, u, "offset", utf8_offset);
  // PUC UTF8PATT — may contain embedded '\0', so use length-aware intern.
  static const char kUtf8Patt[] = "[\0-\x7F\xC2-\xF4][\x80-\xBF]*";
  set_field_value(L, u, "charpattern",
                  TValue::obj(ValueTag::String,
                              L->intern(std::string_view(kUtf8Patt, sizeof(kUtf8Patt) - 1))));
  set_global_value(L, "utf8", TValue::obj(ValueTag::Table, u));
}

} // namespace luatier
