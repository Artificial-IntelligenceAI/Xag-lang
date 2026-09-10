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

// What a local holds, taken apart once rather than spelled and re-read.
//
// It used to be only the spelling, and every engine pulled it apart again with
// its own `rfind("loan ")` — thirty-odd places across five files, each free to
// forget a case. Two of them forgot the same one, and a borrowed number printed
// as `true` for months while three engines agreed. Taking a type apart where it
// is made, once, is the difference between that mistake being untested and
// being unwritable.
struct MirType {
  enum class Lending { None, Read, Write };

  Lending lending = Lending::None;
  bool orNothing = false;
  // How many `many`s stand around what is held: one for `many int64`, two for
  // `many many int64`. Read as a yes-or-no everywhere that only asks whether
  // this is several at all, which is most places.
  unsigned many = 0;
  // Whether the several it holds may become more of them. A `many-growing`
  // keeps room it is not using yet, so it is laid out differently and let go of
  // differently — and growing may move every place, which is why a loan into
  // one cannot outlive a growth.
  bool grows = false;
  // What is left once the words above are off it. `named` says which struct or
  // which `one-of`, when `held` is one of those.
  Type held = Type::Unknown;
  unsigned named = 0;

  bool isLoan() const { return lending != Lending::None; }
  bool writesThrough() const { return lending == Lending::Write; }

  // The same type with the loan taken off, which is what a borrow is a borrow
  // of. Asking a `loan int64` what kind of number it is has no answer; asking
  // this does.
  MirType lent() const {
    MirType out = *this;
    out.lending = Lending::None;
    return out;
  }

  // Past the absence, and then past the `many`: what one place holds.
  MirType within() const {
    MirType out = lent();
    out.orNothing = false;
    return out;
  }

  // Two types are the same when every part of them is. Comparing spellings did
  // this before, which worked only because one type had one spelling.
  bool operator==(const MirType &other) const {
    return lending == other.lending && orNothing == other.orNothing &&
           many == other.many && held == other.held &&
           ((held != Type::Struct && held != Type::OneOf) || named == other.named);
  }
  bool operator!=(const MirType &other) const { return !(*this == other); }

  // One level in. A `many` of a `many` gives back a `many`, which is the whole
  // of what a second level means — taking them all off at once said a place of
  // `many many int64` held an `int64`, and the backend read a number out of a
  // slot holding an array.
  MirType element() const {
    MirType out = within();
    out.many = many > 0 ? many - 1 : 0;
    return out;
  }
};

// Spelled the way the middle layer prints it, and the way it was written before
// this was a structure: `loan many int64`.
std::string spell(const MirType &type);

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
  // A `one-of`: which of the things it may be this is, and what that case
  // holds. `Case` makes one — `local` says which case, and there is one operand
  // unless the case holds nothing. `Which` answers the case a value is in, as a
  // number, which is what a `when` switches on. What the case holds is read
  // with `Inside`, exactly as an `or-nothing`'s is.
  Case,
  Which,
};

struct RValue {
  RValueKind kind = RValueKind::Use;
  std::string op;      // Binary, Unary, or "loan"/"loanmut" for Ref
  std::string callee;  // Call
  unsigned local = 0;  // Ref
  std::vector<Operand> operands;
  TypeRef type;
  // Element: the place asked for was shown to be one this `many` has, so the
  // question does not need asking again while it runs. Last, so that every
  // place that lists an RValue's parts keeps meaning what it meant.
  bool settled = false;
};

enum class StatementKind {
  Assign, // place = value
  Drop,   // the local's value ends here
  Store,  // one place of a `many`: place[at] = value
  // One more place at the end of a `many-growing`. Its own kind rather than a
  // `Store` past the end, because where it goes is not written down — the
  // length is what says where, and the length is what changes.
  Grow,
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
  // The block a `no-itmt` loop jumps back to. A run the compiler is doing stops
  // when it gets here, the way it stops at a read: the loop was told not to be
  // run while compiling, and running the program around it would run it anyway.
  bool noItmt = false;
  // The block a `loop.while` jumps back to. Its ends are not written down, so
  // it may never finish — which is the one thing that stops a run the compiler
  // is doing from being allowed to take as long as it likes.
  bool mayNotFinish = false;
};

struct Body {
  std::string name;
  unsigned parameters = 0;
  TypeRef result;
  std::vector<Local> locals;
  std::vector<BasicBlock> blocks;
  std::vector<std::string> types; // TypeRef indexes this
  // The same types, taken apart. Filled where a type is made, so the two cannot
  // drift; the spellings stay until every engine reads this instead.
  std::vector<MirType> typed;
};

struct Mir {
  std::vector<Body> bodies;
  // What each struct is made of, carried through so that nothing after the
  // checker has to read it out of the tree again.
  std::vector<Shape> shapes;
  // The same fields, said the way the middle layer says types. A `Shape` holds
  // what the checker worked out, which the engines cannot read; showing a
  // struct walks its fields and has to know what each one is.
  std::vector<std::vector<MirType>> fieldTypes;
  // The `one-of` types and what each of their cases holds, the same way.
  std::vector<Shape> sums;
  std::vector<std::vector<MirType>> caseTypes;
};

struct MirResult {
  Mir mir;
  std::vector<Diagnostic> diagnostics;
  bool ok() const { return !anyErrors(diagnostics); }
};

// Lowering reads what the checker already worked out rather than working it out
// again. Drops are placed where a scope ends; making them conditional on what
// was moved is drop elaboration, which does not exist yet.
MirResult build(const Source &source, const Program &program,
               const CheckResult &checked);

// Drops arrive from lowering placed at every scope end, whether or not anything
// is still there to drop. Elaboration reads the graph and settles each one: gone
// where the value was certainly moved, kept where it certainly was not, and
// guarded by a flag where the paths disagree.
void elaborate(Mir &mir);

void print(const Mir &mir, std::ostream &out);

} // namespace xag
