#include "xag_runtime.h"

#include <cstdio>
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

XagStr xag_str_from(const char *bytes, uint64_t length) {
  XagStr text{nullptr, length, length};
  if (length == 0)
    return text;
  text.bytes = take(length);
  std::memcpy(text.bytes, bytes, length);
  return text;
}

XagStr xag_str_join(const XagStr *pieces, uint64_t count) {
  uint64_t total = 0;
  for (uint64_t i = 0; i < count; ++i)
    total += pieces[i].length;

  XagStr joined{nullptr, total, total};
  if (total == 0)
    return joined;
  joined.bytes = take(total);
  uint64_t at = 0;
  for (uint64_t i = 0; i < count; ++i) {
    if (pieces[i].length == 0)
      continue;
    std::memcpy(joined.bytes + at, pieces[i].bytes, pieces[i].length);
    at += pieces[i].length;
  }
  return joined;
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

void xag_print_i64(int64_t number) { std::fprintf(output(), "%lld", (long long)number); }

void xag_print_bool(int truth) { std::fputs(truth ? "true" : "false", output()); }

void xag_set_output(void *file) { out = static_cast<std::FILE *>(file); }

// Wrapping is done in unsigned arithmetic, where it is defined rather than
// merely usual.
int64_t xag_i64_add(int64_t a, int64_t b) {
  return static_cast<int64_t>(static_cast<uint64_t>(a) + static_cast<uint64_t>(b));
}
int64_t xag_i64_sub(int64_t a, int64_t b) {
  return static_cast<int64_t>(static_cast<uint64_t>(a) - static_cast<uint64_t>(b));
}
int64_t xag_i64_mul(int64_t a, int64_t b) {
  return static_cast<int64_t>(static_cast<uint64_t>(a) * static_cast<uint64_t>(b));
}

int64_t xag_i64_div(int64_t a, int64_t b) {
  if (b == 0)
    xag_stop("a number was divided by zero");
  if (a == INT64_MIN && b == -1)
    return a; // the one quotient that does not fit, wrapped like any other
  return a / b;
}

int64_t xag_i64_mod(int64_t a, int64_t b) {
  if (b == 0)
    xag_stop("a remainder was taken against zero");
  if (a == INT64_MIN && b == -1)
    return 0;
  return a % b;
}

// By squaring, in one place, so that no engine writes this loop a second time.
int64_t xag_i64_pow(int64_t base, int64_t exponent) {
  if (exponent < 0)
    xag_stop("a whole number was raised to a negative power, and that is not a whole number");
  int64_t answer = 1;
  while (exponent > 0) {
    if (exponent & 1)
      answer = xag_i64_mul(answer, base);
    base = xag_i64_mul(base, base);
    exponent >>= 1;
  }
  return answer;
}

int64_t xag_live_allocations(void) { return live; }
int xag_balance_is_clear(void) { return live == 0 ? 1 : 0; }

void xag_stop(const char *why) {
  std::fflush(output());
  std::fprintf(stderr, "\nthe program stopped: %s\n", why ? why : "no reason was given");
  std::exit(1);
}

} // extern "C"
