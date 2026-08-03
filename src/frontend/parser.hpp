#pragma once

#include "frontend/ast.hpp"
#include "frontend/lexer.hpp"

namespace luatier {

class Parser {
public:
  explicit Parser(Lexer lex);
  std::unique_ptr<Chunk> parse_chunk();

private:
  Token peek();
  Token next();
  void unget(Token t);
  bool check(TokenKind k);
  bool match(TokenKind k);
  Token expect(TokenKind k, const char* msg);
  [[noreturn]] void error(const std::string& msg);

  void enterlevel();
  void leavelevel();
  void enterfunc(int linedefined);
  void leavefunc();
  void new_localvar(const std::string& name);
  struct LevelGuard {
    Parser& p;
    explicit LevelGuard(Parser& parser) : p(parser) { p.enterlevel(); }
    ~LevelGuard() { p.leavelevel(); }
    LevelGuard(const LevelGuard&) = delete;
    LevelGuard& operator=(const LevelGuard&) = delete;
  };

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
  std::unique_ptr<ExprFunction> parse_function_body(int defline, bool is_method = false);
  std::vector<ExprPtr> parse_expr_list();

  Lexer lex_;
  bool has_unget_ = false;
  Token unget_{};
  int nCcalls_ = 0;
  // Active locals in the current function (PUC dyd->actvar / fs->nactvar).
  int nactvar_ = 0;
  int func_line_ = 0; // linedefined of current function (0 = main)
  std::vector<int> nactvar_stack_;
  std::vector<int> func_line_stack_;
};

std::unique_ptr<Chunk> parse(std::string_view src, const std::string& name = "chunk");

} // namespace luatier
