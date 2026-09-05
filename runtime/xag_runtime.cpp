#include "xag_runtime.h"

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace {

int64_t live = 0;
std::FILE *out = nullptr;

std::FILE *output() { return out ? out : stdout; }

char *take(uint64_t bytes) {
  if (bytes == 0)
    return nullptr;
  char *memory = static_cast<char *>(std::malloc(bytes));
  if (!memory)
    xag_stop("there was no memory left");
  ++live;
  return memory;
}

void release(char *memory) {
  if (!memory)
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

// A working subset of UAX #29: enough for combining marks, variation selectors,
// skin tones, zero-width joiner sequences and flags. The full tables are not
// here, and the cases they would add are noted where `characters` is decided.
bool extends(uint32_t c) {
  return (c >= 0x0300 && c <= 0x036F) || (c >= 0x0483 && c <= 0x0489) ||
         (c >= 0x0591 && c <= 0x05BD) || (c >= 0x0610 && c <= 0x061A) ||
         (c >= 0x064B && c <= 0x065F) || c == 0x0670 ||
         (c >= 0x06D6 && c <= 0x06DC) || (c >= 0x0E31 && c <= 0x0E3A) ||
         (c >= 0x0E47 && c <= 0x0E4E) || (c >= 0x1AB0 && c <= 0x1AFF) ||
         (c >= 0x1DC0 && c <= 0x1DFF) || (c >= 0x20D0 && c <= 0x20F0) ||
         (c >= 0xFE00 && c <= 0xFE0F) || (c >= 0xFE20 && c <= 0xFE2F) ||
         (c >= 0x1F3FB && c <= 0x1F3FF) || (c >= 0xE0100 && c <= 0xE01EF);
}

bool isRegional(uint32_t c) { return c >= 0x1F1E6 && c <= 0x1F1FF; }

} // namespace

extern "C" {

void xag_str_from(XagStr *out, const char *bytes, uint64_t length) {
  if (!out)
    return;
  *out = XagStr{nullptr, length, length};
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

  *out = XagStr{nullptr, total, total};
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
    if (text->bytes) {
      std::memcpy(grown, text->bytes, text->length);
      release(text->bytes);
    }
    text->bytes = grown;
    text->capacity = capacity;
  }
  std::memcpy(text->bytes + text->length, tail->bytes, tail->length);
  text->length = wanted;
}

int64_t xag_str_count(const XagStr *text) {
  if (!text || text->length == 0)
    return 0;

  int64_t clusters = 0;
  uint64_t at = 0;
  bool open = false;      // a cluster is already being counted
  bool afterZwj = false;  // the last scalar was a zero-width joiner
  unsigned regionals = 0; // how many flag halves have run together

  while (at < text->length) {
    const Scalar scalar = scalarAt(text->bytes, text->length, at);
    const uint32_t c = scalar.value;

    bool joins = false;
    if (!open) {
      joins = false;
    } else if (c == 0x000A && at > 0 && text->bytes[at - 1] == '\r') {
      joins = true; // a carriage return and a line feed are one break
    } else if (extends(c) || c == 0x200D) {
      joins = true;
    } else if (afterZwj) {
      joins = true; // whatever a joiner joined belongs to what came before
    } else if (isRegional(c) && regionals % 2 == 1) {
      joins = true; // flags come in pairs
    }

    if (!joins) {
      ++clusters;
      regionals = 0;
    }
    regionals = isRegional(c) ? regionals + 1 : 0;
    afterZwj = c == 0x200D;
    open = true;
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
  text->bytes = nullptr;
  text->length = 0;
  text->capacity = 0;
}

void xag_print(const XagStr *text) {
  if (text && text->length)
    std::fwrite(text->bytes, 1, text->length, output());
}

void xag_print_bool(int truth) { std::fputs(truth ? "true" : "false", output()); }

void xag_set_output(void *file) { out = static_cast<std::FILE *>(file); }

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

double xag_bin_mod(double a, double b, uint32_t width) {
  return xag_bin_fit(std::fmod(a, b), width);
}

double xag_bin_pow(double base, double exponent, uint32_t width) {
  return xag_bin_fit(std::pow(base, exponent), width);
}

// The shortest spelling that reads back as the same value. Both engines call
// this, so what a number looks like cannot depend on which of them said it.
void xag_print_bin(double value, uint32_t width) {
  if (std::isnan(value)) {
    std::fputs("not-a-number", output());
    return;
  }
  if (std::isinf(value)) {
    std::fputs(value < 0 ? "-infinity" : "infinity", output());
    return;
  }
  const unsigned most = width == 16 ? 5u : width == 32 ? 9u : 17u;
  char written[64];
  for (unsigned digits = 1; digits <= most; ++digits) {
    std::snprintf(written, sizeof(written), "%.*g", static_cast<int>(digits), value);
    if (xag_bin_fit(std::strtod(written, nullptr), width) == value)
      break;
  }
  std::fputs(written, output());
}

int32_t xag_bin_reads(const char *text, uint64_t length, uint32_t width, double *out) {
  char buffer[512];
  if (length + 1 > sizeof(buffer))
    return 0;
  std::memcpy(buffer, text, length);
  buffer[length] = 0;

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

void xag_print_int(XagInt value, uint32_t width, int32_t is_signed) {
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
  std::fwrite(digits + at, 1, sizeof(digits) - at, output());
}

int64_t xag_live_allocations(void) { return live; }
int xag_balance_is_clear(void) { return live == 0 ? 1 : 0; }

void xag_stop(const char *why) {
  std::fflush(output());
  std::fprintf(stderr, "\nthe program stopped: %s\n", why ? why : "no reason was given");
  std::exit(1);
}

} // extern "C"
