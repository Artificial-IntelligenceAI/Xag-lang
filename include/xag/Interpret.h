#pragma once

#include "xag/Mir.h"

#include <string>

namespace xag {

struct InterpretResult {
  bool ran = false;
  std::string trouble; // empty when nothing went wrong
};

// The test interpreter: it walks the graph as written, calls the runtime for
// everything a value can do, and does nothing clever anywhere. It is the
// semantics in executable form, and it is the engine to believe when the three
// of them disagree — so it is never to be optimised, and it shares no code with
// the fast interpreter beyond the runtime.
InterpretResult interpret(const Mir &mir);

} // namespace xag
