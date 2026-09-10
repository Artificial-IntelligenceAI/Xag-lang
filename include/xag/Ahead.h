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
  std::string said;     // everything it wrote to standard output
  // And everything it wrote to standard error, kept apart. A `print` says which
  // of the two it goes to, so the two are different answers and folding them
  // into one would let a program that writes to the wrong stream still look
  // right — which is exactly the disagreement worth catching.
  std::string complained;
  // Where it said a sum came round. The build it was made by is the only one
  // with checked arithmetic in it, so this is the second opinion about an
  // overflow — the thing that makes standing a bound up more than one engine's
  // word. Each place once, in the order they happened.
  std::vector<Span> cameRound;
  std::string trouble;  // why there is no answer, when there is none
  // It stopped, the way a program stops, and where. `stopped` is the reason it
  // printed; `stoppedAt` is where it had got to, which only the build that
  // keeps track can say.
  bool stopped = false;
  std::string why;
  Span stoppedAt;
  // It reached a read and stopped there rather than reading, because the build
  // the compiler makes does that. The interpreter does the same, so the two
  // still stop in the same place and what came before still compares.
  bool wouldRead = false;
  // It reached a loop told not to be run while compiling, and stopped there.
  bool wouldTakeTime = false;
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
// Both runs answer two questions: what the program wrote, and where a sum came
// round. The second is what lets a bound be stood up rather than only dropped.
// Comparing output alone, a sum coming round in a value nothing prints leaves
// both runs silent — and two engines silent in the same way is not two engines
// agreeing, which is the shape of every hole found in this project so far.
// What the checker worked out that a run wants to know before it starts: the
// most times a counted loop goes round, and where that loop is.
struct HowLong {
  long long rounds = 0;
  Span where;
};

AheadResult ahead(const Source &source, const Mir &mir,
                  const std::vector<Diagnostic> &aboutSums,
                  const std::vector<Span> &intoPlainNames = {},
                  const Building &building = {}, HowLong howLong = {});

} // namespace xag
