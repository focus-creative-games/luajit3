#include "frontend/sema.hpp"

#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace luatier {

namespace {

struct LocalScope {
  std::string name;
};

struct Ctx {
  int loop_depth = 0;
  std::vector<LocalScope> locals;
  // Per-block label maps; innermost at back. Nested blocks may reuse names.
  std::vector<std::unordered_map<std::string, std::set<std::string>>> label_scopes;
  struct PendingGoto {
    std::string label;
    std::set<std::string> visible;
    size_t scope_depth = 0;
  };
  std::vector<PendingGoto> gotos;

  std::set<std::string> visible_locals() const {
    std::set<std::string> s;
    for (auto& l : locals)
      s.insert(l.name);
    return s;
  }

  const std::set<std::string>* find_label(const std::string& name) const {
    for (int i = static_cast<int>(label_scopes.size()) - 1; i >= 0; --i) {
      auto it = label_scopes[static_cast<size_t>(i)].find(name);
      if (it != label_scopes[static_cast<size_t>(i)].end())
        return &it->second;
    }
    return nullptr;
  }

  void check_gotos() {
    for (auto& g : gotos) {
      const std::set<std::string>* labs = find_label(g.label);
      if (!labs)
        panic("no visible label '" + g.label + "' for goto");
      for (auto& name : *labs) {
        if (!g.visible.count(name))
          panic("goto jumps into the scope of local '" + name + "'");
      }
    }
  }
};

void walk_block(Block& b, Ctx& ctx, bool allow_last_label = true, bool pop_locals = true);

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
    ctx.gotos.push_back({n.label, ctx.visible_locals(), ctx.label_scopes.size()});
    break;
  }
  case AstKind::Label:
    // Labels are registered in walk_block (need lookahead for "last label" rule).
    break;
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
    // Body locals remain visible in `until`; labels are not "last" (until
    // extends the block, so a trailing label still sits in local scopes).
    size_t before = ctx.locals.size();
    walk_block(*n.body, ctx, /*allow_last_label=*/false, /*pop_locals=*/false);
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

void walk_block(Block& b, Ctx& ctx, bool allow_last_label, bool pop_locals) {
  size_t before = ctx.locals.size();
  ctx.label_scopes.emplace_back();
  const size_t scope_idx = ctx.label_scopes.size() - 1;
  for (size_t i = 0; i < b.stmts.size(); ++i) {
    AstNode& s = *b.stmts[i];
    if (s.kind == AstKind::Label) {
      auto& n = static_cast<LabelStmt&>(s);
      auto& cur = ctx.label_scopes[scope_idx];
      if (cur.count(n.name))
        panic("label '" + n.name + "' already defined");
      // PUC: if label is the last non-void statement in the block (only labels
      // follow), gotos may target it without entering this block's locals.
      bool last = allow_last_label;
      if (last) {
        for (size_t j = i + 1; j < b.stmts.size(); ++j) {
          if (b.stmts[j]->kind != AstKind::Label) {
            last = false;
            break;
          }
        }
      }
      if (last) {
        std::set<std::string> outer;
        for (size_t k = 0; k < before; ++k)
          outer.insert(ctx.locals[k].name);
        cur[n.name] = std::move(outer);
      } else {
        cur[n.name] = ctx.visible_locals();
      }
      continue;
    }
    walk_stmt(s, ctx);
  }
  // Bind gotos from this block / nested blocks to labels defined here.
  std::vector<Ctx::PendingGoto> remain;
  const size_t scope_sz = ctx.label_scopes.size();
  auto& cur = ctx.label_scopes[scope_idx];
  for (auto& g : ctx.gotos) {
    if (g.scope_depth >= scope_sz) {
      auto it = cur.find(g.label);
      if (it != cur.end()) {
        for (auto& name : it->second) {
          if (!g.visible.count(name))
            panic("goto jumps into the scope of local '" + name + "'");
        }
        continue;
      }
    }
    remain.push_back(std::move(g));
  }
  ctx.gotos = std::move(remain);
  ctx.label_scopes.pop_back();
  if (pop_locals)
    ctx.locals.resize(before);
}

} // namespace

void sema_analyze(Chunk& chunk) {
  Ctx ctx;
  walk_block(*chunk.body, ctx);
  ctx.check_gotos();
}

} // namespace luatier
