#include "xag_runtime.h"
#include "xag_unicode.h"

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace {

int64_t live = 0;
std::FILE *out = nullptr;
std::FILE *err = nullptr;
std::FILE *in = nullptr;
// Which stream the print in hand named. Said by every print statement, so it is
// never read having been left over from an earlier one.
int32_t stream = 0;

std::FILE *output() {
  if (stream == 1)
    return err ? err : stderr;
  return out ? out : stdout;
}

// Where the compiler's own markers go — where a sum came round, that a read was
// reached — as against what the program itself writes. They shared standard
// error until a program could write there too, and then a program's complaint
// and the compiler talking to itself were one stream with no way to tell them
// apart. `XAG_NOTES` names a file while the compiler is running the program to
// see what it does; with nothing named they go to standard error as before,
// which is what a reader running their own built program should see.
std::FILE *notes() {
  static std::FILE *file = nullptr;
  static bool asked = false;
  if (!asked) {
    asked = true;
    if (const char *path = std::getenv("XAG_NOTES"); path && *path)
      file = std::fopen(path, "w");
  }
  return file ? file : stderr;
}

// Said as it happens rather than at the end: a program that stops is one whose
// last marker matters most, and there is no tidy exit to flush it in.
void note(const char *line) {
  std::FILE *where = notes();
  std::fputs(line, where);
  std::fflush(where);
}
std::FILE *input() { return in ? in : stdin; }

char *take(uint64_t bytes) {
  if (bytes == 0)
    return nullptr;
  char *memory = static_cast<char *>(std::malloc(bytes));
  if (!memory)
    xag_stop("there was no memory left");
  ++live;
  return memory;
}

// Where empty text points. A `str` that is here and empty and a `str` that is
// not here at all were both a null pointer, so no bit pattern was free to tell
// them apart and anything that may hold nothing had to carry a flag beside it.
// Empty text points here instead, and a null pointer means nothing at all —
// which is what lets `or-nothing str` be as wide as a `str`.
//
// One byte rather than none, because two objects of size zero may share an
// address and this one has to be its own.
char nothingAtAll[1] = {0};

bool borrowed(const char *memory) { return memory == nothingAtAll; }

void release(char *memory) {
  // Never the static empty: nobody took it, so nobody gives it back.
  if (!memory || borrowed(memory))
    return;
  std::free(memory);
  --live;
}

// ---- UTF-8, one scalar at a time

struct Scalar {
  uint32_t value = 0;
  unsigned width = 1;
};

Scalar scalarAt(const char *bytes, uint64_t length, uint64_t at) {
  const auto byte = static_cast<unsigned char>(bytes[at]);
  auto continuation = [&](uint64_t i) -> uint32_t {
    return at + i < length ? static_cast<unsigned char>(bytes[at + i]) & 0x3Fu : 0u;
  };
  if (byte < 0x80)
    return Scalar{byte, 1};
  if ((byte & 0xE0) == 0xC0 && at + 1 < length)
    return Scalar{((byte & 0x1Fu) << 6) | continuation(1), 2};
  if ((byte & 0xF0) == 0xE0 && at + 2 < length)
    return Scalar{((byte & 0x0Fu) << 12) | (continuation(1) << 6) | continuation(2), 3};
  if ((byte & 0xF8) == 0xF0 && at + 3 < length)
    return Scalar{((byte & 0x07u) << 18) | (continuation(1) << 12) |
                      (continuation(2) << 6) | continuation(3),
                  4};
  return Scalar{byte, 1}; // a byte that is not the start of anything is its own
}


} // namespace

