#pragma once

#include "xag/Ast.h"
#include "xag/Diagnostic.h"
#include "xag/Token.h"

#include <vector>

namespace xag {

struct ParseResult {
  Program program;
  std::vector<Diagnostic> diagnostics;

  bool ok() const { return !anyErrors(diagnostics); }
};

// Parsing, like lexing, reports everything it can see rather than stopping at the
// first mistake: a broken statement is abandoned at the next `;` or `}` and the
// next one is read.
ParseResult parse(const Source &source, const std::vector<Token> &tokens);

// The tree, printed. Used by `xagc parse` and by the tests.
void print(const Program &program, std::ostream &out);

} // namespace xag
