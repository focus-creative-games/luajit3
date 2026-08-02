#pragma once

#include "frontend/ast.hpp"
#include "frontend/lexer.hpp"

namespace lj3 {

class Parser {
public:
  explicit Parser(Lexer lex);
  std::unique_ptr<Chunk> parse_chunk();

private:
  Token peek();
  Token next();
  bool check(TokenKind k);
  bool match(TokenKind k);
  Token expect(TokenKind k, const char* msg);
  [[noreturn]] void error(const std::string& msg);

  std::unique_ptr<Block> parse_block();
  AstPtr parse_stmt();
  ExprPtr parse_expr();
  ExprPtr parse_or();
  ExprPtr parse_and();
  ExprPtr parse_compare();
  ExprPtr parse_bor();
  ExprPtr parse_bxor();
  ExprPtr parse_band();
  ExprPtr parse_shift();
  ExprPtr parse_concat();
  ExprPtr parse_add();
  ExprPtr parse_mul();
  ExprPtr parse_unary();
  ExprPtr parse_power();
  ExprPtr parse_suffix(ExprPtr e);
  ExprPtr parse_primary();
  ExprPtr parse_table();
  std::unique_ptr<ExprFunction> parse_function_body();
  std::vector<ExprPtr> parse_expr_list();

  Lexer lex_;
};

std::unique_ptr<Chunk> parse(std::string_view src, const std::string& name = "chunk");

} // namespace lj3
