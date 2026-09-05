#pragma once

#include "xag/Ast.h"
#include "xag/Check.h"
#include "xag/Diagnostic.h"

#include <string>
#include <vector>

namespace xag {

// Types are held symbolically — an index into the body's own table rather than
// anything lowered. A body can then be copied and its types substituted, which
// is what instantiating a generic will one day be.
struct TypeRef {
  unsigned index = 0;
};

// A named slot. The first `parameters` locals of a body are its parameters, and
// local 0 is where the answer goes when there is one.
struct Local {
  unsigned id = 0;
  TypeRef type;
  std::string name; // as written, for reading the printout; "" for a temporary
  bool copies = true;
};

enum class OperandKind {
  Copy,    // read a local and leave it where it is
  Move,    // read a local and take it
  Written, // a value written in the source
};

struct Operand {
  OperandKind kind = OperandKind::Copy;
  unsigned local = 0;
  std::string written;
  TypeRef type;
};

enum class RValueKind {
  Use,    // one operand
  Binary, // two, with an operator
  Unary,  // one, with an operator
  Call,   // a callee and its arguments
  Join,   // pieces side by side, built into one
  Ref,    // a loan of a local, for reading or for writing
};

struct RValue {
  RValueKind kind = RValueKind::Use;
  std::string op;      // Binary, Unary, or "ref"/"refmut" for Ref
  std::string callee;  // Call
  unsigned local = 0;  // Ref
  std::vector<Operand> operands;
  TypeRef type;
};

enum class StatementKind {
  Assign, // place = value
  Drop,   // the local's value ends here
};

struct Statement {
  StatementKind kind = StatementKind::Assign;
  Span span;
  unsigned place = 0;
  RValue value;
};

enum class TerminatorKind {
  Goto,   // one target
  Switch, // one operand, one target per value, plus a fallback
  Return,
};

// Switch is general from the start: it carries a value per target rather than a
// true/false pair, so a decision tree can use it unchanged when there is one.
struct Terminator {
  TerminatorKind kind = TerminatorKind::Return;
  Span span;
  Operand condition;
  std::vector<std::string> values; // Switch: what `condition` is compared against
  std::vector<unsigned> targets;   // Switch: one per value, then the fallback
  bool answers = false;            // Return: whether an answer is carried
  Operand answer;
};

struct BasicBlock {
  unsigned id = 0;
  std::vector<Statement> statements;
  Terminator terminator;
};

struct Body {
  std::string name;
  unsigned parameters = 0;
  TypeRef result;
  std::vector<Local> locals;
  std::vector<BasicBlock> blocks;
  std::vector<std::string> types; // TypeRef indexes this
};

struct Mir {
  std::vector<Body> bodies;
};

struct MirResult {
  Mir mir;
  std::vector<Diagnostic> diagnostics;
  bool ok() const { return diagnostics.empty(); }
};

// Lowering reads what the checker already worked out rather than working it out
// again. Drops are placed where a scope ends; making them conditional on what
// was moved is drop elaboration, which does not exist yet.
MirResult build(const Source &source, const Program &program, const CheckResult &checked);

void print(const Mir &mir, std::ostream &out);

} // namespace xag