extern "C" {

void xag_str_from(XagStr *out, const char *bytes, uint64_t length) {
  if (!out)
    return;
  *out = XagStr{nothingAtAll, length, length};
  if (length == 0)
    return;
  out->bytes = take(length);
  std::memcpy(out->bytes, bytes, length);
}

void xag_str_join(XagStr *out, const XagStr *pieces, uint64_t count) {
  if (!out)
    return;
  uint64_t total = 0;
  for (uint64_t i = 0; i < count; ++i)
    total += pieces[i].length;

  *out = XagStr{nothingAtAll, total, total};
  if (total == 0)
    return;
  out->bytes = take(total);
  uint64_t at = 0;
  for (uint64_t i = 0; i < count; ++i) {
    if (pieces[i].length == 0)
      continue;
    std::memcpy(out->bytes + at, pieces[i].bytes, pieces[i].length);
    at += pieces[i].length;
  }
}

void xag_str_push(XagStr *text, const XagStr *tail) {
  if (!text || !tail || tail->length == 0)
    return;
  const uint64_t wanted = text->length + tail->length;
  if (wanted > text->capacity) {
    uint64_t capacity = text->capacity ? text->capacity * 2 : wanted;
    if (capacity < wanted)
      capacity = wanted;
    char *grown = take(capacity);
    // The static empty is not memory anybody took, so there is nothing in it to
    // carry over and nothing to give back.
    if (text->bytes && !borrowed(text->bytes)) {
      std::memcpy(grown, text->bytes, text->length);
      release(text->bytes);
    }
    text->bytes = grown;
    text->capacity = capacity;
  }
  std::memcpy(text->bytes + text->length, tail->bytes, tail->length);
  text->length = wanted;
}

// UAX #29, rules GB1 through GB13, against the table the standard publishes.
//
// Written out rather than approximated because the approximation was wrong
// about Hangul, about every Indic vowel sign and about Arabic prepends — which
// is to say, right about Latin and emoji and wrong about most of the world.
// A scalar starts at every byte that is not a continuation byte, and the text
// is UTF-8 that the lexer already checked.
int64_t xag_str_count_letters(const XagStr *text) {
  if (!text || text->length == 0)
    return 0;
  int64_t scalars = 0;
  for (uint64_t at = 0; at < text->length; ++at)
    if ((static_cast<unsigned char>(text->bytes[at]) & 0xC0) != 0x80)
      ++scalars;
  return scalars;
}

int64_t xag_str_count(const XagStr *text) {
  if (!text || text->length == 0)
    return 0;

  int64_t clusters = 0;
  uint64_t at = 0;

  uint16_t before = 0;      // what the last code point was
  bool started = false;     // whether anything has been seen yet
  unsigned regionals = 0;   // how many flag halves run together up to here
  bool pictographic = false;// an Extended_Pictographic, then only Extends
  bool joined = false;      // ...and then a zero-width joiner
  bool consonant = false;   // an InCB Consonant, then Extends and Linkers
  bool linked = false;      // ...including at least one Linker

  while (at < text->length) {
    const Scalar scalar = scalarAt(text->bytes, text->length, at);
    const uint16_t what = xag::clusterOf(scalar.value);
    const uint8_t here = static_cast<uint8_t>(what & 0xF);
    const uint8_t last = static_cast<uint8_t>(before & 0xF);
    const unsigned incb = (what >> xag::kIncbShift) & 0x3;

    bool breaks;
    if (!started) {
      breaks = true; // GB1: something begins here
    } else if (last == xag::ClusterCR && here == xag::ClusterLF) {
      breaks = false; // GB3
    } else if (last == xag::ClusterControl || last == xag::ClusterCR ||
               last == xag::ClusterLF) {
      breaks = true; // GB4
    } else if (here == xag::ClusterControl || here == xag::ClusterCR ||
               here == xag::ClusterLF) {
      breaks = true; // GB5
    } else if (last == xag::ClusterL &&
               (here == xag::ClusterL || here == xag::ClusterV ||
                here == xag::ClusterLV || here == xag::ClusterLVT)) {
      breaks = false; // GB6
    } else if ((last == xag::ClusterLV || last == xag::ClusterV) &&
               (here == xag::ClusterV || here == xag::ClusterT)) {
      breaks = false; // GB7
    } else if ((last == xag::ClusterLVT || last == xag::ClusterT) &&
               here == xag::ClusterT) {
      breaks = false; // GB8
    } else if (here == xag::ClusterExtend || here == xag::ClusterZWJ) {
      breaks = false; // GB9
    } else if (here == xag::ClusterSpacingMark) {
      breaks = false; // GB9a
    } else if (last == xag::ClusterPrepend) {
      breaks = false; // GB9b
    } else if (consonant && linked && incb == xag::kIncbConsonant) {
      breaks = false; // GB9c, an Indic conjunct
    } else if (joined && (what & xag::kPictographic)) {
      breaks = false; // GB11, a picture joined to a picture
    } else if (here == xag::ClusterRegionalIndicator &&
               last == xag::ClusterRegionalIndicator && (regionals % 2) == 1) {
      breaks = false; // GB12 and GB13, flags in pairs
    } else {
      breaks = true; // GB999
    }

    if (breaks)
      ++clusters;

    // What this code point leaves behind for the next one.
    regionals = here == xag::ClusterRegionalIndicator
                    ? (breaks ? 1 : regionals + 1)
                    : 0;
    if (what & xag::kPictographic) {
      pictographic = true;
      joined = false;
    } else if (here == xag::ClusterExtend) {
      // a picture is still in view through its extends
    } else if (here == xag::ClusterZWJ) {
      joined = pictographic;
    } else {
      pictographic = false;
      joined = false;
    }
    if (incb == xag::kIncbConsonant) {
      consonant = true;
      linked = false;
    } else if (consonant && incb == xag::kIncbLinker) {
      linked = true;
    } else if (consonant && incb == xag::kIncbExtend) {
      // still within reach of the consonant
    } else {
      consonant = false;
      linked = false;
    }

    before = what;
    started = true;
    at += scalar.width;
  }
  return clusters;
}

int64_t xag_str_compare(const XagStr *left, const XagStr *right) {
  const uint64_t a = left ? left->length : 0;
  const uint64_t b = right ? right->length : 0;
  const uint64_t shared = a < b ? a : b;
  if (shared) {
    const int seen = std::memcmp(left->bytes, right->bytes, shared);
    if (seen != 0)
      return seen < 0 ? -1 : 1;
  }
  if (a == b)
    return 0;
  return a < b ? -1 : 1;
}

void xag_str_drop(XagStr *text) {
  if (!text)
    return;
  release(text->bytes);
  // Empty text, not absent text. A slot that has been let go of still holds a
  // `str`, and a `str` never holds a null pointer — that pattern means there is
  // nothing there at all, which is a different thing and somebody else's to say.
  text->bytes = nothingAtAll;
  text->length = 0;
  text->capacity = 0;
}

const char *xag_why_a_sum_came_round(void) {
  return "a sum does not fit, and nothing said it was meant to come round";
}

void xag_sum_came_round(void) { xag_stop(xag_why_a_sum_came_round()); }

void xag_print(const XagStr *text) {
  if (text && text->length)
    std::fwrite(text->bytes, 1, text->length, output());
}

// Every number is written out by exactly one piece of code, which both printing
// and `convert-to-str` call. Two spellings of the same value would be two
// chances for the screen and the string to disagree.
uint64_t xag_bool_writes(char *out, uint64_t room, int truth) {
  const char *said = truth ? "true" : "false";
  return xag_text_out(out, room, said, std::strlen(said));
}

void xag_print_bool(int truth) { std::fputs(truth ? "true" : "false", output()); }

void xag_set_output(void *file) { out = static_cast<std::FILE *>(file); }
void xag_set_error(void *file) { err = static_cast<std::FILE *>(file); }
void xag_writes_to(int32_t which) { stream = which; }

void *xag_output_file(void) { return output(); }

// Wrapping is done in unsigned arithmetic, where it is defined rather than
// merely usual, and then cut to the width the type was written with.
XagInt xag_int_fit(XagInt value, uint32_t width, int32_t is_signed) {
  if (width >= 128)
    return value;
  const __uint128_t mask = (static_cast<__uint128_t>(1) << width) - 1;
  __uint128_t kept = static_cast<__uint128_t>(value) & mask;
  if (is_signed && (kept >> (width - 1)) & 1)
    kept |= ~mask; // the sign, put back
  return static_cast<XagInt>(kept);
}

XagInt xag_int_div(XagInt a, XagInt b, uint32_t width, int32_t is_signed) {
  if (b == 0)
    xag_stop("a number was divided by zero");
  if (!is_signed) {
    const __uint128_t answer =
        static_cast<__uint128_t>(a) / static_cast<__uint128_t>(b);
    return xag_int_fit(static_cast<XagInt>(answer), width, is_signed);
  }
  // The one quotient that does not fit, wrapped like every other.
  if (b == -1)
    return xag_int_fit(static_cast<XagInt>(-static_cast<__uint128_t>(a)), width, is_signed);
  return xag_int_fit(a / b, width, is_signed);
}

XagInt xag_int_mod(XagInt a, XagInt b, uint32_t width, int32_t is_signed) {
  if (b == 0)
    xag_stop("a remainder was taken against zero");
  if (!is_signed) {
    const __uint128_t answer =
        static_cast<__uint128_t>(a) % static_cast<__uint128_t>(b);
    return xag_int_fit(static_cast<XagInt>(answer), width, is_signed);
  }
  if (b == -1)
    return 0;
  return xag_int_fit(a % b, width, is_signed);
}

// Floored: worked out as the truncated quotient and remainder, and moved one
// step toward negative infinity when the remainder is not zero and disagrees
// with the divisor about its sign. The one quotient that does not fit is the
// same one either way — there is no remainder to disagree.
XagInt xag_int_div_floored(XagInt a, XagInt b, uint32_t width, int32_t is_signed) {
  if (!is_signed || b == -1 || b == 0)
    return xag_int_div(a, b, width, is_signed);
  XagInt quotient = a / b;
  const XagInt remainder = a % b;
  if (remainder != 0 && ((remainder < 0) != (b < 0)))
    --quotient;
  return xag_int_fit(quotient, width, is_signed);
}

XagInt xag_int_mod_floored(XagInt a, XagInt b, uint32_t width, int32_t is_signed) {
  if (!is_signed || b == -1 || b == 0)
    return xag_int_mod(a, b, width, is_signed);
  XagInt remainder = a % b;
  if (remainder != 0 && ((remainder < 0) != (b < 0)))
    remainder += b;
  return xag_int_fit(remainder, width, is_signed);
}

// By squaring, in one place, so that no engine writes this loop a second time.
XagInt xag_int_pow(XagInt base, XagInt exponent, uint32_t width, int32_t is_signed) {
  if (is_signed && exponent < 0)
    xag_stop("a whole number was raised to a negative power, and that is not a whole number");
  XagInt answer = 1;
  __uint128_t left = static_cast<__uint128_t>(exponent);
  __uint128_t running = static_cast<__uint128_t>(base);
  while (left > 0) {
    if (left & 1)
      answer = xag_int_fit(static_cast<XagInt>(static_cast<__uint128_t>(answer) * running),
                           width, is_signed);
    running = static_cast<__uint128_t>(
        xag_int_fit(static_cast<XagInt>(running * running), width, is_signed));
    left >>= 1;
  }
  return answer;
}

double xag_bin_fit(double value, uint32_t width) {
  if (width == 16)
    return static_cast<double>(static_cast<_Float16>(value));
  if (width == 32)
    return static_cast<double>(static_cast<float>(value));
  return value;
}

const char *xag_why_no_number(void) {
  return "a `bin` had no number to give back";
}

void xag_no_number(void) { xag_stop(xag_why_no_number()); }

double xag_bin_number(double value) {
  if (!std::isfinite(value))
    xag_no_number();
  return value;
}

double xag_bin_mod(double a, double b, uint32_t width) {
  return xag_bin_fit(std::fmod(a, b), width);
}

// `fmod` and then one adjustment, which is how Python does it: adding the
// divisor to a remainder that disagrees with it about sign. That sum can round
// — a remainder of -1 against a divisor of 1e30 comes back as 1e30 — and is
// left to, because the exact answer is not a `bin` either.
double xag_bin_mod_floored(double a, double b, uint32_t width) {
  double remainder = std::fmod(a, b);
  if (remainder != 0) {
    if ((remainder < 0) != (b < 0))
      remainder += b;
  } else if (std::isfinite(b)) {
    remainder = std::copysign(0.0, b);
  }
  return xag_bin_fit(remainder, width);
}

// A power takes a whole-number exponent, in every format alike. Xag has no
// transcendental functions, so raising to a fraction has no answer to give —
// and a `bin64` answering where a `bin128` cannot would be worse than neither.
double xag_bin_pow(double base, double exponent, uint32_t width) {
  if (!std::isfinite(exponent) || exponent != std::trunc(exponent))
    return std::nan("");
  return xag_bin_fit(std::pow(base, exponent), width);
}

// The exact value of a binary float, all of it.
//
// Every binary float is a whole number times a power of two, and every such
// number has a decimal that ends: `m · 2^-k` is `m · 5^k` with the point k
// places in. So this multiplies it out and puts the point where it goes.
// Nothing is rounded and nothing is left off.
//
// What this wrote before was the shortest spelling that reads back to the same
// bits, which is what most languages write — and it could not tell three
// different numbers apart. A `bin16`, a `bin32` and a `bin64` each holding the
// nearest thing they have to a tenth all said `0.1`, and none of them held a
// tenth. They say `0.0999755859375`, `0.10000000149011612` and
// `0.1000000000000000055511151231257827021181583404541015625` now, which is
// what they have.
//
// It is long exactly where the value was never representable, which is where it
// is worth seeing. A half, a quarter and every whole number stay short.
//
// Answers how many characters the whole of it needs, and writes what fits —
// so a caller with too small a buffer asks once, makes room, and asks again.
uint64_t xag_bin_exactly(char *out, uint64_t room, int32_t negative,
                         unsigned __int128 significand, int32_t exponent) {
  if (significand == 0)
    return xag_text_out(out, room, negative ? "-0" : "0", negative ? 2u : 1u);
  // Enough for either direction: multiplying by five adds fewer than one digit
  // a time and so does multiplying by two, so the answer is never longer than
  // the number of steps plus what it started with.
  const int32_t steps = exponent < 0 ? -exponent : exponent;
  const int32_t most = steps + 64;
  unsigned char *digit = static_cast<unsigned char *>(std::malloc(most));
  if (!digit)
    xag_stop("there was no memory left");
  int length = 0;
  for (unsigned __int128 left = significand; left; left /= 10)
    digit[length++] = static_cast<unsigned char>(left % 10);

  // Ones place first, so a carry runs upward.
  const auto times = [&](unsigned by) {
    uint64_t carry = 0;
    for (int i = 0; i < length; ++i) {
      const uint64_t made = static_cast<uint64_t>(digit[i]) * by + carry;
      digit[i] = static_cast<unsigned char>(made % 10);
      carry = made / 10;
    }
    while (carry) {
      digit[length++] = static_cast<unsigned char>(carry % 10);
      carry /= 10;
    }
  };
  // Eleven fives and twenty-six twos at a time, which are the most that fit in
  // a step without the carry outgrowing what holds it.
  for (int32_t left = steps; left > 0;) {
    const int32_t now = left > (exponent < 0 ? 11 : 26) ? (exponent < 0 ? 11 : 26) : left;
    unsigned by = 1;
    for (int i = 0; i < now; ++i)
      by *= exponent < 0 ? 5u : 2u;
    times(by);
    left -= now;
  }

  const int32_t point = exponent < 0 ? -exponent : 0; // digits after the point
  int32_t lowest = 0;                                 // the last one worth writing
  while (lowest < point && digit[lowest] == 0)
    ++lowest;

  uint64_t needs = negative ? 1 : 0;
  needs += length > point ? static_cast<uint64_t>(length - point) : 1;
  if (lowest < point)
    needs += 1 + static_cast<uint64_t>(point - lowest);

  uint64_t at = 0;
  const auto put = [&](char c) {
    if (at + 1 < room)
      out[at] = c;
    ++at;
  };
  if (negative)
    put('-');
  if (length > point)
    for (int i = length - 1; i >= point; --i)
      put(static_cast<char>('0' + digit[i]));
  else
    put('0');
  if (lowest < point) {
    put('.');
    for (int i = point - 1; i >= lowest; --i)
      put(static_cast<char>('0' + digit[i]));
  }
  if (room)
    out[at < room ? at : room - 1] = 0;
  std::free(digit);
  return needs;
}

uint64_t xag_bin_writes(char *out, uint64_t room, double value, uint32_t width) {
  (void)width; // the value is already the one that width holds
  if (std::isnan(value))
    return xag_text_out(out, room, "not-a-number", 12);
  if (std::isinf(value))
    return value < 0 ? xag_text_out(out, room, "-infinity", 9)
                     : xag_text_out(out, room, "infinity", 8);
  uint64_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  const bool negative = (bits >> 63) != 0;
  const uint64_t raised = (bits >> 52) & 0x7ff;
  const uint64_t fraction = bits & 0xfffffffffffffull;
  // A subnormal has no hidden one and sits at the smallest exponent there is.
  const unsigned __int128 significand =
      raised == 0 ? fraction : fraction | (1ull << 52);
  const int32_t exponent =
      raised == 0 ? -1074 : static_cast<int32_t>(raised) - 1075;
  return xag_bin_exactly(out, room, negative, significand, exponent);
}

// Said in whatever room it takes. The exact value of a subnormal `bin64` is a
// thousand characters, and every one of them is the number.
void xag_print_bin(double value, uint32_t width) {
  char written[XAG_NUMBER_ROOM];
  const uint64_t needs = xag_bin_writes(written, sizeof(written), value, width);
  if (needs < sizeof(written)) {
    std::fputs(written, output());
    return;
  }
  char *room = static_cast<char *>(std::malloc(needs + 1));
  if (!room)
    xag_stop("there was no memory left");
  (void)xag_bin_writes(room, needs + 1, value, width);
  std::fputs(room, output());
  std::free(room);
}

int32_t xag_bin_reads(const char *text, uint64_t length, uint32_t width, double *out) {
  // What a print writes has to read back, and a print writes the whole of a
  // value: the exact smallest `bin64` runs to a thousand characters. Refusing
  // anything longer than a fixed buffer refused the language's own spelling of
  // its own numbers.
  char room[512];
  char *buffer = room;
  if (length + 1 > sizeof(room)) {
    buffer = static_cast<char *>(std::malloc(length + 1));
    if (!buffer)
      xag_stop("there was no memory left");
  }
  std::memcpy(buffer, text, length);
  buffer[length] = 0;
  struct Freeing {
    char *what;
    char *stack;
    ~Freeing() {
      if (what != stack)
        std::free(what);
    }
  } freeing{buffer, room};

  // The spellings a print produces are the spellings a program may write, so
  // what comes out can go back in.
  auto answer = [&](double value) {
    if (out)
      *out = value;
    return 1;
  };
  if (std::strcmp(buffer, "infinity") == 0)
    return answer(HUGE_VAL);
  if (std::strcmp(buffer, "-infinity") == 0)
    return answer(-HUGE_VAL);
  if (std::strcmp(buffer, "not-a-number") == 0)
    return answer(std::nan(""));

  char *stopped = nullptr;
  const double read = std::strtod(buffer, &stopped);
  if (stopped == buffer || *stopped != 0)
    return 0; // not a number at all
  const double fitted = xag_bin_fit(read, width);
  if (out)
    *out = fitted;
  // Anything else that arrives infinite was not what was written down: it was
  // a number too large for the width to hold.
  return std::isinf(fitted) ? 0 : 1;
}

uint64_t xag_int_writes(char *out, uint64_t room, XagInt value, uint32_t width,
                        int32_t is_signed) {
  value = xag_int_fit(value, width, is_signed);
  char digits[41];
  unsigned at = sizeof(digits);
  __uint128_t magnitude;
  bool negative = false;
  if (is_signed && value < 0) {
    negative = true;
    magnitude = -static_cast<__uint128_t>(value);
  } else {
    magnitude = static_cast<__uint128_t>(value);
  }
  if (magnitude == 0)
    digits[--at] = '0';
  while (magnitude > 0) {
    digits[--at] = static_cast<char>('0' + static_cast<unsigned>(magnitude % 10));
    magnitude /= 10;
  }
  if (negative)
    digits[--at] = '-';
  return xag_text_out(out, room, digits + at, sizeof(digits) - at);
}

void xag_print_int(XagInt value, uint32_t width, int32_t is_signed) {
  char written[XAG_NUMBER_ROOM];
  (void)xag_int_writes(written, sizeof(written), value, width, is_signed);
  std::fputs(written, output());
}

// `convert-to-str` is these: written out by the very code that prints, and then
// kept as text the program owns.
void xag_str_of_bool(XagStr *out, int truth) {
  char written[XAG_NUMBER_ROOM];
  xag_str_from(out, written, xag_bool_writes(written, sizeof(written), truth));
}

void xag_str_of_int(XagStr *out, XagInt value, uint32_t width, int32_t is_signed) {
  char written[XAG_NUMBER_ROOM];
  xag_str_from(out, written,
               xag_int_writes(written, sizeof(written), value, width, is_signed));
}

void xag_str_of_bin(XagStr *out, double value, uint32_t width) {
  char written[XAG_NUMBER_ROOM];
  const uint64_t needs = xag_bin_writes(written, sizeof(written), value, width);
  if (needs < sizeof(written)) {
    xag_str_from(out, written, needs);
    return;
  }
  char *room = static_cast<char *>(std::malloc(needs + 1));
  if (!room)
    xag_stop("there was no memory left");
  (void)xag_bin_writes(room, needs + 1, value, width);
  xag_str_from(out, room, needs);
  std::free(room);
}

void xag_str_of_bin128(XagStr *out, XagBin128 value) {
  char written[XAG_NUMBER_ROOM];
  const uint64_t needs = xag_bin128_writes(written, sizeof(written), value);
  if (needs < sizeof(written)) {
    xag_str_from(out, written, needs);
    return;
  }
  char *room = static_cast<char *>(std::malloc(needs + 1));
  if (!room)
    xag_stop("there was no memory left");
  (void)xag_bin128_writes(room, needs + 1, value);
  xag_str_from(out, room, needs);
  std::free(room);
}

void xag_str_of_deci(XagStr *out, uint32_t width, XagDeci value) {
  char written[XAG_NUMBER_ROOM];
  xag_str_from(out, written, xag_deci_writes(written, sizeof(written), width, value));
}

int64_t xag_live_allocations(void) { return live; }
void xag_forget_allocations(int64_t backTo) { live = backTo; }

void xag_would_read(void) {
  std::fflush(output());
  note("xag-would-read\n");
  std::exit(0);
}

void xag_would_take_time(void) {
  std::fflush(output());
  note("xag-would-take-time\n");
  std::exit(0);
}
int xag_balance_is_clear(void) { return live == 0 ? 1 : 0; }

void xag_many_out_of_range(int64_t index, uint64_t length) {
  if (length == 0)
    xag_stop("a place was asked for in a `many` that holds none");
  char why[128];
  // Below the first place and past the last are different mistakes, and the one
  // worth spelling out is the first: a reader who wrote `*0*` was counting from
  // somewhere this language does not count from.
  if (index < 1)
    std::snprintf(why, sizeof(why),
                  "place %lld was asked for, and the first place is 1",
                  static_cast<long long>(index));
  else
    std::snprintf(why, sizeof(why), "place %lld was asked for, and the `many` has %llu",
                  static_cast<long long>(index),
                  static_cast<unsigned long long>(length));
  xag_stop(why);
}

uint64_t xag_many_place(int64_t index, uint64_t length) {
  if (length == 0)
    xag_stop("a place was asked for in a `many` that holds none");
  // Places are counted from one, so the last is `count[…]` and the offset is
  // one less than the place.
  if (index < 1 || static_cast<uint64_t>(index) > length)
    xag_many_out_of_range(index, length);
  return static_cast<uint64_t>(index) - 1;
}

void xag_many_new(XagMany *out, uint64_t length, uint64_t stride) {
  out->length = length;
  if (length == 0) {
    out->places = nullptr;
    return;
  }
  out->places = take(length * stride);
  std::memset(out->places, 0, length * stride);
}

void xag_many_drop(XagMany *m) {
  if (m->places)
    release(static_cast<char *>(m->places));
  m->places = nullptr;
  m->length = 0;
}

void xag_many_drop_str(XagMany *m) {
  XagStr *held = static_cast<XagStr *>(m->places);
  for (uint64_t i = 0; i < m->length; ++i)
    xag_str_drop(&held[i]);
  xag_many_drop(m);
}

void xag_growing_new(XagGrowing *out) {
  out->places = nullptr;
  out->length = 0;
  out->room = 0;
}

void xag_growing_add(XagGrowing *g, uint64_t stride, const void *one) {
  if (g->length == g->room) {
    // Twice as roomy, or room for four to begin with — so that growing one
    // place at a time costs one move every so often rather than one each time.
    const uint64_t wider = g->room == 0 ? 4 : g->room * 2;
    char *moved = take(wider * stride);
    if (g->places) {
      std::memcpy(moved, g->places, g->length * stride);
      release(static_cast<char *>(g->places));
    }
    g->places = moved;
    g->room = wider;
  }
  std::memcpy(static_cast<char *>(g->places) + g->length * stride, one, stride);
  ++g->length;
}

void xag_growing_drop(XagGrowing *g) {
  if (g->places)
    release(static_cast<char *>(g->places));
  g->places = nullptr;
  g->length = 0;
  g->room = 0;
}

void xag_growing_drop_str(XagGrowing *g) {
  XagStr *held = static_cast<XagStr *>(g->places);
  for (uint64_t i = 0; i < g->length; ++i)
    xag_str_drop(&held[i]);
  xag_growing_drop(g);
}

void xag_many_fill(XagMany *m, uint64_t stride, const void *one) {
  char *at = static_cast<char *>(m->places);
  for (uint64_t i = 0; i < m->length; ++i)
    std::memcpy(at + i * stride, one, stride);
}

// ---- what comes in

void xag_set_input(void *file) { in = static_cast<std::FILE *>(file); }
void *xag_input_file(void) { return input(); }

int32_t xag_read_line(XagStr *out) {
  char *line = nullptr;
  std::size_t room = 0;
  const auto got = ::getline(&line, &room, input());
  if (got < 0) {
    std::free(line);
    // Nothing was read, and the `str` handed back is empty rather than absent —
    // whether there was a line is the answer, said separately.
    out->bytes = nothingAtAll;
    out->length = 0;
    out->capacity = 0;
    return 0;
  }
  // Whatever ended the line is not part of it, and a line ended by the file
  // running out was not ended by anything.
  std::size_t length = static_cast<std::size_t>(got);
  while (length && (line[length - 1] == '\n' || line[length - 1] == '\r'))
    --length;
  xag_str_from(out, line, length);
  std::free(line);
  return 1;
}

namespace {
int32_t argumentCount = 0;
char **argumentValues = nullptr;
} // namespace

void xag_set_arguments(int32_t count, char **values) {
  argumentCount = count;
  argumentValues = values;
}

void xag_arguments(XagMany *out) {
  xag_many_new(out, static_cast<uint64_t>(argumentCount < 0 ? 0 : argumentCount),
               sizeof(XagStr));
  XagStr *held = static_cast<XagStr *>(out->places);
  for (int32_t i = 0; i < argumentCount; ++i)
    xag_str_from(&held[i], argumentValues[i], std::strlen(argumentValues[i]));
}

int32_t xag_int_reads(uint32_t width, int32_t is_signed, const char *text,
                      uint64_t length, XagInt *out) {
  if (length == 0)
    return 0;
  uint64_t at = 0;
  const bool negative = text[0] == '-';
  if (negative || text[0] == '+')
    at = 1;
  if (at == length || (negative && !is_signed))
    return 0;

  __uint128_t magnitude = 0;
  const __uint128_t ceiling =
      is_signed ? (static_cast<__uint128_t>(1) << (width - 1))
                : (width == 128 ? ~static_cast<__uint128_t>(0)
                                : (static_cast<__uint128_t>(1) << width) - 1);
  for (; at < length; ++at) {
    if (text[at] < '0' || text[at] > '9')
      return 0;
    const __uint128_t before = magnitude;
    magnitude = magnitude * 10 + static_cast<unsigned>(text[at] - '0');
    if (magnitude / 10 != before) // it went past what 128 bits hold
      return 0;
    if (is_signed ? magnitude > ceiling : magnitude > ceiling)
      return 0;
  }
  if (is_signed && !negative && magnitude >= ceiling)
    return 0;
  *out = negative ? -static_cast<XagInt>(magnitude) : static_cast<XagInt>(magnitude);
  return 1;
}

void xag_note_taken(void) { ++live; }
void xag_note_given(void) { --live; }

void xag_came_round(uint32_t at) {
  char line[64];
  std::snprintf(line, sizeof(line), "xag-came-round %u\n", static_cast<unsigned>(at));
  note(line);
}

namespace {
XagStopping handsBackTo = nullptr;
} // namespace

void xag_hand_back_stops(XagStopping handler) { handsBackTo = handler; }

uint32_t xag_where = 0;

void xag_stop(const char *why) {
  const char *reason = why ? why : "no reason was given";
  // A handler does not come back. One that does has not done its job, and the
  // program stops the way it would have.
  if (handsBackTo)
    handsBackTo(reason);
  // Both streams, not whichever the last print named: a program that wrote to
  // one and then stopped while the other still held something would lose it.
  std::fflush(out ? out : stdout);
  std::fflush(err ? err : stderr);
  char line[256];
  std::snprintf(line, sizeof(line), "\nthe program stopped: %s\n", reason);
  note(line);
  // Only the build that was asked to keep track has anything to say here, and
  // that build is one nobody but the compiler ever runs.
  if (xag_where != 0) {
    std::snprintf(line, sizeof(line), "xag-stopped-at %u\n",
                  static_cast<unsigned>(xag_where));
    note(line);
  }
  std::exit(1);
}

} // extern "C"
