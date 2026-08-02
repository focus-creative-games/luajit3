#include "frontend/parser.hpp"

namespace lj3 {

Parser::Parser(Lexer lex) : lex_(std::move(lex)) {}

Token Parser::peek() { return lex_.peek(); }
Token Parser::next() { return lex_.next(); }
bool Parser::check(TokenKind k) { return peek().kind == k; }
bool Parser::match(TokenKind k) {
  if (check(k)) {
    next();
    return true;
  }
  return false;
}

Token Parser::expect(TokenKind k, const char* msg) {
  if (!check(k))
    error(msg);
  return next();
}

void Parser::error(const std::string& msg) {
  auto t = peek();
  panic(lex_.chunk_name() + ":" + std::to_string(t.line) + ": " + msg);
}

std::unique_ptr<Chunk> parse(std::string_view src, const std::string& name) {
  Parser p(Lexer(src, name));
  return p.parse_chunk();
}

std::unique_ptr<Chunk> Parser::parse_chunk() {
  auto chunk = std::make_unique<Chunk>();
  chunk->body = parse_block();
  expect(TokenKind::End, "expected end of file");
  return chunk;
}

std::unique_ptr<Block> Parser::parse_block() {
  auto block = std::make_unique<Block>();
  for (;;) {
    auto k = peek().kind;
    if (k == TokenKind::KwEnd || k == TokenKind::KwElse || k == TokenKind::KwElseif ||
        k == TokenKind::KwUntil || k == TokenKind::End)
      break;
    if (k == TokenKind::KwReturn) {
      block->stmts.push_back(parse_stmt());
      match(TokenKind::Semi);
      break;
    }
    block->stmts.push_back(parse_stmt());
    match(TokenKind::Semi);
  }
  return block;
}

std::vector<ExprPtr> Parser::parse_expr_list() {
  std::vector<ExprPtr> list;
  list.push_back(parse_expr());
  while (match(TokenKind::Comma))
    list.push_back(parse_expr());
  return list;
}

AstPtr Parser::parse_stmt() {
  int line = peek().line;
  if (match(TokenKind::KwIf)) {
    auto s = std::make_unique<IfStmt>();
    s->line = line;
    do {
      IfStmt::Branch br;
      br.cond = parse_expr();
      expect(TokenKind::KwThen, "expected 'then'");
      br.body = parse_block();
      s->branches.push_back(std::move(br));
    } while (match(TokenKind::KwElseif));
    if (match(TokenKind::KwElse))
      s->else_body = parse_block();
    expect(TokenKind::KwEnd, "expected 'end'");
    return s;
  }
  if (match(TokenKind::KwWhile)) {
    auto s = std::make_unique<WhileStmt>();
    s->line = line;
    s->cond = parse_expr();
    expect(TokenKind::KwDo, "expected 'do'");
    s->body = parse_block();
    expect(TokenKind::KwEnd, "expected 'end'");
    return s;
  }
  if (match(TokenKind::KwRepeat)) {
    auto s = std::make_unique<RepeatStmt>();
    s->line = line;
    s->body = parse_block();
    expect(TokenKind::KwUntil, "expected 'until'");
    s->cond = parse_expr();
    return s;
  }
  if (match(TokenKind::KwFor)) {
    auto name = expect(TokenKind::Identifier, "expected name").text;
    if (match(TokenKind::Eq)) {
      auto s = std::make_unique<ForNum>();
      s->line = line;
      s->name = name;
      s->from = parse_expr();
      expect(TokenKind::Comma, "expected ','");
      s->to = parse_expr();
      if (match(TokenKind::Comma))
        s->step = parse_expr();
      expect(TokenKind::KwDo, "expected 'do'");
      s->body = parse_block();
      expect(TokenKind::KwEnd, "expected 'end'");
      return s;
    }
    auto s = std::make_unique<ForIn>();
    s->line = line;
    s->names.push_back(name);
    while (match(TokenKind::Comma))
      s->names.push_back(expect(TokenKind::Identifier, "expected name").text);
    expect(TokenKind::KwIn, "expected 'in'");
    s->iters = parse_expr_list();
    expect(TokenKind::KwDo, "expected 'do'");
    s->body = parse_block();
    expect(TokenKind::KwEnd, "expected 'end'");
    return s;
  }
  if (match(TokenKind::KwDo)) {
    auto s = std::make_unique<DoBlock>();
    s->line = line;
    s->body = parse_block();
    expect(TokenKind::KwEnd, "expected 'end'");
    return s;
  }
  if (match(TokenKind::KwBreak)) {
    auto s = std::make_unique<BreakStmt>();
    s->line = line;
    return s;
  }
  if (match(TokenKind::KwGoto)) {
    auto s = std::make_unique<GotoStmt>();
    s->line = line;
    s->label = expect(TokenKind::Identifier, "expected label").text;
    return s;
  }
  if (match(TokenKind::ColonColon)) {
    auto s = std::make_unique<LabelStmt>();
    s->line = line;
    s->name = expect(TokenKind::Identifier, "expected label").text;
    expect(TokenKind::ColonColon, "expected '::'");
    return s;
  }
  if (match(TokenKind::KwReturn)) {
    auto s = std::make_unique<ReturnStmt>();
    s->line = line;
    if (!check(TokenKind::KwEnd) && !check(TokenKind::KwElse) && !check(TokenKind::KwElseif) &&
        !check(TokenKind::KwUntil) && !check(TokenKind::Semi) && !check(TokenKind::End))
      s->values = parse_expr_list();
    return s;
  }
  if (match(TokenKind::KwFunction)) {
    auto s = std::make_unique<FunctionDecl>();
    s->line = line;
    s->name_path.push_back(expect(TokenKind::Identifier, "expected name").text);
    while (match(TokenKind::Dot))
      s->name_path.push_back(expect(TokenKind::Identifier, "expected name").text);
    if (match(TokenKind::Colon)) {
      s->is_method = true;
      s->name_path.push_back(expect(TokenKind::Identifier, "expected method").text);
    }
    s->fn = parse_function_body();
    if (s->is_method)
      s->fn->params.insert(s->fn->params.begin(), "self");
    return s;
  }
  if (match(TokenKind::KwLocal)) {
    if (match(TokenKind::KwFunction)) {
      auto s = std::make_unique<LocalFunction>();
      s->line = line;
      s->name = expect(TokenKind::Identifier, "expected name").text;
      s->fn = parse_function_body();
      return s;
    }
    auto s = std::make_unique<LocalDecl>();
    s->line = line;
    s->names.push_back(expect(TokenKind::Identifier, "expected name").text);
    while (match(TokenKind::Comma))
      s->names.push_back(expect(TokenKind::Identifier, "expected name").text);
    if (match(TokenKind::Eq))
      s->values = parse_expr_list();
    return s;
  }

  // expression statement / assignment
  auto e = parse_expr();
  if (check(TokenKind::Comma) || check(TokenKind::Eq)) {
    auto s = std::make_unique<Assign>();
    s->line = line;
    s->vars.push_back(std::move(e));
    while (match(TokenKind::Comma))
      s->vars.push_back(parse_expr());
    expect(TokenKind::Eq, "expected '='");
    s->values = parse_expr_list();
    return s;
  }
  if (e->kind != AstKind::ExprCall)
    error("unexpected expression statement");
  auto s = std::make_unique<CallStmt>();
  s->line = line;
  s->call = std::move(e);
  return s;
}

std::unique_ptr<ExprFunction> Parser::parse_function_body() {
  auto fn = std::make_unique<ExprFunction>();
  expect(TokenKind::LParen, "expected '('");
  if (!check(TokenKind::RParen)) {
    if (match(TokenKind::DotDotDot)) {
      fn->is_vararg = true;
    } else {
      fn->params.push_back(expect(TokenKind::Identifier, "expected name").text);
      while (match(TokenKind::Comma)) {
        if (match(TokenKind::DotDotDot)) {
          fn->is_vararg = true;
          break;
        }
        fn->params.push_back(expect(TokenKind::Identifier, "expected name").text);
      }
    }
  }
  expect(TokenKind::RParen, "expected ')'");
  fn->body = parse_block();
  expect(TokenKind::KwEnd, "expected 'end'");
  return fn;
}

ExprPtr Parser::parse_expr() { return parse_or(); }

ExprPtr Parser::parse_or() {
  auto e = parse_and();
  while (match(TokenKind::KwOr)) {
    auto n = std::make_unique<ExprBin>();
    n->op = BinOp::Or;
    n->lhs = std::move(e);
    n->rhs = parse_and();
    e = std::move(n);
  }
  return e;
}
ExprPtr Parser::parse_and() {
  auto e = parse_compare();
  while (match(TokenKind::KwAnd)) {
    auto n = std::make_unique<ExprBin>();
    n->op = BinOp::And;
    n->lhs = std::move(e);
    n->rhs = parse_compare();
    e = std::move(n);
  }
  return e;
}
ExprPtr Parser::parse_compare() {
  auto e = parse_bor();
  for (;;) {
    BinOp op;
    if (match(TokenKind::EqEq))
      op = BinOp::Eq;
    else if (match(TokenKind::TildeEq))
      op = BinOp::Ne;
    else if (match(TokenKind::Lt))
      op = BinOp::Lt;
    else if (match(TokenKind::LtEq))
      op = BinOp::Le;
    else if (match(TokenKind::Gt))
      op = BinOp::Gt;
    else if (match(TokenKind::GtEq))
      op = BinOp::Ge;
    else
      break;
    auto n = std::make_unique<ExprBin>();
    n->op = op;
    n->lhs = std::move(e);
    n->rhs = parse_bor();
    e = std::move(n);
  }
  return e;
}
ExprPtr Parser::parse_bor() {
  auto e = parse_bxor();
  while (match(TokenKind::Pipe)) {
    auto n = std::make_unique<ExprBin>();
    n->op = BinOp::Bor;
    n->lhs = std::move(e);
    n->rhs = parse_bxor();
    e = std::move(n);
  }
  return e;
}
ExprPtr Parser::parse_bxor() {
  auto e = parse_band();
  while (match(TokenKind::Tilde)) {
    auto n = std::make_unique<ExprBin>();
    n->op = BinOp::Bxor;
    n->lhs = std::move(e);
    n->rhs = parse_band();
    e = std::move(n);
  }
  return e;
}
ExprPtr Parser::parse_band() {
  auto e = parse_shift();
  while (match(TokenKind::Amp)) {
    auto n = std::make_unique<ExprBin>();
    n->op = BinOp::Band;
    n->lhs = std::move(e);
    n->rhs = parse_shift();
    e = std::move(n);
  }
  return e;
}
ExprPtr Parser::parse_shift() {
  auto e = parse_concat();
  for (;;) {
    BinOp op;
    if (match(TokenKind::LtLt))
      op = BinOp::Shl;
    else if (match(TokenKind::GtGt))
      op = BinOp::Shr;
    else
      break;
    auto n = std::make_unique<ExprBin>();
    n->op = op;
    n->lhs = std::move(e);
    n->rhs = parse_concat();
    e = std::move(n);
  }
  return e;
}
ExprPtr Parser::parse_concat() {
  auto e = parse_add();
  if (match(TokenKind::DotDot)) {
    auto n = std::make_unique<ExprBin>();
    n->op = BinOp::Concat;
    n->lhs = std::move(e);
    n->rhs = parse_concat(); // right assoc
    e = std::move(n);
  }
  return e;
}
ExprPtr Parser::parse_add() {
  auto e = parse_mul();
  for (;;) {
    BinOp op;
    if (match(TokenKind::Plus))
      op = BinOp::Add;
    else if (match(TokenKind::Minus))
      op = BinOp::Sub;
    else
      break;
    auto n = std::make_unique<ExprBin>();
    n->op = op;
    n->lhs = std::move(e);
    n->rhs = parse_mul();
    e = std::move(n);
  }
  return e;
}
ExprPtr Parser::parse_mul() {
  auto e = parse_unary();
  for (;;) {
    BinOp op;
    if (match(TokenKind::Star))
      op = BinOp::Mul;
    else if (match(TokenKind::Slash))
      op = BinOp::Div;
    else if (match(TokenKind::SlashSlash))
      op = BinOp::IDiv;
    else if (match(TokenKind::Percent))
      op = BinOp::Mod;
    else
      break;
    auto n = std::make_unique<ExprBin>();
    n->op = op;
    n->lhs = std::move(e);
    n->rhs = parse_unary();
    e = std::move(n);
  }
  return e;
}
ExprPtr Parser::parse_unary() {
  auto k = peek().kind;
  UnOp op = UnOp::Neg;
  bool is_un = true;
  if (k == TokenKind::KwNot)
    op = UnOp::Not;
  else if (k == TokenKind::Hash)
    op = UnOp::Len;
  else if (k == TokenKind::Minus)
    op = UnOp::Neg;
  else if (k == TokenKind::Tilde)
    op = UnOp::Bnot;
  else
    is_un = false;
  if (is_un) {
    next();
    auto n = std::make_unique<ExprUn>();
    n->op = op;
    n->operand = parse_unary();
    return n;
  }
  return parse_power();
}
ExprPtr Parser::parse_power() {
  auto e = parse_primary();
  e = parse_suffix(std::move(e));
  if (match(TokenKind::Caret)) {
    auto n = std::make_unique<ExprBin>();
    n->op = BinOp::Pow;
    n->lhs = std::move(e);
    n->rhs = parse_unary();
    return n;
  }
  return e;
}

ExprPtr Parser::parse_suffix(ExprPtr e) {
  for (;;) {
    if (match(TokenKind::Dot)) {
      auto n = std::make_unique<ExprField>();
      n->table = std::move(e);
      n->field = expect(TokenKind::Identifier, "expected field").text;
      e = std::move(n);
    } else if (match(TokenKind::LBracket)) {
      auto n = std::make_unique<ExprIndex>();
      n->table = std::move(e);
      n->key = parse_expr();
      expect(TokenKind::RBracket, "expected ']'");
      e = std::move(n);
    } else if (match(TokenKind::Colon)) {
      auto n = std::make_unique<ExprCall>();
      n->is_method = true;
      n->method = expect(TokenKind::Identifier, "expected method").text;
      n->callee = std::move(e);
      expect(TokenKind::LParen, "expected '('");
      if (!check(TokenKind::RParen))
        n->args = parse_expr_list();
      expect(TokenKind::RParen, "expected ')'");
      e = std::move(n);
    } else if (match(TokenKind::LParen)) {
      auto n = std::make_unique<ExprCall>();
      n->callee = std::move(e);
      if (!check(TokenKind::RParen))
        n->args = parse_expr_list();
      expect(TokenKind::RParen, "expected ')'");
      e = std::move(n);
    } else if (check(TokenKind::String) || check(TokenKind::LBrace)) {
      auto n = std::make_unique<ExprCall>();
      n->callee = std::move(e);
      if (check(TokenKind::String)) {
        auto t = next();
        n->args.push_back(std::make_unique<ExprString>(t.text));
      } else {
        n->args.push_back(parse_table());
      }
      e = std::move(n);
    } else {
      break;
    }
  }
  return e;
}

ExprPtr Parser::parse_primary() {
  if (match(TokenKind::KwNil))
    return std::make_unique<ExprNil>();
  if (match(TokenKind::KwTrue))
    return std::make_unique<ExprBool>(true);
  if (match(TokenKind::KwFalse))
    return std::make_unique<ExprBool>(false);
  if (check(TokenKind::Integer)) {
    auto t = next();
    return std::make_unique<ExprInt>(t.integer);
  }
  if (check(TokenKind::Number)) {
    auto t = next();
    return std::make_unique<ExprFloat>(t.number);
  }
  if (check(TokenKind::String)) {
    auto t = next();
    return std::make_unique<ExprString>(t.text);
  }
  if (match(TokenKind::DotDotDot))
    return std::make_unique<ExprVararg>();
  if (check(TokenKind::Identifier)) {
    auto t = next();
    return std::make_unique<ExprName>(t.text);
  }
  if (match(TokenKind::LParen)) {
    auto e = parse_expr();
    expect(TokenKind::RParen, "expected ')'");
    return e;
  }
  if (check(TokenKind::LBrace))
    return parse_table();
  if (match(TokenKind::KwFunction))
    return parse_function_body();
  error("unexpected token in expression");
}

ExprPtr Parser::parse_table() {
  expect(TokenKind::LBrace, "expected '{'");
  auto tab = std::make_unique<ExprTable>();
  while (!check(TokenKind::RBrace) && !check(TokenKind::End)) {
    ExprTable::Field f;
    if (match(TokenKind::LBracket)) {
      f.key = parse_expr();
      expect(TokenKind::RBracket, "expected ']'");
      expect(TokenKind::Eq, "expected '='");
      f.value = parse_expr();
    } else if (check(TokenKind::Identifier)) {
      Token saved = next();
      if (match(TokenKind::Eq)) {
        f.name = saved.text;
        f.value = parse_expr();
      } else {
        auto name = std::make_unique<ExprName>(saved.text);
        f.value = parse_suffix(std::move(name));
      }
    } else {
      f.value = parse_expr();
    }
    tab->fields.push_back(std::move(f));
    if (!match(TokenKind::Comma) && !match(TokenKind::Semi))
      break;
  }
  expect(TokenKind::RBrace, "expected '}'");
  return tab;
}

} // namespace lj3
