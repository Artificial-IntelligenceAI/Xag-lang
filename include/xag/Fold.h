#pragma once

#include "xag/Diagnostic.h"
#include "xag/Mir.h"
#include "xag/Source.h"

#include <vector>

namespace xag {

struct FoldResult {
  std::vector<Diagnostic> diagnostics;
  bool ok() const { return !anyErrors(diagnostics); }
};

// Works out at build time what is already written down, and refuses what is
// written down and certainly wrong.
//
// Everything here is exact. It computes by calling the same runtime the test
// interpreter calls, rather than by working the answer out a second way — two
// implementations of wrapping are two chances to disagree, and the whole point
// of this pass is that the answer is the one the program would have got.
FoldResult fold(const Source &source, Mir &mir);

} // namespace xag
