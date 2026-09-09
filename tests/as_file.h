#pragma once

#include <string>

namespace xag {

// A test's program, in the shape a file has: `READ_ME`, then `PREP`, then
// `START`. The tests were written before a file had a shape, and there are some
// four hundred of them, so the shape is put on here rather than typed out four
// hundred times — what each test is about is the program, not the wrapper.
//
// Everything before `START` is what the file is made of, so it goes in `PREP`.
// A test that already writes the blocks itself is left alone.
inline std::string asFile(const std::string &text) {
  if (text.rfind("READ_ME", 0) == 0)
    return text;
  const std::string::size_type at = text.rfind("\nSTART");
  if (text.rfind("START", 0) != 0 && at == std::string::npos)
    // Nothing that runs. Many of these are about a declaration on its own, and
    // a file still has all three blocks, so the one that runs is written empty.
    return "READ_ME { }\nPREP {\n" + text + "}\nSTART { }\n";
  const std::string::size_type begins = text.rfind("START", 0) == 0 ? 0 : at + 1;
  return "READ_ME { }\nPREP {\n" + text.substr(0, begins) + "}\n" +
         text.substr(begins);
}

} // namespace xag
