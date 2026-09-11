#include "xag/Source.h"

#include <algorithm>

namespace xag {

Source::Source(std::string name, std::string text) : text_(std::move(text)) {
  files_.push_back(File{std::move(name), 0, static_cast<unsigned>(text_.size()), 0});
  lineStarts_.push_back(0);
  indexLines(0);
}

void Source::indexLines(unsigned from) {
  for (unsigned i = from; i < text_.size(); ++i)
    if (text_[i] == '\n')
      lineStarts_.push_back(i + 1);
}

unsigned Source::append(std::string name, const std::string &text) {
  // A newline between files, so the last line of one and the first of the next
  // are never one line — and so an unclosed thing at the end of one file is
  // still reported in that file.
  if (!text_.empty() && text_.back() != '\n') {
    text_.push_back('\n');
    lineStarts_.push_back(static_cast<unsigned>(text_.size()));
  }
  const unsigned begin = static_cast<unsigned>(text_.size());
  files_.push_back(File{std::move(name), begin, begin + static_cast<unsigned>(text.size()),
                        static_cast<unsigned>(lineStarts_.size() - 1)});
  // The new file's first line starts where its text does.
  if (lineStarts_.back() != begin)
    lineStarts_.push_back(begin);
  files_.back().firstLine = static_cast<unsigned>(lineStarts_.size() - 1);
  text_ += text;
  files_.back().end = static_cast<unsigned>(text_.size());
  indexLines(begin);
  return begin;
}

Source::Position Source::positionOf(unsigned offset) const {
  offset = std::min<unsigned>(offset, static_cast<unsigned>(text_.size()));
  unsigned file = 0;
  for (unsigned i = 0; i < files_.size(); ++i)
    if (offset >= files_[i].begin)
      file = i;
  // The first line start strictly greater than offset belongs to the next line.
  auto after = std::upper_bound(lineStarts_.begin(), lineStarts_.end(), offset);
  const unsigned index = static_cast<unsigned>(after - lineStarts_.begin()) - 1;
  const unsigned start = lineStarts_[index];
  return Position{index - files_[file].firstLine + 1, offset - start + 1, file};
}

std::string_view Source::lineText(const Position &at) const {
  if (at.file >= files_.size() || at.line == 0)
    return {};
  const unsigned index = files_[at.file].firstLine + at.line - 1;
  if (index >= lineStarts_.size())
    return {};
  const unsigned start = lineStarts_[index];
  unsigned stop = index + 1 < lineStarts_.size() ? lineStarts_[index + 1]
                                                 : static_cast<unsigned>(text_.size());
  stop = std::min(stop, files_[at.file].end);
  while (stop > start && (text_[stop - 1] == '\n' || text_[stop - 1] == '\r'))
    --stop;
  return std::string_view(text_).substr(start, stop - start);
}

} // namespace xag
