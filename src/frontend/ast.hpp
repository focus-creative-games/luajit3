#pragma once

#include "common/common.hpp"

#include <memory>
#include <string>
#include <vector>

namespace lj3 {

enum class AstKind {
  Chunk,
  Block,
  LocalDecl,
  Assign,
  If,
  While,
  Repeat,
  ForNum,
  ForIn,
  Break,
  Goto,
  Label,
  Return,
  CallStmt,
  DoBlock,
  FunctionDecl,
  LocalFunction,
  ExprNil,
  ExprBool,
  ExprInt,
  ExprFloat,
  ExprString,
  ExprVararg,
  ExprName,
  ExprIndex,
  ExprField,
  ExprBin,
  ExprUn,
  ExprCall,
  ExprMethodCall,
  ExprParen,
  ExprTable,
  ExprFunction,
};

enum class BinOp {
  Add,
  Sub,
  Mul,
  Div,
  IDiv,
  Mod,
  Pow,
  Concat,
  Band,
  Bor,
  Bxor,
  Shl,
  Shr,
  Eq,
  Ne,
  Lt,
  Le,
  Gt,
  Ge,
  And,
  Or,
};

enum class UnOp { Neg, Not, Len, Bnot };

struct AstNode {
  AstKind kind;
  int line = 1;
  virtual ~AstNode() = default;
};

using AstPtr = std::unique_ptr<AstNode>;

struct Block final : AstNode {
  std::vector<AstPtr> stmts;
  Block() { kind = AstKind::Block; }
};

struct Chunk final : AstNode {
  std::unique_ptr<Block> body;
  Chunk() { kind = AstKind::Chunk; }
};

struct Expr : AstNode {};
using ExprPtr = std::unique_ptr<Expr>;

struct ExprNil final : Expr {
  ExprNil() { kind = AstKind::ExprNil; }
};
struct ExprBool final : Expr {
  bool value = false;
  explicit ExprBool(bool v) : value(v) { kind = AstKind::ExprBool; }
};
struct ExprInt final : Expr {
  int64_t value = 0;
  explicit ExprInt(int64_t v) : value(v) { kind = AstKind::ExprInt; }
};
struct ExprFloat final : Expr {
  double value = 0;
  explicit ExprFloat(double v) : value(v) { kind = AstKind::ExprFloat; }
};
struct ExprString final : Expr {
  std::string value;
  explicit ExprString(std::string v) : value(std::move(v)) { kind = AstKind::ExprString; }
};
struct ExprVararg final : Expr {
  ExprVararg() { kind = AstKind::ExprVararg; }
};
struct ExprName final : Expr {
  std::string name;
  int sym = -1; // bound later
  explicit ExprName(std::string n) : name(std::move(n)) { kind = AstKind::ExprName; }
};
struct ExprIndex final : Expr {
  ExprPtr table;
  ExprPtr key;
  ExprIndex() { kind = AstKind::ExprIndex; }
};
struct ExprField final : Expr {
  ExprPtr table;
  std::string field;
  ExprField() { kind = AstKind::ExprField; }
};
struct ExprBin final : Expr {
  BinOp op{};
  ExprPtr lhs;
  ExprPtr rhs;
  ExprBin() { kind = AstKind::ExprBin; }
};
struct ExprUn final : Expr {
  UnOp op{};
  ExprPtr operand;
  ExprUn() { kind = AstKind::ExprUn; }
};
struct ExprCall final : Expr {
  ExprPtr callee;
  std::vector<ExprPtr> args;
  bool is_method = false;
  std::string method;
  ExprCall() { kind = AstKind::ExprCall; }
};
// Parentheses truncate multiple results: (f()) keeps only the first value.
struct ExprParen final : Expr {
  ExprPtr inner;
  ExprParen() { kind = AstKind::ExprParen; }
  explicit ExprParen(ExprPtr e) : inner(std::move(e)) { kind = AstKind::ExprParen; }
};
struct ExprTable final : Expr {
  struct Field {
    ExprPtr key; // null => array part
    ExprPtr value;
    std::string name; // for name = value
  };
  std::vector<Field> fields;
  ExprTable() { kind = AstKind::ExprTable; }
};
struct ExprFunction final : Expr {
  std::vector<std::string> params;
  bool is_vararg = false;
  std::unique_ptr<Block> body;
  int lastline = 0;
  ExprFunction() { kind = AstKind::ExprFunction; }
};

struct LocalDecl final : AstNode {
  std::vector<std::string> names;
  std::vector<ExprPtr> values;
  LocalDecl() { kind = AstKind::LocalDecl; }
};
struct Assign final : AstNode {
  std::vector<ExprPtr> vars;
  std::vector<ExprPtr> values;
  Assign() { kind = AstKind::Assign; }
};
struct IfStmt final : AstNode {
  struct Branch {
    ExprPtr cond;
    std::unique_ptr<Block> body;
    int then_line = 0;
  };
  std::vector<Branch> branches;
  std::unique_ptr<Block> else_body;
  int else_line = 0;
  int end_line = 0;
  IfStmt() { kind = AstKind::If; }
};
struct WhileStmt final : AstNode {
  ExprPtr cond;
  std::unique_ptr<Block> body;
  int end_line = 0;
  WhileStmt() { kind = AstKind::While; }
};
struct RepeatStmt final : AstNode {
  std::unique_ptr<Block> body;
  ExprPtr cond;
  RepeatStmt() { kind = AstKind::Repeat; }
};
struct ForNum final : AstNode {
  std::string name;
  ExprPtr from;
  ExprPtr to;
  ExprPtr step;
  std::unique_ptr<Block> body;
  int end_line = 0;
  ForNum() { kind = AstKind::ForNum; }
};
struct ForIn final : AstNode {
  std::vector<std::string> names;
  std::vector<ExprPtr> iters;
  std::unique_ptr<Block> body;
  int end_line = 0;
  ForIn() { kind = AstKind::ForIn; }
};
struct BreakStmt final : AstNode {
  BreakStmt() { kind = AstKind::Break; }
};
struct GotoStmt final : AstNode {
  std::string label;
  GotoStmt() { kind = AstKind::Goto; }
};
struct LabelStmt final : AstNode {
  std::string name;
  LabelStmt() { kind = AstKind::Label; }
};
struct ReturnStmt final : AstNode {
  std::vector<ExprPtr> values;
  ReturnStmt() { kind = AstKind::Return; }
};
struct CallStmt final : AstNode {
  ExprPtr call;
  CallStmt() { kind = AstKind::CallStmt; }
};
struct DoBlock final : AstNode {
  std::unique_ptr<Block> body;
  DoBlock() { kind = AstKind::DoBlock; }
};
struct FunctionDecl final : AstNode {
  std::vector<std::string> name_path;
  bool is_method = false;
  std::unique_ptr<ExprFunction> fn;
  FunctionDecl() { kind = AstKind::FunctionDecl; }
};
struct LocalFunction final : AstNode {
  std::string name;
  std::unique_ptr<ExprFunction> fn;
  LocalFunction() { kind = AstKind::LocalFunction; }
};

} // namespace lj3
