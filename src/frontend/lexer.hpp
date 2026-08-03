#pragma once

#include "common/common.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace lj3 {

enum class TokenKind {
  End,
  Identifier,
  Number,
  Integer,
  String,
  Vararg,
  // keywords
  KwAnd,
  KwBreak,
  KwDo,
  KwElse,
  KwElseif,
  KwEnd,
  KwFalse,
  KwFor,
  KwFunction,
  KwGoto,
  KwIf,
  KwIn,
  KwLocal,
  KwNil,
  KwNot,
  KwOr,
  KwRepeat,
  KwReturn,
  KwThen,
  KwTrue,
  KwUntil,
  KwWhile,
  // symbols
  Plus,
  Minus,
  Star,
  Slash,
  Percent,
  Caret,
  Hash,
  Amp,
  Tilde,
  Pipe,
  LtLt,
  GtGt,
  SlashSlash,
  EqEq,
  TildeEq,
  LtEq,
  GtEq,
  Lt,
  Gt,
  Eq,
  LParen,
  RParen,
  LBrace,
  RBrace,
  LBracket,
  RBracket,
  ColonColon,
  Semi,
  Colon,
  Comma,
  Dot,
  DotDot,
  DotDotDot,
};

struct Token {
  TokenKind kind = TokenKind::End;
  std::string text;
  double number = 0;
  int64_t integer = 0;
  int line = 1;
  int col = 1;
};

class Lexer {
public:
  explicit Lexer(std::string_view src, std::string name = "chunk");
  Token next();
  Token peek();
  const std::string& chunk_name() const { return name_; }

private:
  Token lex_one();
  void skip_whitespace_and_comments();
  Token lex_number();
  Token lex_string(char quote);
  Token lex_long_string(int level);
  Token ident_or_kw();
  char peek_char(int n = 0) const;
  char get();
  bool match(char c);
  bool eos() const { return pos_ >= src_.size(); }
  bool curr_is_newline() const;
  void incline(); // skip \n / \r / \r\n / \n\r and bump line_

  std::string src_;
  std::string name_;
  size_t pos_ = 0;
  int line_ = 1;
  int col_ = 1;
  bool has_peek_ = false;
  Token peek_;
};

const char* token_name(TokenKind k);

} // namespace lj3
