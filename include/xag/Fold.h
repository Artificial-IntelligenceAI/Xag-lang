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

// What a program is asked to do about `fold`. Refusing is not optional: a
// program that divides by a written zero is wrong whichever engine is about to
// run it, so every engine has to be told. Rewriting is: it is an optimisation,
// and only what gets compiled needs it.
//
// The interpreters are given the program as written and the compiler is given
// what the optimiser made of it, so the oracle compares the two on every case
// it runs. Folding for all three would have every engine agreeing on the same
// wrong constant — the fold would be the one pass nothing could check.
enum class Rewriting { No, Yes };

// Works out at build time what is already written down, and refuses what is
// written down and certainly wrong.
//
// Everything here is exact. It computes by calling the same runtime the test
// interpreter calls, rather than by working the answer out a second way — two
// implementations of wrapping are two chances to disagree, and the whole point
// of this pass is that the answer is the one the program would have got.
FoldResult fold(const Source &source, Mir &mir, Rewriting rewriting);

} // namespace xag
