#pragma once

#include "safetybolt/Diagnostic.h"
#include "safetybolt/Token.h"

#include <vector>

namespace sb {

struct LexResult {
  std::vector<Token> tokens;
  std::vector<Diagnostic> diagnostics;

  bool ok() const { return diagnostics.empty(); }
};

// Lexing never stops at the first mistake: a bad character is reported and
// stepped over, so one run reports everything it can see.
LexResult lex(const Source &source);

} // namespace sb
