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
NativeResult emitIr(const Mir &mir, bool optimise);
NativeResult emitObject(const Mir &mir, bool optimise, const std::string &path);

} // namespace xag
