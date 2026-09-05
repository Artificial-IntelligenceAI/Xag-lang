// The runtime is the one thing three engines agreeing cannot vouch for: they
// all call it, so a wrong answer here is wrong identically everywhere and the
// vote is unanimous. These are its own tests, and they are the only ones it has.

#include "xag_runtime.h"

#include "unicode_cases.h"

#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {

int failures = 0;

#define CHECK(cond)                                                                      \
  do {                                                                                   \
    if (!(cond)) {                                                                       \
      std::cerr << "FAIL " << __FILE__ << ':' << __LINE__ << ": " #cond "\n";            \
      ++failures;                                                                        \
    }                                                                                    \
  } while (false)

XagStr of(const std::string &text) {
  XagStr out{nullptr, 0, 0};
  xag_str_from(&out, text.data(), text.size());
  return out;
}

std::string read(const XagStr &text) {
  return std::string(text.bytes ? text.bytes : "", text.length);
}

void textIsCopiedAndFreed() {
  const int64_t before = xag_live_allocations();
  XagStr hello = of("hello");
  CHECK(read(hello) == "hello");
  CHECK(xag_live_allocations() == before + 1);
  xag_str_drop(&hello);
  CHECK(xag_live_allocations() == before);
  CHECK(hello.bytes == nullptr);
  // Dropping what holds nothing is not a second drop.
  xag_str_drop(&hello);
  CHECK(xag_live_allocations() == before);
}

void joiningBuildsSomethingNew() {
  const int64_t before = xag_live_allocations();
  XagStr a = of("Hello, ");
  XagStr b = of("world");
  XagStr c = of("!");
  const XagStr pieces[3] = {a, b, c};
  XagStr joined{nullptr, 0, 0};
  xag_str_join(&joined, pieces, 3);
  CHECK(read(joined) == "Hello, world!");
  // The pieces are read, not taken: they are still there afterwards.
  CHECK(read(a) == "Hello, ");
  for (XagStr *each : {&a, &b, &c, &joined})
    xag_str_drop(each);
  CHECK(xag_live_allocations() == before);
}

void textGrowsWhereItStands() {
  const int64_t before = xag_live_allocations();
  XagStr text = of("hi");
  XagStr bang = of("!");
  xag_str_push(&text, &bang);
  CHECK(read(text) == "hi!");
  xag_str_drop(&text);
  xag_str_drop(&bang);
  CHECK(xag_live_allocations() == before);
}

// Unicode publishes the cases as well as the rules, so the claim is checkable.
void countingAgreesWithUnicodeItself() {
  unsigned wrong = 0;
  for (unsigned i = 0; i < kClusterCaseCount; ++i) {
    const ClusterCase &one = kClusterCases[i];
    XagStr text{nullptr, 0, 0};
    xag_str_from(&text, one.bytes, one.length);
    const int64_t got = xag_str_count(&text);
    if (got != one.clusters) {
      if (wrong < 5)
        std::cerr << "FAIL cluster case " << one.points << ": got " << got << ", Unicode says "
                  << one.clusters << '\n';
      ++wrong;
    }
    xag_str_drop(&text);
  }
  if (wrong) {
    std::cerr << "FAIL " << wrong << " of " << kClusterCaseCount
              << " Unicode conformance cases\n";
    ++failures;
  } else {
    std::cout << "all " << kClusterCaseCount << " Unicode cluster cases agree\n";
  }
}

void countingCountsWhatAPersonWouldCount() {
  struct Case {
    const char *text;
    int64_t clusters;
  };
  const Case cases[] = {
      {"", 0},
      {"abc", 3},
      {"café", 4},                       // é is one scalar in NFC
      {"cafe\xCC\x81", 4},               // and e + combining acute is still one
      {"🧑‍🧑‍🧒‍🧒", 1}, // seven scalars, one thing on the page
      {"👍🏽", 1},                        // a thumb and a skin tone
      {"🇹🇭", 1},                        // a flag is two halves
      {"🇹🇭🇯🇵", 2},                      // and two flags are four
      {"a\r\nb", 3},                     // a carriage return and a line feed break once
  };
  for (const Case &one : cases) {
    XagStr text = of(one.text);
    const int64_t got = xag_str_count(&text);
    if (got != one.clusters) {
      std::cerr << "FAIL count(\"" << one.text << "\") = " << got << ", wanted "
                << one.clusters << '\n';
      ++failures;
    }
    xag_str_drop(&text);
  }
}

