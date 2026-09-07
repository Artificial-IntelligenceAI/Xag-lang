#pragma once

#include "xag/Diagnostic.h"
#include "xag/Mir.h"
#include "xag/Source.h"

#include <vector>

namespace xag {

struct AheadResult {
  std::vector<Diagnostic> diagnostics;
  // Whether the program was run here. False means nothing was learned, and the
  // bounds stand exactly as they were.
  bool ran = false;
  bool ok() const { return !anyErrors(diagnostics); }
};

// Runs the program before the program is run, to find out what it does rather
// than how far it could go.
//
// The checker's bounds only ever say *at most*, and at most is sometimes wrong
// in the direction that refuses a working program: a loop adding `'i' / *50*`
// a hundred times is bounded at 200 and reaches 52, and an `int8` was refused
// for the difference. Running the loop answers exactly.
//
// It only ever *drops* a bound, never adds one. One engine has run, and one
// engine is not enough to refuse a program on — the second run and the
// comparison come next, and until then a wrong answer here can only let
// something through, which is what already happens whenever a bound gives up.
//
// It runs nothing that reads input, and nothing whose run did not finish: both
// leave the answer partly unknown, and a bound that might still be right is
// left standing.
AheadResult ahead(const Source &source, const Mir &mir,
                  const std::vector<Diagnostic> &aboutSums);

} // namespace xag
