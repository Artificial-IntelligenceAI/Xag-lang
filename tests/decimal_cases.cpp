// Every decimal this compiler can work out, written down beside its answer, so
// that something which is not this compiler can check it.
//
// Two engines agreeing about decimal proves nothing: all three of ours call one
// runtime, so a mistake in it is a mistake in all of them at once and no vote
// finds it. The oracle was built for the parts where the engines can differ.
// This is the other half — an implementation with no shared ancestry, asked the
// same questions. Python's `decimal` is libmpdec, written from the same IBM
// specification IEEE 754 decimal is drawn from, and derived from nothing here.
//
// This program only writes the questions and our answers. `decimal_reference.py`
// is what disagrees with them.

#include "xag_runtime.h"

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <string>

namespace {

// A generator with no library behind it, so a case number always means the same
// case however this is built.
struct Rng {
  uint64_t state;
  explicit Rng(uint64_t seed) : state(seed ? seed : 1) {}
  uint64_t next() {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
  }
  uint32_t below(uint32_t n) { return n ? static_cast<uint32_t>(next() % n) : 0; }
};

std::string spelled(uint32_t width, XagDeci value) {
  std::FILE *sink = std::tmpfile();
  xag_set_output(sink);
  xag_print_deci(width, value);
  xag_set_output(nullptr);
  std::fflush(sink);
  std::rewind(sink);
  char buffer[512];
  const size_t got = std::fread(buffer, 1, sizeof(buffer), sink);
  std::fclose(sink);
  return std::string(buffer, got);
}

// How many digits each format holds, which is what keeps a written value inside
// what its width can carry.
unsigned digitsOf(uint32_t width) {
  return width == 32 ? 7u : width == 64 ? 16u : 34u;
}

// A decimal written the way a program would write one. The shapes are chosen to
// land where decimal arithmetic is interesting: trailing zeros that a cohort has
// to keep, exponents near the ends, and the plain small numbers that any bug
// would have to survive.
std::string aValue(Rng &rng, uint32_t width) {
  const unsigned most = digitsOf(width);
  switch (rng.below(10)) {
  case 0: return "0";
  case 1: return rng.below(2) ? "infinity" : "-infinity";
  case 2: return "not-a-number";
  case 3: { // a whole number, sometimes with places that are all zero
    std::string out = std::to_string(rng.below(1000));
    if (rng.below(2))
      out += "." + std::string(1 + rng.below(3), '0');
    return (rng.below(2) ? "-" : "") + out;
  }
  case 4: { // as many digits as the width holds, which is where rounding bites
    std::string digits;
    for (unsigned i = 0; i < most; ++i)
      digits += static_cast<char>('0' + rng.below(10));
    while (digits.size() > 1 && digits[0] == '0')
      digits.erase(digits.begin());
    return (rng.below(2) ? "-" : "") + digits;
  }
  case 5: { // one digit too many, so something has to round
    std::string digits;
    for (unsigned i = 0; i < most + 1; ++i)
      digits += static_cast<char>('1' + rng.below(9));
    return (rng.below(2) ? "-" : "") + digits;
  }
  case 6: { // a power of ten at the far end of the exponent range
    const int reach = width == 32 ? 90 : width == 64 ? 380 : 6100;
    const int at = static_cast<int>(rng.below(static_cast<uint32_t>(reach * 2))) - reach;
    return (rng.below(2) ? "-" : "") + std::string("1e") + std::to_string(at);
  }
  case 7: { // a fraction, with places
    std::string out = "0.";
    for (unsigned i = 0, n = 1 + rng.below(most); i < n; ++i)
      out += static_cast<char>('0' + rng.below(10));
    return (rng.below(2) ? "-" : "") + out;
  }
  default: { // an ordinary number with a decimal point somewhere in it
    std::string digits;
    for (unsigned i = 0, n = 1 + rng.below(most); i < n; ++i)
      digits += static_cast<char>('0' + rng.below(10));
    const unsigned point = rng.below(static_cast<uint32_t>(digits.size()) + 1);
    std::string out = digits.substr(0, point) + "." + digits.substr(point);
    if (out.front() == '.')
      out = "0" + out;
    if (out.back() == '.')
      out.pop_back();
    return (rng.below(2) ? "-" : "") + out;
  }
  }
}

} // namespace

int main(int argc, char **argv) {
  const unsigned long cases = argc > 1 ? std::strtoul(argv[1], nullptr, 10) : 20000;
  const uint64_t seed = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 1;
  Rng rng(seed);

  static const char *kOperations[] = {"+", "-", "x", "/", "mod", "^", "compare"};
  static const uint32_t kWidths[] = {32, 64, 128};

  for (unsigned long i = 0; i < cases; ++i) {
    const uint32_t width = kWidths[rng.below(3)];
    const char *op = kOperations[rng.below(7)];
    const std::string left = aValue(rng, width), right = aValue(rng, width);

    XagDeci a = 0, b = 0;
    if (!xag_deci_reads(width, left.data(), left.size(), &a) ||
        !xag_deci_reads(width, right.data(), right.size(), &b)) {
      // A value this compiler will not read is not a case; the reference has
      // nothing to disagree with.
      continue;
    }

    // The question goes out before the answer is worked out, unbuffered, so a
    // case that never comes back still says which one it was.
    std::fprintf(stderr, "asking\tdeci%u\t%s %s %s\n", width, left.c_str(), op,
                 right.c_str());

    std::string answer;
    if (std::strcmp(op, "compare") == 0) {
      const int32_t order = xag_deci_compare(width, a, b);
      answer = order == -3 ? "unordered" : std::to_string(order);
    } else {
      const XagDeci got = std::strcmp(op, "+") == 0     ? xag_deci_add(width, a, b)
                          : std::strcmp(op, "-") == 0   ? xag_deci_sub(width, a, b)
                          : std::strcmp(op, "x") == 0   ? xag_deci_mul(width, a, b)
                          : std::strcmp(op, "/") == 0   ? xag_deci_div(width, a, b)
                          : std::strcmp(op, "mod") == 0 ? xag_deci_mod(width, a, b)
                                                        : xag_deci_pow(width, a, b);
      answer = spelled(width, got);
    }

    std::printf("%u\t%s\t%s\t%s\t%s\n", width, op, left.c_str(), right.c_str(),
                answer.c_str());
  }
  return 0;
}