void arithmeticIsWrittenOnce() {
  // `division = "truncated"`: toward zero, remainder follows the dividend.
  CHECK(xag_int_div(-7, 2, 64, 1) == -3);
  CHECK(xag_int_mod(-7, 2, 64, 1) == -1);
  CHECK(xag_int_div(7, -2, 64, 1) == -3);
  CHECK(xag_int_mod(7, -2, 64, 1) == 1);

  // Unsigned division is unsigned all the way down.
  CHECK(xag_int_div(255, 2, 8, 0) == 127);
  CHECK(xag_int_mod(255, 4, 8, 0) == 3);

  // The one quotient that does not fit, wrapped like every other.
  CHECK(xag_int_div(INT64_MIN, -1, 64, 1) == INT64_MIN);
  CHECK(xag_int_mod(INT64_MIN, -1, 64, 1) == 0);

  // By squaring, in one place.
  CHECK(xag_int_pow(2, 10, 64, 1) == 1024);
  CHECK(xag_int_pow(2, 0, 64, 1) == 1);
  CHECK(xag_int_pow(-2, 3, 64, 1) == -8);
  // And it wraps at the width it was asked for, not at the carrier's.
  CHECK(xag_int_pow(2, 10, 8, 0) == 0);
}

void everySizeIsTheSizeItSays() {
  // Wrapping happens at the written width, not at the width of the carrier.
  CHECK(xag_int_fit(127 + 1, 8, 1) == -128);
  CHECK(xag_int_fit(255 + 1, 8, 0) == 0);
  CHECK(xag_int_fit(-1, 8, 0) == 255);
  CHECK(xag_int_fit(-1, 16, 1) == -1);
  CHECK(xag_int_fit(INT64_MAX, 64, 1) == INT64_MAX);
  CHECK(xag_int_fit((XagInt)INT64_MAX + 1, 64, 1) == INT64_MIN);
  CHECK(xag_int_fit(65535, 16, 1) == -1);
  CHECK(xag_int_fit(65535, 32, 1) == 65535);
  // 128 is the carrier's own width, so nothing is cut.
  CHECK(xag_int_fit(-5, 128, 1) == -5);
}

std::string spelled(XagBin128 value) {
  std::FILE *sink = std::tmpfile();
  xag_set_output(sink);
  xag_print_bin128(value);
  xag_set_output(nullptr);
  std::fflush(sink);
  std::rewind(sink);
  char buffer[256];
  const size_t got = std::fread(buffer, 1, sizeof(buffer), sink);
  std::fclose(sink);
  return std::string(buffer, got);
}

XagBin128 read128(const char *text) {
  XagBin128 out = 0;
  if (!xag_bin128_reads(text, std::strlen(text), &out)) {
    std::cerr << "FAIL could not read " << text << '\n';
    ++failures;
  }
  return out;
}

void binary128IsWrittenOutInSoftware() {
  // The whole reason the type exists: 113 bits of significand hold what 53
  // cannot, and the difference is visible rather than theoretical.
  const XagBin128 big = read128("1e30");
  const XagBin128 more = xag_bin128_add(big, read128("1"));
  CHECK(spelled(more) == "1.000000000000000000000000000001e+30");
  CHECK(spelled(xag_bin128_sub(more, big)) == "1");
  CHECK(1e30 + 1.0 == 1e30); // which a double cannot do

  CHECK(spelled(xag_bin128_add(read128("1.5"), read128("2.25"))) == "3.75");
  CHECK(spelled(xag_bin128_mul(xag_bin128_div(read128("2"), read128("3")),
                               read128("3"))) == "2");
  CHECK(spelled(read128("0.1")) == "0.1");

  // Nothing stops, here as in every other `bin`.
  CHECK(spelled(xag_bin128_div(read128("1"), read128("0"))) == "infinity");
  CHECK(spelled(xag_bin128_div(read128("-1"), read128("0"))) == "-infinity");
  CHECK(spelled(xag_bin128_div(read128("0"), read128("0"))) == "not-a-number");
  CHECK(spelled(xag_bin128_mul(read128("1e4000"), read128("1e1000"))) == "infinity");

  // A not-a-number is ordered against nothing at all.
  CHECK(xag_bin128_compare(read128("not-a-number"), read128("1")) == -3);
  CHECK(xag_bin128_compare(read128("1"), read128("3")) == -1);
  CHECK(xag_bin128_compare(read128("3"), read128("1")) == 1);
  CHECK(xag_bin128_compare(read128("-0"), read128("0")) == 0);

  // Every double survives the trip out and back exactly.
  const double tries[] = {0.0, 1.0, -1.0, 0.1, 1e300, 1e-300, 3.141592653589793,
                          2.2250738585072014e-308, 5e-324, 1.7976931348623157e308};
  for (double one : tries)
    CHECK(xag_bin128_to_double(xag_bin128_from_double(one)) == one);

  // And what is printed can be read back, which is what shortest means here.
  for (const char *text : {"0.1", "1", "-2.5", "1e300", "1e-300", "12345.678",
                           "1.000000000000000000000000000001e+30"}) {
    const XagBin128 value = read128(text);
    const std::string said = spelled(value);
    XagBin128 back = 0;
    CHECK(xag_bin128_reads(said.data(), said.size(), &back) == 1);
    CHECK(back == value);
  }
}

