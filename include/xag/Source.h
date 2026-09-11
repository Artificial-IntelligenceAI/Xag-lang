#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace xag {

// A half-open byte range into a Source.
struct Span {
  unsigned begin = 0;
  unsigned end = 0;
};

// One file of Xag, plus the line index a diagnostic needs to point at it — or
// several files, held end to end, so that a program and the libraries it
// imports are one span-space and a span still says which file and which line.
//
// A library's items join the program's after they are read, and everything
// below the parser keys what it knows by span. With a source per file, a span
// from a library rendered against the program's file pointed at whatever
// happened to sit at that offset there — a type error on line 4 of the library
// was shown as line 4 of `main.xag`. One source, several files, is the fix.
class Source {
public:
  struct Position {
    unsigned line = 1;   // 1-based, within the file
    unsigned column = 1; // 1-based, counted in bytes
    unsigned file = 0;   // which of the files held, for `nameOf`
  };

  Source(std::string name, std::string text);

  // Adds another file after the ones held. Answers where its text begins, so
  // its tokens can be read from there and their spans shifted to match.
  unsigned append(std::string name, const std::string &text);

  // The first file's name — what this source was made with.
  std::string_view name() const { return files_.front().name; }
  std::string_view nameOf(const Position &at) const { return files_[at.file].name; }
  std::string_view text() const { return text_; }
  // Where each file's text starts and stops in the whole.
  unsigned fileBegin(unsigned file) const { return files_[file].begin; }
  unsigned fileEnd(unsigned file) const { return files_[file].end; }
  unsigned files() const { return static_cast<unsigned>(files_.size()); }

  Position positionOf(unsigned offset) const;
  std::string_view lineText(const Position &at) const;

private:
  struct File {
    std::string name;
    unsigned begin = 0, end = 0;
    unsigned firstLine = 0; // index into lineStarts_ of this file's line 1
  };
  std::string text_;
  std::vector<File> files_;
  std::vector<unsigned> lineStarts_;

  void indexLines(unsigned from);
};

} // namespace xag
