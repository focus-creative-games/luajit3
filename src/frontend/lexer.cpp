#include "frontend/lexer.hpp"

#include <cctype>
#include <unordered_map>

namespace lj3 {

const char* token_name(TokenKind k) {
  switch (k) {
  case TokenKind::End: return "end-of-file";
  case TokenKind::Identifier: return "identifier";
  case TokenKind::Number: return "number";
  case TokenKind::Integer: return "integer";
  case TokenKind::String: return "string";
  default: return "token";
  }
}

Lexer::Lexer(std::string_view src, std::string name) : src_(src), name_(std::move(name)) {}

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
  if (c == '\n') {
    line_++;
    col_ = 1;
  } else {
    col_++;
  }
  return c;
}

bool Lexer::match(char c) {
  if (peek_char() == c) {
    get();
    return true;
  }
  return false;
}

void Lexer::skip_whitespace_and_comments() {
  for (;;) {
    char c = peek_char();
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v') {
      get();
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
            char d = get();
            if (d == '\0')
              panic("unfinished long comment");
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
      while (peek_char() && peek_char() != '\n')
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
    if (s.find('.') == std::string::npos && s.find('e') == std::string::npos &&
        s.find('E') == std::string::npos && s.find('p') == std::string::npos &&
        s.find('P') == std::string::npos) {
      size_t idx = 0;
      t.integer = static_cast<int64_t>(std::stoll(s, &idx, 0));
      t.kind = TokenKind::Integer;
    } else {
      t.number = std::stod(s);
      t.kind = TokenKind::Number;
    }
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
  get(); // quote
  std::string out;
  for (;;) {
    char c = get();
    if (c == '\0' || c == '\n')
      panic("unfinished string");
    if (c == quote)
      break;
    if (c == '\\') {
      char e = get();
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
      case '\n': out.push_back('\n'); break;
      default: out.push_back(e); break;
      }
    } else {
      out.push_back(c);
    }
  }
  t.text = std::move(out);
  return t;
}

Token Lexer::lex_long_string(int level) {
  Token t;
  t.kind = TokenKind::String;
  t.line = line_;
  t.col = col_;
  // opening [ =* [ already consumed by caller partially
  std::string out;
  if (peek_char() == '\n')
    get();
  for (;;) {
    char c = get();
    if (c == '\0')
      panic("unfinished long string");
    if (c == ']') {
      int lvl = 0;
      size_t save = pos_;
      int save_line = line_, save_col = col_;
      while (peek_char() == '=') {
        get();
        lvl++;
      }
      if (lvl == level && peek_char() == ']') {
        get();
        break;
      }
      pos_ = save;
      line_ = save_line;
      col_ = save_col;
      out.push_back(']');
    } else {
      out.push_back(c);
    }
  }
  t.text = std::move(out);
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
  char c = peek_char();
  if (c == '\0') {
    t.kind = TokenKind::End;
    return t;
  }
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
    panic(std::string("unexpected character: ") + c);
  }
  return t;
}

} // namespace lj3
