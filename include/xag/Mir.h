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
  Join,    // pieces side by side, built into one
  Ref,     // a loan of a local, for reading or for writing
  Collect, // pieces side by side, kept as several — a `many`
  Element, // two operands: a `many`, and which of its places
  Fill,    // two operands: what to put in every place, and how many places
  Holds,   // one operand: whether something that may hold nothing holds anything
  Inside,  // one operand: what it holds, which the caller has already asked about
  Group,   // one operand per thing a struct holds, in the order it holds them
  Part,    // one operand: a struct, and `local` says which of its fields
  Taken,   // the same, but the field is handed over and left holding nothing
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
  Store,  // one place of a `many`: place[at] = value
};

struct Statement {
  StatementKind kind = StatementKind::Assign;
  Span span;
  unsigned place = 0;
  // Which part of it. A field is known where it is written, so a place is a
  // local and a path of fields into it — which is what lets one field be handed
  // over while the rest stay, and one be lent while another is written.
  std::vector<unsigned> parts;
  Operand at; // Store: which of the places
  RValue value;

  // A drop that only sometimes has anything to do — because the value was moved
  // down one path and not another — is guarded by a flag rather than duplicated
  // into every path. `conditional` says to read `flag` first.
  bool conditional = false;
  unsigned flag = 0;
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

// What this project decided, once, for every file in it. Only the settings that
// change what a program *answers* live here, because those are the ones every
// engine has to agree under.
struct Settings {
  bool wrapsOutOfRange = false; // out-of-range = "stops" (false) or "wraps"
};

struct Mir {
  std::vector<Body> bodies;
  Settings settings;
  // What each struct is made of, carried through so that nothing after the
  // checker has to read it out of the tree again.
  std::vector<Shape> shapes;
};

struct MirResult {
  Mir mir;
  std::vector<Diagnostic> diagnostics;
  bool ok() const { return diagnostics.empty(); }
};

// Lowering reads what the checker already worked out rather than working it out
// again. Drops are placed where a scope ends; making them conditional on what
// was moved is drop elaboration, which does not exist yet.
MirResult build(const Source &source, const Program &program,
               const CheckResult &checked, Settings settings = {});

// Drops arrive from lowering placed at every scope end, whether or not anything
// is still there to drop. Elaboration reads the graph and settles each one: gone
// where the value was certainly moved, kept where it certainly was not, and
// guarded by a flag where the paths disagree.
void elaborate(Mir &mir);

void print(const Mir &mir, std::ostream &out);

} // namespace xag
