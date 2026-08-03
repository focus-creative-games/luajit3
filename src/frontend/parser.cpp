#include "frontend/parser.hpp"

#include "common/common.hpp"
#include "vm/ldebug.hpp"

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

void Parser::enterlevel() {
  ++nCcalls_;
  if (nCcalls_ > LUAI_MAXCCALLS)
    error("too many C levels");
}

void Parser::leavelevel() {
  if (nCcalls_ > 0)
    --nCcalls_;
}

void Parser::enterfunc(int linedefined) {
  nactvar_stack_.push_back(nactvar_);
  func_line_stack_.push_back(func_line_);
  nactvar_ = 0;
  func_line_ = linedefined;
}

void Parser::leavefunc() {
  if (!nactvar_stack_.empty()) {
    nactvar_ = nactvar_stack_.back();
    nactvar_stack_.pop_back();
  } else {
    nactvar_ = 0;
  }
  if (!func_line_stack_.empty()) {
    func_line_ = func_line_stack_.back();
    func_line_stack_.pop_back();
  } else {
    func_line_ = 0;
  }
}

void Parser::new_localvar(const std::string& /*name*/) {
  ++nactvar_;
  if (nactvar_ > MAXVARS) {
    int line = peek().line;
    std::string where = (func_line_ == 0)
                            ? "main function"
                            : ("function at line " + std::to_string(func_line_));
    panic(format_chunkid(lex_.chunk_name()) + ":" + std::to_string(line) +
          ": too many local variables (limit is " + std::to_string(MAXVARS) + ") in " +
          where);
  }
}

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
  panic(format_chunkid(lex_.chunk_name()) + ":" + std::to_string(t.line) + ": " + msg +
        " near " + token_to_near(t));
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
  LevelGuard lg(*this);
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
    s->fn = parse_function_body(s->line, s->is_method);
    return s;
  }
  if (match(TokenKind::KwLocal)) {
    if (match(TokenKind::KwFunction)) {
      auto s = std::make_unique<LocalFunction>();
      s->line = line;
      s->name = expect(TokenKind::Identifier, "expected name").text;
      new_localvar(s->name);
      s->fn = parse_function_body(s->line, false);
      return s;
    }
    auto s = std::make_unique<LocalDecl>();
    s->line = line;
    s->names.push_back(expect(TokenKind::Identifier, "expected name").text);
    new_localvar(s->names.back());
    while (match(TokenKind::Comma)) {
      s->names.push_back(expect(TokenKind::Identifier, "expected name").text);
      new_localvar(s->names.back());
    }
    if (match(TokenKind::Eq))
      s->values = parse_expr_list();
    return s;
  }

  // PUC exprstat/primaryexp: only NAME or '(' may start a statement expression.
  if (!check(TokenKind::Identifier) && !check(TokenKind::LParen))
    error("unexpected symbol");
  auto e = parse_suffix(parse_primary());
  if (check(TokenKind::Comma) || check(TokenKind::Eq)) {
    auto s = std::make_unique<Assign>();
    s->line = line;
    s->vars.push_back(std::move(e));
    int nvars = 1;
    while (match(TokenKind::Comma)) {
      // PUC assignment(): checklimit(nvars + nCcalls, LUAI_MAXCCALLS, "C levels")
      if (nvars + nCcalls_ > LUAI_MAXCCALLS)
        error("too many C levels");
      ++nvars;
      // LHS is suffixedexp, not a full expression (PUC).
      if (!check(TokenKind::Identifier) && !check(TokenKind::LParen))
        error("unexpected symbol");
      s->vars.push_back(parse_suffix(parse_primary()));
    }
    expect(TokenKind::Eq, "expected '='");
    s->values = parse_expr_list();
    return s;
  }
  if (e->kind != AstKind::ExprCall)
    error("syntax error");
  auto s = std::make_unique<CallStmt>();
  s->line = line;
  s->call = std::move(e);
  return s;
}

