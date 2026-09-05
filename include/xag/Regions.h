#pragma once

#include "xag/Diagnostic.h"
#include "xag/Mir.h"

#include <vector>

namespace xag {

struct RegionResult {
  std::vector<Diagnostic> diagnostics;
  bool ok() const { return diagnostics.empty(); }
};

// How long a loan lasts, and what may not happen while it does.
//
// The ownership pass answers what a signature alone can decide. This one reads
// the graph: a loan lives from where it is taken until the last place anything
// holding it is looked at, and what it borrows from may not be moved, written
// to, ended, or lent again for writing in between.
RegionResult regions(const Source &source, const Mir &mir);

} // namespace xag
