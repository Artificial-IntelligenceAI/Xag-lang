#pragma once

#include "xag/Source.h"

#include <iosfwd>
#include <string>
#include <vector>

namespace xag {

// A second place worth looking at, underlined beneath the first. A mistake that
// happened in one place and showed up in another has two, and pointing at only
// one of them leaves the reader to go and find the other.
struct Note {
  Span span;
  std::string label;
};

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
  std::vector<Note> notes; // shown after the first, in the order given
};

// One diagnostic, with its own file/line header and underlined source line.
void render(const Source &source, const Diagnostic &diagnostic, std::ostream &out);

// Where to say so when a diagnostic is wrong. The opening greeting hedges, and
// a hedge with nowhere to go is just a shrug.
extern const char *const kIssues;

// The greeting that opens a failing run, and the tally that closes it.
void renderOpening(std::ostream &out);
void renderTally(std::size_t errors, std::ostream &out);

} // namespace xag
