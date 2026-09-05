#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace sb {

// A half-open byte range into a Source.
struct Span {
  unsigned begin = 0;
  unsigned end = 0;
};

// One file of SafetyBolt, plus the line index a diagnostic needs to point at it.
class Source {
public:
  struct Position {
    unsigned line = 1;   // 1-based
    unsigned column = 1; // 1-based, counted in bytes
  };

  Source(std::string name, std::string text);

  std::string_view name() const { return name_; }
  std::string_view text() const { return text_; }

  Position positionOf(unsigned offset) const;
  std::string_view lineText(unsigned line) const;

private:
  std::string name_;
  std::string text_;
  std::vector<unsigned> lineStarts_;
};

} // namespace sb
