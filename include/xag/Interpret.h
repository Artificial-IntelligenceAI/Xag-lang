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
  // Where the program had got to when it stopped, and whether stopping was the
  // program's doing rather than this engine's. Running longer than the engine
  // will wait is the engine giving up and says nothing about the program.
  Span stoppedAt;
  bool theirFault = false;
  // What the whole numbers and `bool`s held when `START` finished, written the
  // way they would be written down. Only filled when asked for — a run that
  // stopped fills nothing, because it never finished holding anything.
  std::vector<std::string> endedHolding;
  // It reached a read and stopped there rather than reading. Everything before
  // that point happened; nothing after it is known, because what a program does
  // after a read depends on what it was given.
  bool wouldRead = false;
  // Every statement it actually got to. A bound about a statement the run never
  // reached is a bound nothing has answered.
  std::vector<Span> reached;
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

// The same run again, and what `START` was left holding.
//
// For folding a loop away: a loop the compiler could run is one whose answer it
// knows, and a loop LLVM cannot see through is one worth writing the answer
// into. `endedHolding` is a written value per local, or empty where the local
// held something that is not a number.
InterpretResult interpretForTheAnswer(const Mir &mir);

} // namespace xag
