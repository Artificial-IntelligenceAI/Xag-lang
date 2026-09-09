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

Block clone(const Block &block);
Branch clone(const Branch &branch);
Value clone(const Value &value);
ValueList clone(const ValueList &list);
ExprPtr clone(const ExprPtr &expr);
StmtPtr clone(const StmtPtr &stmt);

} // namespace xag
