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
// Whether the compiler is refusing, or telling. A warning is for what it cannot
// settle either way: refusing there would turn a correct program away because
// the compiler was not clever enough, and saying nothing would let a wrong
// answer through in silence. Codes are `E…` when it refuses and `W…` when it
// only says so.
// `Mine` is the third, and it is not about the reader's code at all: the two
// engines that ran their loop gave different answers, so the compiler
// contradicted itself and knows it. It stops like a refusal — a program cannot
// be handed back when the compiler holds two answers for one piece of it — but
// it is not a refusal, and saying so in the reader's voice would blame them for
// our mistake. Codes are `E…` when it refuses, `W…` when it only says so, and
// none at all for `Mine`: a code names a rule the reader's code broke, and no
// rule was broken.
enum class Severity { Error, Warning, Mine };

struct Diagnostic {
  Span span;
  std::string code;    // E0001 and up, or W0001 and up
  std::string message; // what happened, in a sentence
  std::string label = "here";
  std::vector<std::string> rules;
  std::vector<std::string> tips;
  std::vector<Note> notes; // shown after the first, in the order given
  // Last, so that every place that builds one of these by listing its parts
  // keeps working and means what it always did: a refusal.
  Severity severity = Severity::Error;
  // What this one followed from: the span of the mistake that made it happen.
  // Empty on a mistake that started by itself, which is most of them.
  //
  // A reader with one typo should be told about one typo. Everything the typo
  // broke is worth seeing — it is the size of the mistake — but seeing it as
  // five separate refusals means looking for five things to fix when there is
  // one. So a diagnostic that follows from another is folded underneath it, and
  // the count says one.
  //
  // Last, and after `severity`, so that every place that builds one of these by
  // listing its parts keeps working and means what it always did.
  Span follows{};
};

// Folds every diagnostic that followed from another underneath the one it
// followed from, and hands back what is left: the mistakes that started by
// themselves, each carrying what it broke.
//
// A follow-on whose root is not among these is kept as it is. That happens when
// the root was a warning nobody printed, or when a pass stopped before the root
// was reached, and dropping it would be losing the only thing said about it.
std::vector<Diagnostic> foldFollowOns(std::vector<Diagnostic> diagnostics);

// One diagnostic, with its own file/line header and underlined source line.
void render(const Source &source, const Diagnostic &diagnostic, std::ostream &out);

// Where to say so when a diagnostic is wrong. The opening greeting hedges, and
// a hedge with nowhere to go is just a shrug.
extern const char *const kIssues;

// Whether any of these is a refusal. A run with only warnings in it went fine.
bool anyErrors(const std::vector<Diagnostic> &diagnostics);

// The greeting that opens a failing run, and the tally that closes it.
void renderOpening(std::ostream &out);
void renderWarningOpening(std::ostream &out);
void renderTally(std::size_t errors, std::ostream &out);
void renderWarningTally(std::size_t warnings, std::ostream &out);
void renderMineOpening(std::ostream &out);
void renderMineTally(std::size_t howMany, std::ostream &out);

} // namespace xag