std::unique_ptr<ExprFunction> Parser::parse_function_body(int defline, bool is_method) {
  enterfunc(defline);
  auto fn = std::make_unique<ExprFunction>();
  fn->line = defline;
  if (is_method) {
    fn->params.push_back("self");
    new_localvar("self");
  }
  expect(TokenKind::LParen, "expected '('");
  if (!check(TokenKind::RParen)) {
    if (match(TokenKind::DotDotDot)) {
      fn->is_vararg = true;
    } else {
      fn->params.push_back(expect(TokenKind::Identifier, "expected name").text);
      new_localvar(fn->params.back());
      while (match(TokenKind::Comma)) {
        if (match(TokenKind::DotDotDot)) {
          fn->is_vararg = true;
          break;
        }
        fn->params.push_back(expect(TokenKind::Identifier, "expected name").text);
        new_localvar(fn->params.back());
      }
    }
  }
  expect(TokenKind::RParen, "expected ')'");
  fn->body = parse_block();
  fn->lastline = expect(TokenKind::KwEnd, "expected 'end'").line;
  leavefunc();
  return fn;
}

ExprPtr Parser::parse_expr() {
  LevelGuard lg(*this);
  return parse_or();
}

ExprPtr Parser::parse_or() {
  auto e = parse_and();
  while (check(TokenKind::KwOr)) {
    int line = peek().line;
    next();
    auto n = make_at<ExprBin>(line);
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
    auto n = make_at<ExprBin>(line);
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
    int line = peek().line;
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
    auto n = make_at<ExprBin>(line);
    n->op = op;
    n->lhs = std::move(e);
    n->rhs = parse_bor();
    e = std::move(n);
  }
  return e;
}
ExprPtr Parser::parse_bor() {
  auto e = parse_bxor();
  while (check(TokenKind::Pipe)) {
    int line = peek().line;
    next();
    auto n = make_at<ExprBin>(line);
    n->op = BinOp::Bor;
    n->lhs = std::move(e);
    n->rhs = parse_bxor();
    e = std::move(n);
  }
  return e;
}
ExprPtr Parser::parse_bxor() {
  auto e = parse_band();
  while (check(TokenKind::Tilde)) {
    int line = peek().line;
    next();
    auto n = make_at<ExprBin>(line);
    n->op = BinOp::Bxor;
    n->lhs = std::move(e);
    n->rhs = parse_band();
    e = std::move(n);
  }
  return e;
}
ExprPtr Parser::parse_band() {
  auto e = parse_shift();
  while (check(TokenKind::Amp)) {
    int line = peek().line;
    next();
    auto n = make_at<ExprBin>(line);
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
    int line = peek().line;
    BinOp op;
    if (match(TokenKind::LtLt))
      op = BinOp::Shl;
    else if (match(TokenKind::GtGt))
      op = BinOp::Shr;
    else
      break;
    auto n = make_at<ExprBin>(line);
    n->op = op;
    n->lhs = std::move(e);
    n->rhs = parse_concat();
    e = std::move(n);
  }
  return e;
}
ExprPtr Parser::parse_concat() {
  auto e = parse_add();
  if (check(TokenKind::DotDot)) {
    LevelGuard lg(*this);
    int line = peek().line;
    next();
    auto n = make_at<ExprBin>(line);
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
    int line = peek().line;
    BinOp op;
    if (match(TokenKind::Plus))
      op = BinOp::Add;
    else if (match(TokenKind::Minus))
      op = BinOp::Sub;
    else
      break;
    auto n = make_at<ExprBin>(line);
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
    int line = peek().line;
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
    auto n = make_at<ExprBin>(line);
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
    LevelGuard lg(*this);
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
  if (check(TokenKind::Caret)) {
    LevelGuard lg(*this);
    int line = peek().line;
    next();
    auto n = make_at<ExprBin>(line);
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
  if (!check(TokenKind::RBrace)) {
    error("'}' expected (to close '{' at line " + std::to_string(line) + ")");
  }
  next(); // consume '}'
  return tab;
}

} // namespace luatier
