#pragma once

#include "xag/Ast.h"
#include "xag/Check.h"

#include <memory>
#include <string>
#include <vector>

namespace xag {

// The tree with the questions already answered.
//
// Everything after the checker used to work the type of a thing out again from
// what was written. `Own.cpp` kept its own model of types built out of chains as
// strings; `MirBuild.cpp` wrote types out with `spell` and read them back with
// `takeApart`. Two answers to one question, and both of the bugs that cost a day
// were the two disagreeing — a case the checker knew about and the ownership
// pass did not, and a type written to text and parsed back against the wrong
// table.
//
// So this is the checker's answer, said once, in a tree shaped for the passes
// that read it rather than for the reader who wrote it:
//
//   - every expression carries its `Ty`, rather than a map keyed by node;
//   - a name is resolved to what declared it;
//   - a word before a bracket has already been told apart — a call, a struct
//     made where it stands, or a case of a `one-of`;
//   - `str:*hi*` and `text:'s'` are different nodes, not one node and a lookup;
//   - brackets that only group are gone, because grouping is the shape;
//   - putting something into an `or-nothing` is written down rather than worked
//     out again by whoever lowers it.
//
// It is not the middle layer. There are no basic blocks, no temporaries and no
// drops: this is still the program as a tree, which is what the ownership pass
// and expansion want to walk.

struct TypedExpr;
using TypedPtr = std::unique_ptr<TypedExpr>;

enum class TypedKind {
  Name,     // a name, resolved to what declared it
  Written,  // a value written out, with the type it was read at
  Escape,   // `\n`
  Nothing,  // the absence
  Hold,     // one child: that value, put into an `or-nothing`
  Case,     // `text:'s'` — `sum`/`which` say which, one child unless it holds none
  Made,     // `point[*1* *2*]` — a struct made here, one child per field
  Several,  // `[…]` where an item goes — the rows of a `many`
  Collect,  // the items of a `many` or a struct, gathered where they were written
  Join,     // pieces of text side by side, built into one
  Element,  // two children: a `many`, and which of its places
  Field,    // one child: what it is of; `which` says which field
  Borrow,   // `loan` / `loanmut` / `move`, one child
  Call,     // a function, a built-in; `name` is which
  Unary,    // one child
  Binary,   // two children
};

struct TypedExpr {
  TypedKind kind = TypedKind::Name;
  Span span;
  Ty type;                    // what the checker worked out this is
  std::string text;           // Name, Written, Escape, Unary/Binary's operator
  std::string name;           // Call: which one
  unsigned which = 0;         // Case: the case; Field: the field
  unsigned sum = 0;           // Case: which `one-of`
  std::vector<TypedPtr> children;
  // A call's arguments, one list per parameter, each a run of items that are
  // joined or collected by whatever they go into.
  std::vector<std::vector<TypedPtr>> args;
  // Where this came from, so a diagnostic can still point at what was written.
  const Expr *wrote = nullptr;
};

struct TypedStmt;
using TypedStmtPtr = std::unique_ptr<TypedStmt>;

struct TypedBlock {
  std::vector<TypedStmtPtr> stmts;
};

// One arm of an `if`, a `when` or a `whichever`.
struct TypedArm {
  Span span;
  bool always = false;        // an `else`, which asks nothing
  TypedPtr condition;
  std::string binds;          // `holds 'name'`, or what an `is` lends to
  Span bindsSpan;
  Ty bound;                   // what that name holds
  unsigned which = 0;         // `when` over a `one-of`: which case
  bool matchesNothing = false;
  std::string family;         // `whichever`: the word that says which kind
  TypedBlock body;
};

enum class TypedStmtKind {
  Declare, Set, Add, If, LoopRange, LoopWhile, When, Break, Give, Call, Unsafe,
};

struct TypedStmt {
  TypedStmtKind kind = TypedStmtKind::Call;
  Span span;
  Ty type;                    // Declare, LoopRange: what the name holds
  std::string name;
  Span nameSpan;
  bool changeable = false;    // `mut`
  bool keepsCounter = false;  // `perm`
  bool wrapping = false;      // a sum that comes round is meant to
  std::vector<TypedPtr> value;    // the items being assigned or given
  TypedPtr index;             // `set 'xs'[…] = …`
  // `set 'p'.x = …`, as written. Which field each of those is depends on what
  // the name holds, and the name is what the walk knows rather than what the
  // tree does — the checker records a type against a declaration, and this is
  // not one.
  std::vector<std::string> fields;
  TypedPtr from;              // LoopRange: where the count starts
  TypedPtr to;                //            and where it stops
  TypedPtr condition;         // LoopWhile, and a `when`'s subject
  std::string binds;          // `loop.while … holds 'x'`
  Span bindsSpan;
  Ty bound;
  std::vector<TypedArm> arms;
  TypedBlock body;
  TypedPtr call;
  const Stmt *wrote = nullptr;
};

enum class TypedItemKind { Function, Const, Start };

struct TypedParam {
  Span span;
  Span nameSpan;
  std::string name;
  Ty type;
  // The lifetime it was lent on, when it says one. A `Ty` says a thing is
  // borrowed; only the chain said which loan it is on, and the one rule a
  // signature can answer alone is about exactly that.
  std::string loan;
};

struct TypedItem {
  TypedItemKind kind = TypedItemKind::Start;
  Span span;
  Span nameSpan;
  std::string name;
  Ty answers;
  Span answersSpan;      // the chain, for a diagnostic about the answer
  std::string loan;      // which loan the answer is on, when it says
  std::vector<TypedParam> params;
  std::vector<TypedPtr> value;   // Const
  TypedBlock body;
  const Item *wrote = nullptr;
};

struct TypedProgram {
  std::vector<TypedItem> items;
  // Carried through so nothing after this has to read them out of the tree.
  std::vector<Shape> shapes;
  std::vector<Shape> sums;
};

struct TypedResult {
  TypedProgram program;
  std::vector<Diagnostic> diagnostics;
  bool ok() const { return !anyErrors(diagnostics); }
};

// Built from the tree that was written and what the checker worked out about it.
// Nothing is decided here that the checker did not already decide: where this
// has to choose, it is because the checker's answer was recorded against a node
// and the shape of the tree is what says which node.
TypedResult typedTree(const Source &source, const Program &program,
                      const CheckResult &checked);

// The typed tree, printed the way `xagc parse` prints the written one.
std::string printed(const TypedProgram &program);

} // namespace xag
