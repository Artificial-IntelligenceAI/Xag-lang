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
  return xag_str_from(text.data(), text.size());
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
  XagStr joined = xag_str_join(pieces, 3);
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
  CHECK(xag_i64_add(2, 3) == 5);
  CHECK(xag_i64_sub(2, 3) == -1);
  CHECK(xag_i64_mul(6, 7) == 42);

  // `division = "truncated"`: toward zero, remainder follows the dividend.
  CHECK(xag_i64_div(-7, 2) == -3);
  CHECK(xag_i64_mod(-7, 2) == -1);
  CHECK(xag_i64_div(7, -2) == -3);
  CHECK(xag_i64_mod(7, -2) == 1);

  // `overflow = "wrap"`, and wrapping is defined rather than merely usual.
  CHECK(xag_i64_add(INT64_MAX, 1) == INT64_MIN);
  CHECK(xag_i64_mul(INT64_MIN, -1) == INT64_MIN);
  CHECK(xag_i64_div(INT64_MIN, -1) == INT64_MIN);

  // By squaring, in one place.
  CHECK(xag_i64_pow(2, 10) == 1024);
  CHECK(xag_i64_pow(2, 0) == 1);
  CHECK(xag_i64_pow(-2, 3) == -8);
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
  nothingIsLeftHolding();

  if (failures == 0)
    std::cout << "all runtime tests passed\n";
  return failures == 0 ? 0 : 1;
}
