// The same runtime, built two ways, asked the same questions.
//
// The guest runs it with `XAG_DECIMAL_HARDWARE`: its arithmetic is the decimal
// unit's instructions and its numbers are laid out the way the unit lays them
// out. This runs the software build. Both read the same text and both write
// their answer, so what is compared is what a program would actually see —
// cohorts and all, because `1.10` and `1.1` are equal and are not the same.

#include "xag_runtime.h"

#include <cstdio>
#include <cstring>
#include <iostream>
#include <map>
#include <string>

namespace {

std::string spelled(uint32_t width, XagDeci value) {
  std::FILE *sink = std::tmpfile();
  xag_set_output(sink);
  xag_print_deci(width, value);
  xag_set_output(nullptr);
  std::fflush(sink);
  std::rewind(sink);
  char buffer[256];
  const size_t got = std::fread(buffer, 1, sizeof(buffer), sink);
  std::fclose(sink);
  return std::string(buffer, got);
}

} // namespace

int main() {
  unsigned width = 0;
  std::string left, op, right, equals, theirs;
  unsigned checked = 0, failures = 0;
  std::map<std::string, unsigned> tally;

  while (std::cin >> width) {
    if (!(std::cin >> left >> op >> right >> equals >> theirs))
      break;
    if (equals != "=")
      continue;

    XagDeci a = 0, b = 0;
    if (!xag_deci_reads(width, left.data(), left.size(), &a) ||
        !xag_deci_reads(width, right.data(), right.size(), &b))
      continue;

    const XagDeci ours = op == "+"   ? xag_deci_add(width, a, b)
                         : op == "-" ? xag_deci_sub(width, a, b)
                         : op == "x" ? xag_deci_mul(width, a, b)
                                     : xag_deci_div(width, a, b);
    ++checked;
    const std::string said = spelled(width, ours);
    if (said != theirs) {
      ++failures;
      tally["deci" + std::to_string(width) + " " + op] += 1;
      if (failures <= 10)
        std::cout << "deci" << width << ": " << left << ' ' << op << ' ' << right
                  << "\n    the unit says " << theirs << "\n    and we say   " << said
                  << '\n';
    }
  }

  if (!tally.empty()) {
    std::cout << "\nwhere they disagree:\n";
    for (const auto &[what, count] : tally)
      std::printf("  %6u  %s\n", count, what.c_str());
  }
  std::cout << '\n' << checked << " case(s) through the decimal unit, " << failures
            << " disagreement(s)\n";
  return failures ? 1 : 0;
}
