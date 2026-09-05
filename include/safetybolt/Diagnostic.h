#pragma once

#include "safetybolt/Source.h"

#include <iosfwd>
#include <string>
#include <vector>

namespace sb {

// A diagnostic says what happened, points at it, names the rule it broke, and
// explains why the rule exists. It does not say what to type instead: the
// reader knows their intent and the compiler does not.
struct Diagnostic {
  Span span;
  std::string code;    // E0001 and up
  std::string message; // what happened, in a sentence
  std::string label = "here";
  std::vector<std::string> rules;
  std::vector<std::string> tips;
};

// One diagnostic, with its own file/line header and underlined source line.
void render(const Source &source, const Diagnostic &diagnostic, std::ostream &out);

// The greeting that opens a failing run, and the tally that closes it.
void renderOpening(std::ostream &out);
void renderTally(std::size_t errors, std::ostream &out);

} // namespace sb
