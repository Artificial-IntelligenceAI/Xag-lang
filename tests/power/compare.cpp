// The decimal unit's answers against ours, on the same questions.
//
// The guest writes each case as a coefficient and a power of ten, which is what
// a decimal *is* whatever encoding holds it — so neither side has to know the
// other's. Ours is BID, with the coefficient as an ordinary binary integer; the
// unit's is DPD, with it packed three digits to ten bits. Written this way the
// two never have to meet.
//
// A cohort is compared, not just a value: `1.10` and `1.1` are equal and are
// not the same, and an answer that lost its places would be a wrong answer.

#include "xag_runtime.h"

#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>

namespace {

int failures = 0;
int checked = 0;

XagDeci read(const std::string &coefficient, const std::string &power) {
  const std::string text = coefficient + "e" + power;
  XagDeci out = 0;
  if (!xag_deci_reads(64, text.data(), text.size(), &out)) {
    std::cerr << "could not read " << text << '\n';
    ++failures;
  }
  return out;
}

std::string spelled(XagDeci value) {
  std::FILE *sink = std::tmpfile();
  xag_set_output(sink);
  xag_print_deci(64, value);
  xag_set_output(nullptr);
  std::fflush(sink);
  std::rewind(sink);
  char buffer[256];
  const size_t got = std::fread(buffer, 1, sizeof(buffer), sink);
  std::fclose(sink);
  return std::string(buffer, got);
}

// `1757771948286951e-17` into its two halves.
bool split(const std::string &word, std::string &coefficient, std::string &power) {
  const std::size_t at = word.find('e');
  if (at == std::string::npos)
    return false;
  coefficient = word.substr(0, at);
  power = word.substr(at + 1);
  return true;
}

} // namespace

int main() {
  std::string left, op, right, equals, answer;
  while (std::cin >> left) {
    if (left == "end")
      break;
    if (!(std::cin >> op >> right >> equals >> answer))
      break;
    if (equals != "=")
      continue;

    std::string lc, lp, rc, rp, ac, ap;
    if (!split(left, lc, lp) || !split(right, rc, rp) || !split(answer, ac, ap))
      continue;

    const XagDeci a = read(lc, lp), b = read(rc, rp);
    const XagDeci unit = read(ac, ap);
    const XagDeci ours = op == "+"   ? xag_deci_add(64, a, b)
                         : op == "-" ? xag_deci_sub(64, a, b)
                         : op == "x" ? xag_deci_mul(64, a, b)
                                     : xag_deci_div(64, a, b);
    ++checked;
    if (ours != unit) {
      if (failures < 12)
        std::cout << left << ' ' << op << ' ' << right << '\n'
                  << "    the unit says " << spelled(unit)
                  << "    and we say " << spelled(ours) << '\n';
      ++failures;
    }
  }

  std::cout << checked << " case(s) against the decimal unit, " << failures
            << " disagreement(s)\n";
  return failures ? 1 : 0;
}
