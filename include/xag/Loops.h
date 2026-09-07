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

} // namespace xag
