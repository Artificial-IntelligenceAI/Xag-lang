#include "safetybolt/Diagnostic.h"

#include <ostream>
#include <string>

namespace sb {

void render(const Source &source, const Diagnostic &diagnostic, std::ostream &out) {
  const Source::Position start = source.positionOf(diagnostic.span.begin);
  const std::string_view line = source.lineText(start.line);

  out << source.name() << ':' << start.line << ':' << start.column << ": "
      << diagnostic.message << '\n';

  const std::string gutter(std::to_string(start.line).size(), ' ');
  out << gutter << " |\n";
  out << start.line << " | " << line << '\n';

  // A span that runs past the end of its line is clipped to it, so the underline
  // never claims more than the reader can see.
  unsigned width = diagnostic.span.end > diagnostic.span.begin
                       ? diagnostic.span.end - diagnostic.span.begin
                       : 1;
  const unsigned remaining =
      static_cast<unsigned>(line.size()) + 1 - std::min<unsigned>(start.column, static_cast<unsigned>(line.size()) + 1);
  width = std::max(1u, std::min(width, std::max(1u, remaining)));

  out << gutter << " | " << std::string(start.column - 1, ' ') << std::string(width, '^') << '\n';

  for (const std::string &rule : diagnostic.rules)
    out << gutter << " = rule: " << rule << '\n';
  for (const std::string &tip : diagnostic.tips)
    out << gutter << " = tip:  " << tip << '\n';
}

} // namespace sb
