#pragma once

#include "xag/Diagnostic.h"
#include "xag/Mir.h"
#include "xag/Source.h"

#include <functional>
#include <string>
#include <vector>

namespace xag {

// The second answer: the program built the way anything is built and started
// like any other, and what it wrote.
//
// Not a JIT. Xag is ahead-of-time to native, and a third way of making code is
// exactly what running twice is meant to rule out — so this is the backend that
// ships, doing what it will do. Being a program of its own also means a crash
// in it is a result rather than the compiler falling over.
struct Compiled {
  bool asked = false;   // whether building was even attempted
  bool ran = false;     // it was built, started, and finished on its own
  std::string said;     // everything it wrote
  std::string trouble;  // why there is no answer, when there is none
};

// How to get that second answer. `ahead` does not know how to build a program
// and does not link LLVM; whoever calls it does.
using Building = std::function<Compiled(const Mir &)>;

struct AheadResult {
  std::vector<Diagnostic> diagnostics;
  bool ran = false;      // the program was run here at all
  bool compared = false; // and run a second way, and the two agreed
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
// It runs twice — the test interpreter, and the program built and started — and
// compares what each wrote. Agreeing is what makes an answer worth acting on.
// Disagreeing is the compiler contradicting itself, which is nobody's mistake
// but ours, and it says so rather than refusing the reader's program.
//
// With no way to build, only one engine has run, and one engine may not refuse
// anybody's program: it then drops bounds that were wrong and adds nothing.
//
// It runs nothing that reads input, and nothing whose run did not finish: both
// leave the answer partly unknown, and a bound that might still be right is
// left standing.
//
// It still only ever *drops* a bound, and never stands one up that nothing
// suspected — because the two runs are compared by what they wrote, and a sum
// coming round in a value nothing prints leaves both of them silent. Two
// engines silent in the same way is not two engines agreeing. Standing one up
// waits on the built run being able to say where a sum came round.
AheadResult ahead(const Source &source, const Mir &mir,
                  const std::vector<Diagnostic> &aboutSums,
                  const Building &building = {});

} // namespace xag
