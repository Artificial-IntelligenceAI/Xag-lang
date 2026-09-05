#include "xag/Diagnostic.h"

#include <algorithm>
#include <ostream>
#include <string>

namespace xag {

void renderOpening(std::ostream &out) {
  out << "Hello, I think there may be thing(s) wrong with your code. "
         "I'm sorry, if I'm wrong.\n";
}

void renderTally(std::size_t errors, std::ostream &out) {
  out << '\n' << errors << (errors == 1 ? " error.\n" : " errors.\n");
}

void render(const Source &source, const Diagnostic &diagnostic, std::ostream &out) {
  const Source::Position start = source.positionOf(diagnostic.span.begin);
  const std::string_view line = source.lineText(start.line);
  const std::string number = std::to_string(start.line);
  const std::string blank(number.size(), ' ');

  out << "\nfile: " << source.name() << ", line: " << start.line
      << ", column: " << start.column << " (" << source.name() << ':' << start.line
      << ':' << start.column << ")\n\n";

  out << diagnostic.message << "\n\n";

  // A span that runs past the end of its line is clipped to it, so the underline
  // never claims more than the reader can see.
  const unsigned lineWidth = static_cast<unsigned>(line.size());
  const unsigned from = std::min<unsigned>(start.column - 1, lineWidth);
  unsigned width = diagnostic.span.end > diagnostic.span.begin
                       ? diagnostic.span.end - diagnostic.span.begin
                       : 1;
  width = std::max(1u, std::min(width, std::max(1u, lineWidth - from)));

  out << "  " << number << " | " << line << '\n';
  out << "  " << blank << " | " << std::string(from, ' ') << std::string(width, '^')
      << ' ' << diagnostic.label << "\n\n";

  out << "Error code: " << diagnostic.code << '\n';
  if (!diagnostic.rules.empty()) {
    out << "Rule(s) broken:";
    for (const std::string &rule : diagnostic.rules)
      out << ' ' << rule;
    out << '\n';
  }
  if (!diagnostic.tips.empty()) {
    out << "Tip(s):";
    for (const std::string &tip : diagnostic.tips)
      out << ' ' << tip;
    out << '\n';
  }
}

} // namespace xag
