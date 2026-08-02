#include "frontend/sema.hpp"

#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace lj3 {

namespace {

struct LocalScope {
  std::string name;
};

struct Ctx {
  int loop_depth = 0;
  std::vector<LocalScope> locals;
  std::unordered_map<std::string, std::set<std::string>> labels;
  struct PendingGoto {
    std::string label;
    std::set<std::string> visible;
  };
  std::vector<PendingGoto> gotos;

  std::set<std::string> visible_locals() const {
    std::set<std::string> s;
    for (auto& l : locals)
      s.insert(l.name);
    return s;
  }

  void check_gotos() {
    for (auto& g : gotos) {
      auto it = labels.find(g.label);
      if (it == labels.end())
        panic("no visible label '" + g.label + "' for goto");
      for (auto& name : it->second) {
        if (!g.visible.count(name))
          panic("goto jumps into the scope of local '" + name + "'");
      }
    }
  }
};

void walk_block(Block& b, Ctx& ctx);

void walk_expr(Expr& e, Ctx& ctx) {
  switch (e.kind) {
  case AstKind::ExprBin: {
    auto& n = static_cast<ExprBin&>(e);
    walk_expr(*n.lhs, ctx);
    walk_expr(*n.rhs, ctx);
    break;
  }
  case AstKind::ExprUn: {
    auto& n = static_cast<ExprUn&>(e);
    walk_expr(*n.operand, ctx);
    break;
  }
  case AstKind::ExprIndex: {
    auto& n = static_cast<ExprIndex&>(e);
    walk_expr(*n.table, ctx);
    walk_expr(*n.key, ctx);
    break;
  }
  case AstKind::ExprField: {
    auto& n = static_cast<ExprField&>(e);
    walk_expr(*n.table, ctx);
    break;
  }
  case AstKind::ExprCall: {
    auto& n = static_cast<ExprCall&>(e);
    walk_expr(*n.callee, ctx);
    for (auto& a : n.args)
      walk_expr(*a, ctx);
    break;
  }
  case AstKind::ExprParen: {
    auto& n = static_cast<ExprParen&>(e);
    walk_expr(*n.inner, ctx);
    break;
  }
  case AstKind::ExprTable: {
    auto& n = static_cast<ExprTable&>(e);
    for (auto& f : n.fields) {
      if (f.key)
        walk_expr(*f.key, ctx);
      walk_expr(*f.value, ctx);
    }
    break;
  }
  case AstKind::ExprFunction: {
    auto& n = static_cast<ExprFunction&>(e);
    Ctx nested;
    for (auto& p : n.params)
      nested.locals.push_back({p});
    walk_block(*n.body, nested);
    nested.check_gotos();
    break;
  }
  default:
    break;
  }
}

void walk_stmt(AstNode& s, Ctx& ctx) {
  switch (s.kind) {
  case AstKind::Break:
    if (ctx.loop_depth <= 0)
      panic("break outside loop");
    break;
  case AstKind::Goto: {
    auto& n = static_cast<GotoStmt&>(s);
    ctx.gotos.push_back({n.label, ctx.visible_locals()});
    break;
  }
  case AstKind::Label: {
    auto& n = static_cast<LabelStmt&>(s);
    if (ctx.labels.count(n.name))
      panic("label '" + n.name + "' already defined");
    ctx.labels[n.name] = ctx.visible_locals();
    break;
  }
  case AstKind::While: {
    auto& n = static_cast<WhileStmt&>(s);
    walk_expr(*n.cond, ctx);
    ctx.loop_depth++;
    walk_block(*n.body, ctx);
    ctx.loop_depth--;
    break;
  }
  case AstKind::Repeat: {
    auto& n = static_cast<RepeatStmt&>(s);
    ctx.loop_depth++;
    size_t before = ctx.locals.size();
    for (auto& st : n.body->stmts)
      walk_stmt(*st, ctx);
    walk_expr(*n.cond, ctx);
    ctx.locals.resize(before);
    ctx.loop_depth--;
    break;
  }
  case AstKind::ForNum: {
    auto& n = static_cast<ForNum&>(s);
    walk_expr(*n.from, ctx);
    walk_expr(*n.to, ctx);
    if (n.step)
      walk_expr(*n.step, ctx);
    ctx.loop_depth++;
    size_t before = ctx.locals.size();
    ctx.locals.push_back({n.name});
    walk_block(*n.body, ctx);
    ctx.locals.resize(before);
    ctx.loop_depth--;
    break;
  }
  case AstKind::ForIn: {
    auto& n = static_cast<ForIn&>(s);
    for (auto& i : n.iters)
      walk_expr(*i, ctx);
    ctx.loop_depth++;
    size_t before = ctx.locals.size();
    for (auto& name : n.names)
      ctx.locals.push_back({name});
    walk_block(*n.body, ctx);
    ctx.locals.resize(before);
    ctx.loop_depth--;
    break;
  }
  case AstKind::If: {
    auto& n = static_cast<IfStmt&>(s);
    for (auto& br : n.branches) {
      walk_expr(*br.cond, ctx);
      walk_block(*br.body, ctx);
    }
    if (n.else_body)
      walk_block(*n.else_body, ctx);
    break;
  }
  case AstKind::LocalDecl: {
    auto& n = static_cast<LocalDecl&>(s);
    for (auto& v : n.values)
      walk_expr(*v, ctx);
    for (auto& name : n.names)
      ctx.locals.push_back({name});
    break;
  }
  case AstKind::Assign: {
    auto& n = static_cast<Assign&>(s);
    for (auto& v : n.vars)
      walk_expr(*v, ctx);
    for (auto& v : n.values)
      walk_expr(*v, ctx);
    break;
  }
  case AstKind::Return: {
    auto& n = static_cast<ReturnStmt&>(s);
    for (auto& v : n.values)
      walk_expr(*v, ctx);
    break;
  }
  case AstKind::CallStmt: {
    auto& n = static_cast<CallStmt&>(s);
    walk_expr(*n.call, ctx);
    break;
  }
  case AstKind::LocalFunction: {
    auto& n = static_cast<LocalFunction&>(s);
    ctx.locals.push_back({n.name});
    Ctx nested;
    walk_block(*n.fn->body, nested);
    nested.check_gotos();
    break;
  }
  case AstKind::FunctionDecl: {
    auto& n = static_cast<FunctionDecl&>(s);
    Ctx nested;
    walk_block(*n.fn->body, nested);
    nested.check_gotos();
    break;
  }
  case AstKind::DoBlock: {
    auto& n = static_cast<DoBlock&>(s);
    walk_block(*n.body, ctx);
    break;
  }
  default:
    break;
  }
}

void walk_block(Block& b, Ctx& ctx) {
  size_t before = ctx.locals.size();
  for (auto& s : b.stmts)
    walk_stmt(*s, ctx);
  ctx.locals.resize(before);
}

} // namespace

void sema_analyze(Chunk& chunk) {
  Ctx ctx;
  walk_block(*chunk.body, ctx);
  ctx.check_gotos();
}

} // namespace lj3
