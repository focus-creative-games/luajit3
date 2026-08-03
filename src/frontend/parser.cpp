#include "frontend/parser.hpp"

namespace luatier {

namespace {

template <typename T, typename... Args>
std::unique_ptr<T> make_at(int line, Args&&... args) {
  auto n = std::make_unique<T>(std::forward<Args>(args)...);
  n->line = line;
  return n;
}

} // namespace

Parser::Parser(Lexer lex) : lex_(std::move(lex)) {}

Token Parser::peek() {
  if (has_unget_)
    return unget_;
  return lex_.peek();
}
Token Parser::next() {
  if (has_unget_) {
    has_unget_ = false;
    return unget_;
  }
  return lex_.next();
}
void Parser::unget(Token t) {
  if (has_unget_)
    panic("parser unget overflow");
  unget_ = t;
  has_unget_ = true;
}
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
    if (k == TokenKind::Semi) {
      next();
      continue;
    }
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
      br.then_line = expect(TokenKind::KwThen, "expected 'then'").line;
      br.body = parse_block();
      s->branches.push_back(std::move(br));
    } while (match(TokenKind::KwElseif));
    if (check(TokenKind::KwElse)) {
      s->else_line = peek().line;
      next();
      s->else_body = parse_block();
    }
    s->end_line = expect(TokenKind::KwEnd, "expected 'end'").line;
    return s;
  }
  if (match(TokenKind::KwWhile)) {
    auto s = std::make_unique<WhileStmt>();
    s->line = line;
    s->cond = parse_expr();
    expect(TokenKind::KwDo, "expected 'do'");
    s->body = parse_block();
    s->end_line = expect(TokenKind::KwEnd, "expected 'end'").line;
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
      s->end_line = expect(TokenKind::KwEnd, "expected 'end'").line;
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
    s->end_line = expect(TokenKind::KwEnd, "expected 'end'").line;
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
    s->fn = parse_function_body(s->line);
    if (s->is_method)
      s->fn->params.insert(s->fn->params.begin(), "self");
    return s;
  }
  if (match(TokenKind::KwLocal)) {
    if (match(TokenKind::KwFunction)) {
      auto s = std::make_unique<LocalFunction>();
      s->line = line;
      s->name = expect(TokenKind::Identifier, "expected name").text;
      s->fn = parse_function_body(s->line);
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

std::unique_ptr<ExprFunction> Parser::parse_function_body(int defline) {
  auto fn = std::make_unique<ExprFunction>();
  fn->line = defline;
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
  fn->lastline = expect(TokenKind::KwEnd, "expected 'end'").line;
  return fn;
}

ExprPtr Parser::parse_expr() { return parse_or(); }

ExprPtr Parser::parse_or() {
  auto e = parse_and();
  while (check(TokenKind::KwOr)) {
    int line = peek().line;
    next();
    auto n = make_at<ExprBin>(e->line > 0 ? e->line : line);
    n->op = BinOp::Or;
    n->lhs = std::move(e);
    n->rhs = parse_and();
    e = std::move(n);
  }
  return e;
}
ExprPtr Parser::parse_and() {
  auto e = parse_compare();
  while (check(TokenKind::KwAnd)) {
    int line = peek().line;
    next();
    auto n = make_at<ExprBin>(e->line > 0 ? e->line : line);
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
    int line = peek().line;
    next();
    auto n = make_at<ExprUn>(line);
    n->op = op;
    n->operand = parse_unary();
    return n;
  }
  return parse_power();
}
ExprPtr Parser::parse_power() {
  // PUC simpleexp: only NAME / '(' exp ')' / '...' go through suffixedexp.
  // Literals (nil/true/false/number/string), constructors, and function
  // definitions do not — so `a = nil\n(function()end)()` is two statements.
  const bool allow_suffix = check(TokenKind::Identifier) || check(TokenKind::LParen) ||
                            check(TokenKind::DotDotDot);
  auto e = parse_primary();
  if (allow_suffix)
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
    if (check(TokenKind::Dot)) {
      int line = peek().line;
      next();
      auto n = make_at<ExprField>(e->line > 0 ? e->line : line);
      n->table = std::move(e);
      n->field = expect(TokenKind::Identifier, "expected field").text;
      e = std::move(n);
    } else if (check(TokenKind::LBracket)) {
      int line = peek().line;
      next();
      auto n = make_at<ExprIndex>(e->line > 0 ? e->line : line);
      n->table = std::move(e);
      n->key = parse_expr();
      expect(TokenKind::RBracket, "expected ']'");
      e = std::move(n);
    } else if (check(TokenKind::Colon)) {
      int line = peek().line;
      next();
      auto n = make_at<ExprCall>(e->line > 0 ? e->line : line);
      n->is_method = true;
      n->method = expect(TokenKind::Identifier, "expected method").text;
      n->callee = std::move(e);
      if (match(TokenKind::LParen)) {
        if (!check(TokenKind::RParen))
          n->args = parse_expr_list();
        expect(TokenKind::RParen, "expected ')'");
      } else if (check(TokenKind::String)) {
        auto t = next();
        n->args.push_back(make_at<ExprString>(t.line, t.text));
      } else if (check(TokenKind::LBrace)) {
        n->args.push_back(parse_table());
      } else {
        error("expected method arguments");
      }
      e = std::move(n);
    } else if (check(TokenKind::LParen)) {
      int line = peek().line;
      next();
      auto n = make_at<ExprCall>(e->line > 0 ? e->line : line);
      n->callee = std::move(e);
      if (!check(TokenKind::RParen))
        n->args = parse_expr_list();
      expect(TokenKind::RParen, "expected ')'");
      e = std::move(n);
    } else if (check(TokenKind::String) || check(TokenKind::LBrace)) {
      auto n = make_at<ExprCall>(e->line);
      n->callee = std::move(e);
      if (check(TokenKind::String)) {
        auto t = next();
        n->args.push_back(make_at<ExprString>(t.line, t.text));
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
  if (check(TokenKind::KwNil)) {
    int line = peek().line;
    next();
    return make_at<ExprNil>(line);
  }
  if (check(TokenKind::KwTrue)) {
    int line = peek().line;
    next();
    return make_at<ExprBool>(line, true);
  }
  if (check(TokenKind::KwFalse)) {
    int line = peek().line;
    next();
    return make_at<ExprBool>(line, false);
  }
  if (check(TokenKind::Integer)) {
    auto t = next();
    return make_at<ExprInt>(t.line, t.integer);
  }
  if (check(TokenKind::Number)) {
    auto t = next();
    return make_at<ExprFloat>(t.line, t.number);
  }
  if (check(TokenKind::String)) {
    auto t = next();
    return make_at<ExprString>(t.line, t.text);
  }
  if (check(TokenKind::DotDotDot)) {
    int line = peek().line;
    next();
    return make_at<ExprVararg>(line);
  }
  if (check(TokenKind::Identifier)) {
    auto t = next();
    return make_at<ExprName>(t.line, t.text);
  }
  if (check(TokenKind::LParen)) {
    int line = peek().line;
    next();
    auto e = parse_expr();
    expect(TokenKind::RParen, "expected ')'");
    auto p = make_at<ExprParen>(line, std::move(e));
    return p;
  }
  if (check(TokenKind::LBrace))
    return parse_table();
  if (check(TokenKind::KwFunction)) {
    int fl = peek().line;
    match(TokenKind::KwFunction);
    return parse_function_body(fl);
  }
  error("unexpected symbol");
}

ExprPtr Parser::parse_table() {
  int line = peek().line;
  expect(TokenKind::LBrace, "expected '{'");
  auto tab = make_at<ExprTable>(line);
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
        // Expression starting with this name (may include .., calls, etc.)
        unget(saved);
        f.value = parse_expr();
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

} // namespace luatier
