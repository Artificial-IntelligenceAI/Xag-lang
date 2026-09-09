#pragma once

#include "xag/Ast.h"

namespace xag {

// A deep copy of an item, tree and all.
//
// It exists for generics. A generic is written once and built once per type it
// is called with, so that nothing after the front end ever meets a blank — and
// building it per type means having a copy of it per type. The copy is taken
// first and the blank filled in afterwards, so that this stays a plain clone
// with nothing to know about types.
//
// Everything in the tree is held by value except `Expr` and `Stmt`, which are
// behind `unique_ptr`; those are what this is really for.
Item clone(const Item &item);

// Every `any` in an item's chains, written as a type instead.
//
// Done after the clone rather than during it, so the clone stays a clone. It
// reaches a chain wherever one is written: the item's own, its parameters', and
// every declaration inside it — a `var` or a loop counter in a generic body may
// name the blank too.
//
// `spelled` is a type as it is written, so it may be `int64` or the name of a
// struct. Answers how many were filled in, which is how a caller tells a generic
// from one that only looked like it.
unsigned fillTheBlank(Item &item, std::string_view spelled);

Block clone(const Block &block);
Branch clone(const Branch &branch);
Value clone(const Value &value);
ValueList clone(const ValueList &list);
ExprPtr clone(const ExprPtr &expr);
StmtPtr clone(const StmtPtr &stmt);

} // namespace xag
