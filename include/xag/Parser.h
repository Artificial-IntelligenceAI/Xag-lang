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
// Whether a word is one a chain reads — `mut`, `loan`, `many`, `any` and the
// rest. A unit's prefix cannot be one, or `var.t.point 'p'` could not be read:
// the chain would not know whether `t` was answering a question or naming a
// library.
bool isChainWord(std::string_view word);

// `prefixes` is every library's call name, from the manifests. A chain reads
// `t.point` as one type when `t` is one of them and as a mistake otherwise —
// so the parser has to be told, because nothing about the word says which.
ParseResult parse(const Source &source, const std::vector<Token> &tokens,
                  const std::vector<std::string> &prefixes = {});

// The tree, printed. Used by `xagc parse` and by the tests.
void print(const Program &program, std::ostream &out);

} // namespace xag
