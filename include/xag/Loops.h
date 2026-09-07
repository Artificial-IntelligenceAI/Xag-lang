#pragma once

#include "xag/Mir.h"
#include "xag/Source.h"

#include <vector>

namespace xag {

// A loop taken out of the program it was written in, as a program of its own.
//
// The point is loops that sit *after* something the compiler cannot do — a
// read, most often. The program around them can never be run at compile time,
// and the loop can: what it is entered with is written down even when what the
// program is given is not.
struct Lifted {
  Mir mir;                  // the loop alone, as a program called START
  std::vector<Span> places; // where its statements were written, in the real file

  // Where it was taken from, so that what it answers can be written back.
  unsigned body = 0;                // which body of the program it belongs to
  unsigned header = 0;              // the block everything else jumps back to
  unsigned leaves = 0;              // where it goes once it is done
  std::vector<unsigned> liveOut;    // what it leaves behind that is read after
};

// Every loop in the program that can stand on its own.
//
// A loop qualifies when all of these hold. Each one is a rule about being sure
// rather than about being able:
//
//  - everything it touches is a plain number or a `bool`. Text, a `many`, a
//    struct or a borrow would have to be built up again outside the loop, and
//    handing a loop something it does not really own is how a compile-time run
//    starts freeing what a program still holds.
//  - it calls nothing. A call reaches code with its own state and its own
//    reads, and following it is following the whole program again.
//  - every value it is entered with is written down: assigned exactly once
//    anywhere outside the loop, and assigned a written value. One assignment
//    outside means no other value can reach it, without asking which paths run.
//
// What comes back runs the same way the loop runs in the program, so a sum that
// comes round in one comes round in the other.
std::vector<Lifted> loopsThatStandAlone(const Mir &mir);

// Writes the answer of every loop that has one in place of the loop, and says
// how many. For the compiler only: the interpreters are given the program as
// written, which is what leaves the oracle something to compare.
//
// LLVM folds a counted loop away by itself wherever it can see the shape of it,
// so most of these are already gone by the time it looks. What it cannot see
// through is a loop with a branch in the middle: a thousand rounds of
// `if ('i' mod *7*) == *0*` still emitted as a loop, where running it says the
// answer is 47259641. That is the rule this obeys — prove only what LLVM cannot
// know, and expose the rest.
unsigned writeInWhatTheLoopsAnswer(Mir &mir);

} // namespace xag
