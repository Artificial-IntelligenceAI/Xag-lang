#pragma once

#include "xag/Ast.h"
#include "xag/Diagnostic.h"

#include <vector>

namespace xag {

// The types a value can have. `Unknown` is not a type a program can write: it
// means nothing in this position has said what the value is, which is itself
// the thing a written value has to answer for.
enum class Type { Unknown, I64, Str, Bool, Nothing };

const char *name(Type type);

struct CheckResult {
  std::vector<Diagnostic> diagnostics;
  bool ok() const { return diagnostics.empty(); }
};

// Names and types. Ownership is a separate pass, and does not exist yet.
CheckResult check(const Source &source, const Program &program);

} // namespace xag