std::string spelledDeci(uint32_t width, XagDeci value) {
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

XagDeci readDeci(uint32_t width, const char *text) {
  XagDeci out = 0;
  if (!xag_deci_reads(width, text, std::strlen(text), &out)) {
    std::cerr << "FAIL could not read " << text << '\n';
    ++failures;
  }
  return out;
}

void decimalCountsInTens() {
  // The reason the type exists, in one line.
  CHECK(spelledDeci(64, xag_deci_add(64, readDeci(64, "0.1"), readDeci(64, "0.2"))) ==
        "0.3");
  CHECK(0.1 + 0.2 != 0.3); // which binary cannot say

  // A cohort is kept: `1.10` and `1.1` are equal and are not the same.
  CHECK(spelledDeci(64, readDeci(64, "1.10")) == "1.10");
  CHECK(spelledDeci(64, readDeci(64, "1.1")) == "1.1");
  CHECK(xag_deci_compare(64, readDeci(64, "1.10"), readDeci(64, "1.1")) == 0);

  // Addition keeps the smaller exponent of the two, so places survive a sum.
  CHECK(spelledDeci(64, xag_deci_add(64, readDeci(64, "1.10"), readDeci(64, "2.00"))) ==
        "3.10");
  CHECK(spelledDeci(64, xag_deci_add(64, readDeci(64, "1.1"), readDeci(64, "2.0"))) ==
        "3.1");
  // Multiplication takes the sum of them.
  CHECK(spelledDeci(64, xag_deci_mul(64, readDeci(64, "2.5"), readDeci(64, "4"))) ==
        "10.0");
  // An exact quotient is written where the standard prefers it.
  CHECK(spelledDeci(64, xag_deci_div(64, readDeci(64, "1"), readDeci(64, "8"))) == "0.125");
  CHECK(spelledDeci(64, xag_deci_div(64, readDeci(64, "10"), readDeci(64, "2"))) == "5");
  CHECK(spelledDeci(64, xag_deci_div(64, readDeci(64, "1"), readDeci(64, "3"))) ==
        "0.3333333333333333");

  // Each width holds the digits it says it holds.
  CHECK(spelledDeci(32, readDeci(32, "1234567")) == "1234567");
  CHECK(spelledDeci(32, readDeci(32, "12345678")) == "1.234568e+7");
  CHECK(spelledDeci(128, readDeci(128, "1234567890123456789012345678901234")) ==
        "1234567890123456789012345678901234");

  // Nothing stops, as in every other format that has infinities.
  CHECK(spelledDeci(64, xag_deci_div(64, readDeci(64, "1"), readDeci(64, "0"))) ==
        "infinity");
  CHECK(spelledDeci(64, xag_deci_div(64, readDeci(64, "0"), readDeci(64, "0"))) ==
        "not-a-number");
  CHECK(xag_deci_compare(64, readDeci(64, "not-a-number"), readDeci(64, "1")) == -3);

  // And what is printed reads back as the very same thing, places included.
  for (uint32_t width : {32u, 64u, 128u})
    for (const char *text : {"0", "1", "-2.5", "1.10", "0.001", "1.5e+30", "1.5e-30"}) {
      const XagDeci value = readDeci(width, text);
      const std::string said = spelledDeci(width, value);
      XagDeci back = 0;
      CHECK(xag_deci_reads(width, said.data(), said.size(), &back) == 1);
      CHECK(back == value);
    }
}

void nothingIsLeftHolding() {
  CHECK(xag_balance_is_clear() == 1);
}

} // namespace

int main() {
  textIsCopiedAndFreed();
  joiningBuildsSomethingNew();
  textGrowsWhereItStands();
  countingAgreesWithUnicodeItself();
  countingCountsWhatAPersonWouldCount();
  binary128IsWrittenOutInSoftware();
  decimalCountsInTens();
  arithmeticIsWrittenOnce();
  everySizeIsTheSizeItSays();
  nothingIsLeftHolding();

  if (failures == 0)
    std::cout << "all runtime tests passed\n";
  return failures == 0 ? 0 : 1;
}
