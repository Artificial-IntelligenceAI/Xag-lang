// The runtime is the one thing three engines agreeing cannot vouch for: they
// all call it, so a wrong answer here is wrong identically everywhere and the
// vote is unanimous. These are its own tests, and they are the only ones it has.

#include "xag_runtime.h"

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

void nothingIsLeftHolding() {
  CHECK(xag_balance_is_clear() == 1);
}

} // namespace

int main() {
  textIsCopiedAndFreed();
  joiningBuildsSomethingNew();
  textGrowsWhereItStands();
  countingCountsWhatAPersonWouldCount();
  arithmeticIsWrittenOnce();
  everySizeIsTheSizeItSays();
  nothingIsLeftHolding();

  if (failures == 0)
    std::cout << "all runtime tests passed\n";
  return failures == 0 ? 0 : 1;
}
