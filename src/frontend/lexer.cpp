#include "frontend/lexer.hpp"

#include <cctype>
#include <clocale>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <unordered_map>

namespace luatier {

// Source numerals always use '.'; strtod would honor LC_NUMERIC otherwise.
static double c_strtod(const char* s, char** end) {
  std::string saved;
  if (const char* cur = std::setlocale(LC_NUMERIC, nullptr))
    saved = cur;
  std::setlocale(LC_NUMERIC, "C");
  double d = std::strtod(s, end);
  if (!saved.empty())
    std::setlocale(LC_NUMERIC, saved.c_str());
  return d;
}

const char* token_name(TokenKind k) {
  switch (k) {
  case TokenKind::End: return "end-of-file";
  case TokenKind::Identifier: return "identifier";
  case TokenKind::Number: return "number";
  case TokenKind::Integer: return "integer";
  case TokenKind::String: return "string";
  case TokenKind::Char: return "character";
  default: return "token";
  }
}

std::string token_to_near(const Token& t) {
  switch (t.kind) {
  case TokenKind::End:
    return "<eof>";
  case TokenKind::Identifier:
    return "'" + t.text + "'";
  case TokenKind::String:
    // PUC txtToken: "'%s'" over scanner buff (includes quotes / [[...]]).
    return "'" + (t.near.empty() ? t.text : t.near) + "'";
  case TokenKind::Number:
  case TokenKind::Integer:
    return "'" + t.text + "'";
  case TokenKind::Char: {
    auto uc = static_cast<unsigned char>(t.integer);
    if (std::isprint(uc))
      return std::string("'") + static_cast<char>(uc) + "'";
    return "'<" + std::string("\\") + std::to_string(static_cast<int>(uc)) + ">'";
  }
  case TokenKind::KwAnd: return "'and'";
  case TokenKind::KwBreak: return "'break'";
  case TokenKind::KwDo: return "'do'";
  case TokenKind::KwElse: return "'else'";
  case TokenKind::KwElseif: return "'elseif'";
  case TokenKind::KwEnd: return "'end'";
  case TokenKind::KwFalse: return "'false'";
  case TokenKind::KwFor: return "'for'";
  case TokenKind::KwFunction: return "'function'";
  case TokenKind::KwGoto: return "'goto'";
  case TokenKind::KwIf: return "'if'";
  case TokenKind::KwIn: return "'in'";
  case TokenKind::KwLocal: return "'local'";
  case TokenKind::KwNil: return "'nil'";
  case TokenKind::KwNot: return "'not'";
  case TokenKind::KwOr: return "'or'";
  case TokenKind::KwRepeat: return "'repeat'";
  case TokenKind::KwReturn: return "'return'";
  case TokenKind::KwThen: return "'then'";
  case TokenKind::KwTrue: return "'true'";
  case TokenKind::KwUntil: return "'until'";
  case TokenKind::KwWhile: return "'while'";
  case TokenKind::Plus: return "'+'";
  case TokenKind::Minus: return "'-'";
  case TokenKind::Star: return "'*'";
  case TokenKind::Slash: return "'/'";
  case TokenKind::Percent: return "'%'";
  case TokenKind::Caret: return "'^'";
  case TokenKind::Hash: return "'#'";
  case TokenKind::Amp: return "'&'";
  case TokenKind::Tilde: return "'~'";
  case TokenKind::Pipe: return "'|'";
  case TokenKind::LtLt: return "'<<'";
  case TokenKind::GtGt: return "'>>'";
  case TokenKind::SlashSlash: return "'//'";
  case TokenKind::EqEq: return "'=='";
  case TokenKind::TildeEq: return "'~='";
  case TokenKind::LtEq: return "'<='";
  case TokenKind::GtEq: return "'>='";
  case TokenKind::Lt: return "'<'";
  case TokenKind::Gt: return "'>'";
  case TokenKind::Eq: return "'='";
  case TokenKind::LParen: return "'('";
  case TokenKind::RParen: return "')'";
  case TokenKind::LBrace: return "'{'";
  case TokenKind::RBrace: return "'}'";
  case TokenKind::LBracket: return "'['";
  case TokenKind::RBracket: return "']'";
  case TokenKind::ColonColon: return "'::'";
  case TokenKind::Semi: return "';'";
  case TokenKind::Colon: return "':'";
  case TokenKind::Comma: return "','";
  case TokenKind::Dot: return "'.'";
  case TokenKind::DotDot: return "'..'";
  case TokenKind::DotDotDot: return "'...'";
  default:
    return "'<unknown>'";
  }
}

Lexer::Lexer(std::string_view src, std::string name) : src_(src), name_(std::move(name)) {
  // Skip first-line # comment (shebang #!../lua or # testing... per Lua 5.3).
  if (!src_.empty() && src_[0] == '#') {
    while (pos_ < src_.size() && src_[pos_] != '\n')
      ++pos_;
    if (pos_ < src_.size() && src_[pos_] == '\n') {
      ++pos_;
      line_ = 2;
      col_ = 1;
    }
  }
}

Token Lexer::next() {
  if (has_peek_) {
    has_peek_ = false;
    return peek_;
  }
  return lex_one();
}

Token Lexer::peek() {
  if (!has_peek_) {
    peek_ = lex_one();
    has_peek_ = true;
  }
  return peek_;
}

char Lexer::peek_char(int n) const {
  size_t i = pos_ + static_cast<size_t>(n);
  return i < src_.size() ? src_[i] : '\0';
}

char Lexer::get() {
  if (pos_ >= src_.size())
    return '\0';
  char c = src_[pos_++];
  // Newlines update line via incline(); here only advance column for non-newlines.
  if (c != '\n' && c != '\r')
    col_++;
  return c;
}

bool Lexer::match(char c) {
  if (peek_char() == c) {
    get();
    return true;
  }
  return false;
}

bool Lexer::curr_is_newline() const {
  char c = peek_char();
  return c == '\n' || c == '\r';
}

void Lexer::incline() {
  char old = get();
  if (curr_is_newline() && peek_char() != old)
    get(); // skip \n\r or \r\n
  line_++;
  col_ = 1;
}

void Lexer::skip_whitespace_and_comments() {
  for (;;) {
    char c = peek_char();
    if (c == ' ' || c == '\t' || c == '\f' || c == '\v') {
      get();
      continue;
    }
    if (curr_is_newline()) {
      incline();
      continue;
    }
    if (c == '-' && peek_char(1) == '-') {
      get();
      get();
      if (peek_char() == '[') {
        get();
        int level = 0;
        while (peek_char() == '=') {
          get();
          level++;
        }
        if (peek_char() == '[') {
          get();
          for (;;) {
            if (eos())
              panic("unfinished long comment near <eof>");
            if (curr_is_newline()) {
              incline();
              continue;
            }
            char d = get();
            if (d == ']') {
              int lvl = 0;
              while (peek_char() == '=') {
                get();
                lvl++;
              }
              if (lvl == level && match(']'))
                break;
            }
          }
          continue;
        }
      }
      // Do not treat embedded '\0' as end-of-source (PUC allows null in chunks).
      while (!eos() && !curr_is_newline())
        get();
      continue;
    }
    break;
  }
}

Token Lexer::lex_number() {
  Token t;
  t.line = line_;
  t.col = col_;
  std::string s;
  bool is_hex = false;
  if (peek_char() == '0' && (peek_char(1) == 'x' || peek_char(1) == 'X')) {
    is_hex = true;
    s.push_back(get());
    s.push_back(get());
  }
  auto is_digit = [&](char c) {
    return is_hex ? std::isxdigit(static_cast<unsigned char>(c)) != 0
                  : std::isdigit(static_cast<unsigned char>(c)) != 0;
  };
  while (is_digit(peek_char()) || peek_char() == '.')
    s.push_back(get());
  if (!is_hex && (peek_char() == 'e' || peek_char() == 'E')) {
    s.push_back(get());
    if (peek_char() == '+' || peek_char() == '-')
      s.push_back(get());
    while (std::isdigit(static_cast<unsigned char>(peek_char())))
      s.push_back(get());
  }
  if (is_hex && (peek_char() == 'p' || peek_char() == 'P')) {
    s.push_back(get());
    if (peek_char() == '+' || peek_char() == '-')
      s.push_back(get());
    while (std::isdigit(static_cast<unsigned char>(peek_char())))
      s.push_back(get());
  }
  t.text = s;
  try {
    const bool is_hex_num = s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X');
    const bool pure_int = s.find('.') == std::string::npos &&
                          (is_hex_num || (s.find('e') == std::string::npos &&
                                          s.find('E') == std::string::npos)) &&
                          s.find('p') == std::string::npos &&
                          s.find('P') == std::string::npos;
    if (pure_int) {
      // PUC l_str2int: hex has no overflow check (keeps shifting into uint64);
      // decimal above MAXINTEGER becomes a float.
      try {
        size_t idx = 0;
        const bool hex = is_hex_num;
        if (hex) {
          uint64_t u = 0;
          size_t i = 2;
          for (; i < s.size() && std::isxdigit(static_cast<unsigned char>(s[i])); ++i) {
            unsigned char c = static_cast<unsigned char>(s[i]);
            int d = std::isdigit(c) ? (c - '0') : (std::tolower(c) - 'a' + 10);
            u = (u << 4) + static_cast<uint64_t>(d);
          }
          if (i == s.size()) {
            t.integer = static_cast<int64_t>(u);
            t.kind = TokenKind::Integer;
            return t;
          }
        } else {
          unsigned long long u = std::stoull(s, &idx, 10);
          if (idx == s.size()) {
            if (u <= static_cast<unsigned long long>(INT64_MAX)) {
              t.integer = static_cast<int64_t>(u);
              t.kind = TokenKind::Integer;
            } else {
              t.number = static_cast<double>(u);
              t.kind = TokenKind::Number;
            }
            return t;
          }
        }
      } catch (...) {
        // fall through to strtod
      }
    }
    char* end = nullptr;
    t.number = c_strtod(s.c_str(), &end);
    if (!end || end == s.c_str() || *end != '\0')
      panic("malformed number near '" + s + "'");
    t.kind = TokenKind::Number;
  } catch (...) {
    panic("malformed number near '" + s + "'");
  }
  return t;
}

Token Lexer::lex_string(char quote) {
  Token t;
  t.kind = TokenKind::String;
  t.line = line_;
  t.col = col_;
  get(); // opening quote
  std::string out;
  // PUC-style save buffer for "near '...'" (source form, starts with quote).
  std::string save(1, quote);
  auto hexval = [](char c) -> int {
    if (std::isdigit(static_cast<unsigned char>(c)))
      return c - '0';
    if (c >= 'a' && c <= 'f')
      return c - 'a' + 10;
    return c - 'A' + 10;
  };
  auto escerror = [&](const char* msg) {
    // Include current char in the near-token when still available (PUC esccheck).
    if (!eos() && !curr_is_newline()) {
      save.push_back(peek_char());
      get();
    }
    panic(std::string(msg) + " near '" + save + "'");
  };
  for (;;) {
    if (eos())
      panic("unfinished string near <eof>");
    if (curr_is_newline())
      panic("unfinished string near '" + save + "'");
    char c = get();
    save.push_back(c);
    if (c == quote)
      break;
    if (c == '\\') {
      if (eos())
        panic("unfinished string near <eof>");
      if (curr_is_newline()) {
        incline();
        save.back() = '\n'; // keep one newline in near-token
        out.push_back('\n');
        continue;
      }
      char e = get();
      save.push_back(e);
      switch (e) {
      case 'a': out.push_back('\a'); break;
      case 'b': out.push_back('\b'); break;
      case 'f': out.push_back('\f'); break;
      case 'n': out.push_back('\n'); break;
      case 'r': out.push_back('\r'); break;
      case 't': out.push_back('\t'); break;
      case 'v': out.push_back('\v'); break;
      case '\\': out.push_back('\\'); break;
      case '\'': out.push_back('\''); break;
      case '"': out.push_back('"'); break;
      case 'x': {
        if (eos() || !std::isxdigit(static_cast<unsigned char>(peek_char())))
          escerror("hexadecimal digit expected");
        char hi = get();
        save.push_back(hi);
        if (eos() || !std::isxdigit(static_cast<unsigned char>(peek_char())))
          escerror("hexadecimal digit expected");
        char lo = get();
        save.push_back(lo);
        out.push_back(static_cast<char>((hexval(hi) << 4) | hexval(lo)));
        break;
      }
      case 'u': {
        if (peek_char() != '{')
          escerror("missing '{'");
        get();
        save.push_back('{');
        if (eos() || !std::isxdigit(static_cast<unsigned char>(peek_char())))
          escerror("hexadecimal digit expected");
        unsigned long cp = 0;
        while (std::isxdigit(static_cast<unsigned char>(peek_char()))) {
          char h = get();
          save.push_back(h);
          cp = (cp << 4) + static_cast<unsigned long>(hexval(h));
          if (cp > 0x10FFFF)
            panic(std::string("UTF-8 value too large near '") + save + "'");
        }
        if (peek_char() != '}')
          escerror("missing '}'");
        get();
        save.push_back('}');
        if (cp <= 0x7F)
          out.push_back(static_cast<char>(cp));
        else if (cp <= 0x7FF) {
          out.push_back(static_cast<char>(0xC0 | ((cp >> 6) & 0x1F)));
          out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp <= 0xFFFF) {
          out.push_back(static_cast<char>(0xE0 | ((cp >> 12) & 0x0F)));
          out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
          out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
          out.push_back(static_cast<char>(0xF0 | ((cp >> 18) & 0x07)));
          out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
          out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
          out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
        break;
      }
      case 'z':
        save.pop_back(); // drop 'z' from near-token like PUC removes from buff
        save.pop_back(); // drop '\\'
        for (;;) {
          char w = peek_char();
          if (w == ' ' || w == '\t' || w == '\f' || w == '\v')
            get();
          else if (curr_is_newline())
            incline();
          else
            break;
        }
        break;
      default:
        if (std::isdigit(static_cast<unsigned char>(e))) {
          int v = e - '0';
          int nd = 1;
          for (; nd < 3 && std::isdigit(static_cast<unsigned char>(peek_char())); ++nd) {
            char d = get();
            save.push_back(d);
            v = v * 10 + (d - '0');
          }
          if (v > 255)
            escerror("decimal escape too large");
          out.push_back(static_cast<char>(v));
        } else {
          // Do not consume the next source char into near-token (PUC stops at bad escape).
          panic(std::string("invalid escape sequence near '") + save + "'");
        }
        break;
      }
    } else {
      out.push_back(c);
    }
  }
  t.text = std::move(out);
  t.near = std::move(save);
  return t;
}

Token Lexer::lex_long_string(int level) {
  Token t;
  t.kind = TokenKind::String;
  t.line = line_;
  t.col = col_;
  // opening [ =* [ already consumed by caller partially
  std::string out;
  std::string near = "[";
  near.append(static_cast<size_t>(level), '=');
  near.push_back('[');
  // Lua 5.3: first newline after [[ is skipped (not part of the value).
  if (curr_is_newline())
    incline();
  for (;;) {
    if (eos())
      panic("unfinished long string near <eof>");
    if (curr_is_newline()) {
      incline();
      out.push_back('\n');
      near.push_back('\n');
      continue;
    }
    char c = get();
    near.push_back(c);
    if (c == ']') {
      int lvl = 0;
      size_t save = pos_;
      int save_line = line_, save_col = col_;
      size_t near_save = near.size();
      while (peek_char() == '=') {
        near.push_back(get());
        lvl++;
      }
      if (lvl == level && peek_char() == ']') {
        near.push_back(get());
        break;
      }
      pos_ = save;
      line_ = save_line;
      col_ = save_col;
      near.resize(near_save);
      out.push_back(']');
    } else {
      out.push_back(c);
    }
  }
  t.text = std::move(out);
  t.near = std::move(near);
  return t;
}

Token Lexer::ident_or_kw() {
  Token t;
  t.line = line_;
  t.col = col_;
  std::string s;
  while (std::isalnum(static_cast<unsigned char>(peek_char())) || peek_char() == '_')
    s.push_back(get());
  static const std::unordered_map<std::string, TokenKind> kws = {
      {"and", TokenKind::KwAnd},       {"break", TokenKind::KwBreak},
      {"do", TokenKind::KwDo},         {"else", TokenKind::KwElse},
      {"elseif", TokenKind::KwElseif}, {"end", TokenKind::KwEnd},
      {"false", TokenKind::KwFalse},   {"for", TokenKind::KwFor},
      {"function", TokenKind::KwFunction}, {"goto", TokenKind::KwGoto},
      {"if", TokenKind::KwIf},         {"in", TokenKind::KwIn},
      {"local", TokenKind::KwLocal},   {"nil", TokenKind::KwNil},
      {"not", TokenKind::KwNot},       {"or", TokenKind::KwOr},
      {"repeat", TokenKind::KwRepeat}, {"return", TokenKind::KwReturn},
      {"then", TokenKind::KwThen},     {"true", TokenKind::KwTrue},
      {"until", TokenKind::KwUntil},   {"while", TokenKind::KwWhile},
  };
  auto it = kws.find(s);
  t.kind = it == kws.end() ? TokenKind::Identifier : it->second;
  t.text = std::move(s);
  return t;
}

Token Lexer::lex_one() {
  skip_whitespace_and_comments();
  Token t;
  t.line = line_;
  t.col = col_;
  if (eos()) {
    t.kind = TokenKind::End;
    return t;
  }
  char c = peek_char();
  if (std::isdigit(static_cast<unsigned char>(c)) ||
      (c == '.' && std::isdigit(static_cast<unsigned char>(peek_char(1)))))
    return lex_number();
  if (c == '\'' || c == '"')
    return lex_string(c);
  if (c == '[') {
    get();
    int level = 0;
    while (peek_char() == '=') {
      get();
      level++;
    }
    if (peek_char() == '[') {
      get();
      return lex_long_string(level);
    }
    // not long string — backtrack equals handled; '[' already consumed
    if (level != 0)
      panic("invalid long string delimiter");
    t.kind = TokenKind::LBracket;
    return t;
  }
  if (std::isalpha(static_cast<unsigned char>(c)) || c == '_')
    return ident_or_kw();

  get();
  switch (c) {
  case '+': t.kind = TokenKind::Plus; break;
  case '-': t.kind = TokenKind::Minus; break;
  case '*': t.kind = TokenKind::Star; break;
  case '%': t.kind = TokenKind::Percent; break;
  case '^': t.kind = TokenKind::Caret; break;
  case '#': t.kind = TokenKind::Hash; break;
  case '&': t.kind = TokenKind::Amp; break;
  case '|': t.kind = TokenKind::Pipe; break;
  case '(': t.kind = TokenKind::LParen; break;
  case ')': t.kind = TokenKind::RParen; break;
  case '{': t.kind = TokenKind::LBrace; break;
  case '}': t.kind = TokenKind::RBrace; break;
  case ']': t.kind = TokenKind::RBracket; break;
  case ';': t.kind = TokenKind::Semi; break;
  case ',': t.kind = TokenKind::Comma; break;
  case '/':
    t.kind = match('/') ? TokenKind::SlashSlash : TokenKind::Slash;
    break;
  case '~':
    t.kind = match('=') ? TokenKind::TildeEq : TokenKind::Tilde;
    break;
  case '=':
    t.kind = match('=') ? TokenKind::EqEq : TokenKind::Eq;
    break;
  case '<':
    if (match('<'))
      t.kind = TokenKind::LtLt;
    else if (match('='))
      t.kind = TokenKind::LtEq;
    else
      t.kind = TokenKind::Lt;
    break;
  case '>':
    if (match('>'))
      t.kind = TokenKind::GtGt;
    else if (match('='))
      t.kind = TokenKind::GtEq;
    else
      t.kind = TokenKind::Gt;
    break;
  case ':':
    t.kind = match(':') ? TokenKind::ColonColon : TokenKind::Colon;
    break;
  case '.':
    if (match('.')) {
      if (match('.'))
        t.kind = TokenKind::DotDotDot;
      else
        t.kind = TokenKind::DotDot;
    } else {
      t.kind = TokenKind::Dot;
    }
    break;
  default:
    // PUC: any other byte is a single-char token (errors report near '<\N>').
    t.kind = TokenKind::Char;
    t.integer = static_cast<unsigned char>(c);
    break;
  }
  return t;
}

} // namespace luatier
