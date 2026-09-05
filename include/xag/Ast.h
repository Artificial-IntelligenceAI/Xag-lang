#pragma once

#include "xag/Source.h"

#include <memory>
#include <string>
#include <vector>

namespace xag {

// One segment of a dot-chain. `isName` marks a lifetime — `ref.'life'.str` — which
// is written like every other name because it is one.
struct ChainSegment {
  Span span;
  std::string text;
  bool isName = false;
};

// A chain as written. Which segment is the kind and which the type is a question
// for whoever reads it: a declaration's first segment is its kind, a parameter's
// is already a modifier, and the last segment is always the type.
struct Chain {
  Span span;
  std::vector<ChainSegment> segments;

  const ChainSegment &type() const { return segments.back(); }
  bool startsWith(std::string_view word) const {
    return !segments.empty() && !segments.front().isName && segments.front().text == word;
  }
};

struct Expr;
using ExprPtr = std::unique_ptr<Expr>;

// A value is a list of items sitting next to each other and used in order.
struct Value {
  Span span;
  std::vector<ExprPtr> items;
};

// `[…]`, holding one value or several separated by commas.
struct ValueList {
  Span span;
  std::vector<Value> values;
};

enum class ExprKind {
  Name,    // 'greeting'                text
  Written, // *hello*                   text
  Escape,  // \n                        text
  Typed,   // str:*hello*               text is the type, one child
  Borrow,  // ref 'x' / refmut / move   text is the word, one child
  Index,   // 'xs'[*2*]                 text is the name, one child: the index
  Call,    // print.stdout[…]           path is the dotted callee, args
  Unary,   // not 'x'                   text is the word, one child
  Binary,  // 'a' + 'b'                 text is the operator, two children
  Group,   // ( … )                     one child
};

struct Expr {
  ExprKind kind = ExprKind::Name;
  Span span;
  std::string text;
  std::vector<std::string> path; // Call only
  std::vector<ExprPtr> children;
  ValueList args; // Call only
};

struct Stmt;
using StmtPtr = std::unique_ptr<Stmt>;

struct Block {
  Span span;
  std::vector<StmtPtr> stmts;
};

// One arm of an `if`. The final `else` has no condition.
//
// A condition is a single expression, not a value: `if` bounds it on the left
// and `{` on the right, so nothing has to bracket it.
struct Branch {
  Span span;
  bool hasCondition = true;
  ExprPtr condition;
  Block body;
};

enum class StmtKind {
  Declare,   // var.mut.i64 'total' = […];
  Set,       // set 'total' = […];
  If,        // if […] { } else-if […] { } else { }
  LoopRange, // loop.range.i64 'i' = [a, b] { }
  LoopWhile, // loop.while […] { }
  Break,     // break;
  Give,      // give […];
  Call,      // print.stdout[…];
};

struct Stmt {
  StmtKind kind = StmtKind::Call;
  Span span;

  Chain chain;                 // Declare, LoopRange
  Span nameSpan;               // Declare, Set, LoopRange
  std::string name;            //  "
  ExprPtr index;               // Set, when written `set 'xs'[*2*] = …`
  ValueList value;             // Declare, Set, LoopRange, Give
  ExprPtr condition;           // LoopWhile
  std::vector<Branch> branches;// If
  Block body;                  // loops
  ExprPtr call;                // Call
};

enum class ItemKind { Function, Const, Start };

struct Param {
  Span span;
  Chain chain;
  Span nameSpan;
  std::string name;
};

struct Item {
  ItemKind kind = ItemKind::Start;
  Span span;

  Chain chain;              // Function, Const
  Span nameSpan;
  std::string name;         // the function's word, or the constant's name
  std::vector<Param> params;// Function
  ValueList value;          // Const
  Block body;               // Function, Start
};

struct Program {
  std::vector<Item> items;
};

} // namespace xag
