#include "xag/Diagnostic.h"

#include <algorithm>
#include <ostream>
#include <string>
#include <unordered_map>

namespace xag {

const char *const kIssues = "https://github.com/Artificial-IntelligenceAI/Xag-lang/issues";

bool anyErrors(const std::vector<Diagnostic> &diagnostics) {
  for (const Diagnostic &one : diagnostics)
    if (one.severity == Severity::Error || one.severity == Severity::Mine)
      return true;
  return false;
}

void renderOpening(std::ostream &out) {
  out << "Hello, I think there may be thing(s) wrong with your code. "
         "I'm sorry, if I'm wrong.\n";
}

// A different greeting, because the first one is an apology for refusing and
// this is not a refusal: the code was built, and here is what was noticed.
void renderWarningOpening(std::ostream &out) {
  out << "Hello, your code built. There are thing(s) I could not work out, "
         "and here they are.\n";
}

// Neither of the other two. The first apologises for refusing in case it is
// wrong, and the second says the code built. This one says the compiler is
// wrong, which it does not have to hedge about: it watched itself disagree.
void renderMineOpening(std::ostream &out) {
  out << "This is a bug in xagc, not in your code. I ran part of your program "
         "twice\nand got two different answers, so I cannot honestly build it.\n";
}

void renderMineTally(std::size_t howMany, std::ostream &out) {
  out << '\n'
      << "I disagreed with myself in " << howMany
      << (howMany == 1 ? " place.\n" : " places.\n")
      << "Your program is very likely fine. Please tell us, and paste it in — it\n"
         "found something nothing else has: " << kIssues << '\n';
}

void renderTally(std::size_t errors, std::ostream &out) {
  out << '\n' << errors << (errors == 1 ? " error.\n" : " errors.\n");
  out << "If I am wrong about any of that, please tell me: " << kIssues << '\n';
}

void renderWarningTally(std::size_t warnings, std::ostream &out) {
  out << '\n' << warnings << (warnings == 1 ? " warning.\n" : " warnings.\n");
  out << "If I am wrong about any of that, please tell me: " << kIssues << '\n';
}

// What a follow-on reads as once it is standing under its root: its own
// sentence, joined on. The full stop comes off because the label is not the end
// of anything — another one may be underneath it.
std::string asConsequence(const Diagnostic &one) {
  std::string said = one.message;
  while (!said.empty() && (said.back() == '.' || said.back() == ' '))
    said.pop_back();
  return "and because of that, " + said;
}

std::vector<Diagnostic> foldFollowOns(std::vector<Diagnostic> diagnostics) {
  // Where each root stands, by the place it points at. A root is anything that
  // followed from nothing, which is every diagnostic that is not a consequence.
  std::unordered_map<unsigned, std::size_t> rootAt;
  for (std::size_t i = 0; i < diagnostics.size(); ++i)
    if (diagnostics[i].follows.begin == 0 && diagnostics[i].follows.end == 0)
      rootAt.emplace(diagnostics[i].span.begin, i);

  std::vector<bool> folded(diagnostics.size(), false);
  for (std::size_t i = 0; i < diagnostics.size(); ++i) {
    const Diagnostic &one = diagnostics[i];
    if (one.follows.begin == 0 && one.follows.end == 0)
      continue;
    auto found = rootAt.find(one.follows.begin);
    if (found == rootAt.end())
      continue; // Nothing to stand under. Left where it is rather than lost.
    Diagnostic &root = diagnostics[found->second];

    // The same place twice says nothing the first time did not. One typo read
    // in two passes reaches here as two consequences underlining one span.
    //
    // The whole span, not where it starts: `'q' + *1*` begins where `'q'` does,
    // and the sum being unreadable is not the same news as the name being
    // undeclared. Comparing only the beginning threw that away.
    const auto same = [](Span a, Span b) {
      return a.begin == b.begin && a.end == b.end;
    };
    bool already = same(root.span, one.span);
    for (const Note &note : root.notes)
      already = already || same(note.span, one.span);
    if (!already)
      root.notes.push_back(Note{one.span, asConsequence(one)});
    folded[i] = true;
  }

  std::vector<Diagnostic> left;
  left.reserve(diagnostics.size());
  for (std::size_t i = 0; i < diagnostics.size(); ++i)
    if (!folded[i])
      left.push_back(std::move(diagnostics[i]));
  return left;
}

namespace {

// One place, underlined, with what it is doing there. `shown` carries the line
// already on screen, so several places on one line are drawn under one copy of
// it rather than under three copies of the same thing.
void pointAt(const Source &source, Span span, const std::string &label, unsigned gutter,
             unsigned &shown, std::ostream &out) {
  const Source::Position start = source.positionOf(span.begin);
  const std::string_view line = source.lineText(start.line);
  const std::string number = std::to_string(start.line);

  // A span that runs past the end of its line is clipped to it, so the underline
  // never claims more than the reader can see.
  const unsigned lineWidth = static_cast<unsigned>(line.size());
  const unsigned from = std::min<unsigned>(start.column - 1, lineWidth);
  unsigned width = span.end > span.begin ? span.end - span.begin : 1;
  width = std::max(1u, std::min(width, std::max(1u, lineWidth - from)));

  if (start.line != shown) {
    out << "  " << std::string(gutter - number.size(), ' ') << number << " | " << line
        << '\n';
    shown = start.line;
  }
  out << "  " << std::string(gutter, ' ') << " | " << std::string(from, ' ')
      << std::string(width, '^') << ' ' << label << '\n';
}

unsigned digitsNeeded(const Source &source, const Diagnostic &diagnostic) {
  unsigned widest = static_cast<unsigned>(
      std::to_string(source.positionOf(diagnostic.span.begin).line).size());
  for (const Note &note : diagnostic.notes)
    widest = std::max<unsigned>(
        widest, static_cast<unsigned>(
                    std::to_string(source.positionOf(note.span.begin).line).size()));
  return widest;
}

} // namespace

void render(const Source &source, const Diagnostic &diagnostic, std::ostream &out) {
  // Some things are not about a place. Two engines disagreeing about a whole
  // program is one of them, and underlining the first character of the file
  // would be pointing at something for the sake of pointing.
  if (diagnostic.span.begin == 0 && diagnostic.span.end == 0) {
    out << "\nin " << source.name() << ":\n\n" << diagnostic.message << "\n\n";
    for (const std::string &what : diagnostic.rules)
      out << what << '\n';
    return;
  }

  const Source::Position start = source.positionOf(diagnostic.span.begin);

  out << "\nfile: " << source.name() << ", line: " << start.line
      << ", column: " << start.column << " (" << source.name() << ':' << start.line
      << ':' << start.column << ")\n\n";

  out << diagnostic.message << "\n\n";

  const unsigned gutter = digitsNeeded(source, diagnostic);
  unsigned shown = 0;
  pointAt(source, diagnostic.span, diagnostic.label, gutter, shown, out);
  for (const Note &note : diagnostic.notes)
    pointAt(source, note.span, note.label, gutter, shown, out);
  out << '\n';

  // A code names a rule the reader's code broke. When the compiler is the one
  // that is wrong there is no such rule, so there is no code and nothing is
  // said about rules — only what the two answers were.
  if (diagnostic.severity != Severity::Mine)
    out << "Error code: " << diagnostic.code << '\n';
  if (diagnostic.severity != Severity::Mine && !diagnostic.rules.empty()) {
    out << "Rule(s) broken:";
    for (const std::string &rule : diagnostic.rules)
      out << ' ' << rule;
    out << '\n';
  }
  if (diagnostic.severity == Severity::Mine && !diagnostic.rules.empty()) {
    for (const std::string &what : diagnostic.rules)
      out << what << '\n';
  }
  if (!diagnostic.tips.empty()) {
    out << "Tip(s):";
    for (const std::string &tip : diagnostic.tips)
      out << ' ' << tip;
    out << '\n';
  }
}

} // namespace xag
