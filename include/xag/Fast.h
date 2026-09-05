#pragma once

#include "xag/Mir.h"

#include <string>

namespace xag {

struct FastResult {
  bool ran = false;
  std::string trouble;
};

// The fast interpreter: the graph is turned into flat code once, and then run
// without looking anything up again.
//
// It shares nothing with the test interpreter but the runtime, on purpose. Two
// engines that borrow from each other agree about what they borrowed, and a
// vote between them proves nothing.
FastResult runFast(const Mir &mir);

} // namespace xag
