#pragma once

#include "xag/Mir.h"

#include <string>

namespace xag {

struct NativeResult {
  std::string ir;      // the module, printed — empty unless asked for
  std::string trouble; // empty when nothing went wrong
  bool ok() const { return trouble.empty(); }
};

// LLVM stays behind this header. The front end does not link it, and neither do
// the interpreters, so a program can be checked and run without LLVM anywhere
// near it.
// Whether the built program is asked to say where a sum came round.
//
// `Yes` is only ever the build the compiler makes in order to run a program
// while compiling it. What a reader is handed is built with `No`, and a sum
// that does not fit comes round in it as a processor does it — the checks are
// how the compiler watches, not how the language behaves.
enum class Watching { No, Yes };

NativeResult emitIr(const Mir &mir, bool optimise, Watching watching = Watching::No);
NativeResult emitObject(const Mir &mir, bool optimise, const std::string &path,
                        Watching watching = Watching::No);

} // namespace xag
