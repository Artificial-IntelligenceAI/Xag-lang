#pragma once

#include "xag/Mir.h"

#include <string>
#include <vector>

namespace xag {

struct InterpretResult {
  bool ran = false;
  std::string trouble; // empty when nothing went wrong
  // Where a sum came round, when the run was asked to watch for it. Empty
  // otherwise, and empty is not the same as "none" unless it was asked.
  std::vector<Span> cameRound;
};

// The test interpreter: it walks the graph as written, calls the runtime for
// everything a value can do, and does nothing clever anywhere. It is the
// semantics in executable form, and it is the engine to believe when the three
// of them disagree — so it is never to be optimised, and it shares no code with
// the fast interpreter beyond the runtime.
InterpretResult interpret(const Mir &mir);

// The same run, noticing where a sum came round instead of only letting it.
//
// It computes exactly what the other one computes — a sum that does not fit
// comes round as a processor does it, watched or not — so this cannot change
// what a program means, and the three engines go on agreeing. What it adds is
// the place, which is the whole use of running a loop before the program does.
//
// Only `+`, `-` and `x` are watched: those are the three the language says are
// added, subtracted or multiplied, and they are the three a sum is built from.
InterpretResult interpretWatching(const Mir &mir);

} // namespace xag
