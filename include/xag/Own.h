#pragma once

#include "xag/Ast.h"
#include "xag/Check.h"
#include "xag/Diagnostic.h"
#include "xag/Typed.h"

#include <vector>

namespace xag {

struct OwnResult {
  std::vector<Diagnostic> diagnostics;
  bool ok() const { return !anyErrors(diagnostics); }
};

// Who owns what, who has lent what, and what has stopped existing.
//
// This pass does not infer regions. It checks moves, the words that spell a
// transfer, and the rule that says when a loan has to be named — which is
// decidable from the signature alone.
OwnResult own(const Source &source, const TypedProgram &program);

// The same, given the tree as written and what the checker made of it. Builds
// the typed tree and asks the one above.
OwnResult own(const Source &source, const Program &program, const CheckResult &checked);

} // namespace xag
