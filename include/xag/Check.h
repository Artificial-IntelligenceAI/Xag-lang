#pragma once

#include "xag/Ast.h"
#include "xag/Diagnostic.h"

#include <unordered_map>
#include <vector>

namespace xag {

// The types a value can have. `Unknown` is not a type a program can write: it
// means nothing in this position has said what the value is, which is itself
// the thing a written value has to answer for.
enum class Type { Unknown, I64, Str, Bool, Nothing };

const char *name(Type type);

struct CheckResult {
  std::vector<Diagnostic> diagnostics;

  // What the checker worked out, so that nothing after it has to work the same
  // thing out again. Keyed by node, which is stable for as long as the tree is.
  std::unordered_map<const Expr *, Type> expressions;
  std::unordered_map<const Stmt *, Type> declarations;
  std::unordered_map<const Item *, Type> items;

  Type of(const Expr *e) const {
    auto found = expressions.find(e);
    return found == expressions.end() ? Type::Unknown : found->second;
  }

  bool ok() const { return diagnostics.empty(); }
};

// Names and types. Ownership is a separate pass, and does not exist yet.
CheckResult check(const Source &source, const Program &program);

} // namespace xag
