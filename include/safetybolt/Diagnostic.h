#pragma once

#include "safetybolt/Source.h"

#include <iosfwd>
#include <string>
#include <vector>

namespace sb {

// Every diagnostic says the same three things, in the same order: what happened,
// which rule it broke, and what to write instead. The shape is deliberate — a
// compiler that has to guess at the tip usually did not understand the mistake.
struct Diagnostic {
  Span span;
  std::string message;
  std::vector<std::string> rules;
  std::vector<std::string> tips;
};

void render(const Source &source, const Diagnostic &diagnostic, std::ostream &out);

} // namespace sb
