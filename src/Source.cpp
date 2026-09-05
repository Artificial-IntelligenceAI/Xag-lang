#include "xag/Source.h"

#include <algorithm>

namespace xag {

Source::Source(std::string name, std::string text)
    : name_(std::move(name)), text_(std::move(text)) {
  lineStarts_.push_back(0);
  for (unsigned i = 0; i < text_.size(); ++i)
    if (text_[i] == '\n')
      lineStarts_.push_back(i + 1);
}

Source::Position Source::positionOf(unsigned offset) const {
  offset = std::min<unsigned>(offset, static_cast<unsigned>(text_.size()));
  // The first line start strictly greater than offset belongs to the next line.
  auto after = std::upper_bound(lineStarts_.begin(), lineStarts_.end(), offset);
  unsigned line = static_cast<unsigned>(after - lineStarts_.begin());
  unsigned start = lineStarts_[line - 1];
  return Position{line, offset - start + 1};
}

std::string_view Source::lineText(unsigned line) const {
  if (line == 0 || line > lineStarts_.size())
    return {};
  unsigned start = lineStarts_[line - 1];
  unsigned stop = line < lineStarts_.size() ? lineStarts_[line] : static_cast<unsigned>(text_.size());
  while (stop > start && (text_[stop - 1] == '\n' || text_[stop - 1] == '\r'))
    --stop;
  return std::string_view(text_).substr(start, stop - start);
}

} // namespace xag
