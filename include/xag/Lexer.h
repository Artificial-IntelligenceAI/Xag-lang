#pragma once

#include "xag/Diagnostic.h"
#include "xag/Token.h"

#include <vector>

namespace xag {

struct LexResult {
  std::vector<Token> tokens;
  std::vector<Diagnostic> diagnostics;

  bool ok() const { return !anyErrors(diagnostics); }
};

// Lexing never stops at the first mistake: a bad character is reported and
// stepped over, so one run reports everything it can see.
LexResult lex(const Source &source);
// One file's slice of a source holding several. Spans come back shifted to
// where the slice sits in the whole.
LexResult lex(const Source &source, unsigned begin, unsigned end);

} // namespace xag
