// IEEE 754 decimal32, decimal64 and decimal128, written out in software.
//
// A decimal number is a sign, a whole-number coefficient and a power of ten.
// That is the whole of it: `1.10` is a coefficient of 110 scaled by 10^-2, and
// it is a different thing from `1.1`, which is 11 scaled by 10^-1. They are
// equal and they are not the same, and keeping them apart is the point of
// having the type at all.
//
// The encoding is BID — the coefficient stored as an ordinary binary integer —
// because the wide arithmetic underneath already speaks that language. DPD
// would want declet tables and buy nothing here.
//
// Everything rounds to nearest with ties to even, and every operation keeps the
// exponent the standard says it should prefer.

#include "xag_runtime.h"
#include "xag_wide.h"

#include <cstdio>
#include <cstring>

namespace {

using xag::U256;

std::FILE *output() { return static_cast<std::FILE *>(xag_output_file()); }

// What each format is made of. The bias is `emax + digits - 2` in all three,
// which is worth writing down because it is the only thing tying them together.
struct Shape {
  unsigned bits;      // the whole width
  unsigned digits;    // p, how many decimal digits the coefficient holds
  unsigned trailing;  // t, bits of coefficient below the combination field
  unsigned continued; // w, bits of exponent below the combination field
  int32_t bias;
  int32_t maxExponent; // the largest unbiased exponent
};

Shape shapeOf(uint32_t width) {
  if (width == 32)
    return Shape{32, 7, 20, 6, 101, 96 - 7 + 1};
  if (width == 64)
    return Shape{64, 16, 50, 8, 398, 384 - 16 + 1};
  return Shape{128, 34, 110, 12, 6176, 6144 - 34 + 1};
}

enum class Kind { Finite, Infinity, NotANumber };

struct Taken {
  int sign = 0;
  Kind kind = Kind::Finite;
  __uint128_t coefficient = 0;
  int32_t exponent = 0; // value = coefficient * 10^exponent
};

// Every power of ten a coefficient could want, worked out once. Building these
// by repeated multiplication on every call made each decimal operation cost
// more than the arithmetic it was doing.
constexpr unsigned kPowers = 78; // 10^77 is the last that fits in 256 bits

const U256 *powersOfTen() {
  static U256 table[kPowers];
  static bool ready = false;
  if (!ready) {
    table[0] = xag::wide(1);
    for (unsigned i = 1; i < kPowers; ++i)
      table[i] = xag::multiply(xag::narrow(table[i - 1]), 10);
    ready = true;
  }
  return table;
}

U256 tenTo(unsigned power) {
  return power < kPowers ? powersOfTen()[power] : U256{};
}

// How many digits, by looking rather than by dividing thirty-four times.
unsigned digitsIn(const U256 &value) {
  if (xag::isZero(value))
    return 1;
  const U256 *powers = powersOfTen();
  unsigned low = 1, high = kPowers - 1;
  while (low < high) {
    const unsigned middle = (low + high) / 2;
    if (xag::compare(value, powers[middle]) < 0)
      high = middle;
    else
      low = middle + 1;
  }
  return low;
}

XagDeci packSpecial(uint32_t width, int sign, bool notANumber) {
  const Shape shape = shapeOf(width);
  XagDeci bits = 0;
  const unsigned top = shape.bits - 1;
  if (sign)
    bits |= static_cast<XagDeci>(1) << top;
  // 11110 says infinity; 11111 says not a number.
  const XagDeci mark = notANumber ? 0x1F : 0x1E;
  bits |= mark << (top - 5);
  return bits;
}

// Put a coefficient and an exponent into the format, rounding to the digits it
// holds and settling for an exponent it can say.
XagDeci put(uint32_t width, int sign, U256 coefficient, int32_t exponent) {
  const Shape shape = shapeOf(width);
  const int32_t least = -shape.bias;

  // More digits than the format has: drop the extra, to nearest, ties to even.
  unsigned held = digitsIn(coefficient);
  while (held > shape.digits) {
    const unsigned drop = held - shape.digits;
    U256 quotient, remainder;
    xag::divide(coefficient, tenTo(drop), quotient, remainder);
    const U256 half = tenTo(drop - 1);
    U256 fiveTimes = xag::add(xag::shiftLeft(half, 2), half); // 5 * 10^(drop-1)
    const int side = xag::compare(remainder, fiveTimes);
    if (side > 0 || (side == 0 && (xag::narrow(quotient) & 1)))
      quotient = xag::add(quotient, xag::wide(1));
    coefficient = quotient;
    exponent += static_cast<int32_t>(drop);
    held = digitsIn(coefficient);
  }

  // Too small an exponent to say: shed digits until it can be said, which is
  // where a decimal loses precision rather than reach.
  while (exponent < least) {
    if (xag::isZero(coefficient)) {
      exponent = least;
      break;
    }
    uint64_t left = 0;
    const U256 quotient = xag::divideSmall(coefficient, 10, left);
    const int side = left > 5 ? 1 : (left < 5 ? -1 : 0);
    coefficient = (side > 0 || (side == 0 && (xag::narrow(quotient) & 1)))
                      ? xag::add(quotient, xag::wide(1))
                      : quotient;
    ++exponent;
  }

  // Too large an exponent: pad with zeros while there is room to.
  while (exponent > shape.maxExponent) {
    if (xag::isZero(coefficient)) {
      exponent = shape.maxExponent;
      break;
    }
    const U256 padded = xag::multiply(xag::narrow(coefficient), 10);
    if (digitsIn(padded) > shape.digits)
      return packSpecial(width, sign, false); // past everything: infinity
    coefficient = padded;
    --exponent;
  }

  const __uint128_t held128 = xag::narrow(coefficient);
  const int32_t biased = exponent + shape.bias;

  XagDeci bits = 0;
  const unsigned top = shape.bits - 1;
  if (sign)
    bits |= static_cast<XagDeci>(1) << top;

  const __uint128_t small = static_cast<__uint128_t>(1) << (shape.trailing + 3);
  if (held128 < small) {
    // The ordinary form: two exponent bits, then three of the coefficient.
    const XagDeci g = ((static_cast<XagDeci>(biased) >> shape.continued) & 0x3) << 3 |
                      ((held128 >> shape.trailing) & 0x7);
    bits |= g << (top - 5);
    bits |= static_cast<XagDeci>(biased & ((1 << shape.continued) - 1)) << shape.trailing;
    bits |= held128 & ((static_cast<__uint128_t>(1) << shape.trailing) - 1);
  } else {
    // The other one, for coefficients too large for three leading bits.
    const XagDeci g = 0x18 | (((static_cast<XagDeci>(biased) >> shape.continued) & 0x3) << 1) |
                      ((held128 >> shape.trailing) & 0x1);
    bits |= g << (top - 5);
    bits |= static_cast<XagDeci>(biased & ((1 << shape.continued) - 1)) << shape.trailing;
    bits |= held128 & ((static_cast<__uint128_t>(1) << shape.trailing) - 1);
  }
  return bits;
}

Taken take(uint32_t width, XagDeci bits) {
  const Shape shape = shapeOf(width);
  const unsigned top = shape.bits - 1;
  Taken out;
  out.sign = static_cast<int>((bits >> top) & 1);
  const unsigned g = static_cast<unsigned>((bits >> (top - 5)) & 0x1F);

  if ((g & 0x1E) == 0x1E) {
    out.kind = (g & 0x1) ? Kind::NotANumber : Kind::Infinity;
    return out;
  }

  const __uint128_t trailing =
      bits & ((static_cast<__uint128_t>(1) << shape.trailing) - 1);
  const uint32_t continued =
      static_cast<uint32_t>((bits >> shape.trailing) & ((1u << shape.continued) - 1));

  int32_t biased;
  __uint128_t coefficient;
  if ((g & 0x18) == 0x18) {
    biased = static_cast<int32_t>((((g >> 1) & 0x3) << shape.continued) | continued);
    coefficient = (static_cast<__uint128_t>(0x4 | (g & 0x1)) << shape.trailing) | trailing;
  } else {
    biased = static_cast<int32_t>((((g >> 3) & 0x3) << shape.continued) | continued);
    coefficient = (static_cast<__uint128_t>(g & 0x7) << shape.trailing) | trailing;
  }

  out.coefficient = coefficient;
  out.exponent = biased - shape.bias;
  // A coefficient too large for the format is not one, and reads as zero.
  U256 ceiling = tenTo(shape.digits);
  if (xag::compare(xag::wide(coefficient), ceiling) >= 0)
    out.coefficient = 0;
  return out;
}

// Bring two finite numbers to one exponent, without letting the scaling run
// away: past the digits the format holds, more zeros change no answer.
void align(const Shape &shape, Taken &a, Taken &b) {
  if (a.exponent == b.exponent)
    return;
  Taken &high = a.exponent > b.exponent ? a : b;
  const Taken &low = a.exponent > b.exponent ? b : a;
  int32_t apart = high.exponent - low.exponent;
  const int32_t most = static_cast<int32_t>(shape.digits) + 2;
  if (apart > most)
    apart = most;
  U256 lifted = xag::multiply(high.coefficient, xag::narrow(tenTo(static_cast<unsigned>(apart))));
  high.coefficient = xag::narrow(lifted);
  high.exponent -= apart;
}

} // namespace

extern "C" {

// Which of the two implementations this is. Built, not detected: a library
// compiled without a decimal unit cannot grow one at run time.
int xag_decimal_is_hardware(void) {
#ifdef XAG_DECIMAL_HARDWARE
  return 1;
#else
  return 0;
#endif
}

XagDeci xag_deci_add(uint32_t width, XagDeci a, XagDeci b) {
  const Shape shape = shapeOf(width);
  Taken x = take(width, a), y = take(width, b);
  if (x.kind == Kind::NotANumber || y.kind == Kind::NotANumber)
    return packSpecial(width, 0, true);
  if (x.kind == Kind::Infinity || y.kind == Kind::Infinity) {
    if (x.kind == Kind::Infinity && y.kind == Kind::Infinity)
      return x.sign == y.sign ? packSpecial(width, x.sign, false)
                              : packSpecial(width, 0, true);
    return packSpecial(width, x.kind == Kind::Infinity ? x.sign : y.sign, false);
  }

  align(shape, x, y);
  // Addition prefers the smaller of the two exponents, which is what keeps
  // `1.10 + 2.00` at two places rather than trimming it to `3.1`.
  const int32_t exponent = x.exponent < y.exponent ? x.exponent : y.exponent;

  if (x.sign == y.sign)
    return put(width, x.sign, xag::add(xag::wide(x.coefficient), xag::wide(y.coefficient)),
               exponent);

  const int order = xag::compare(xag::wide(x.coefficient), xag::wide(y.coefficient));
  if (order == 0)
    return put(width, 0, U256{}, exponent); // a difference of nothing is +0
  if (order > 0)
    return put(width, x.sign,
               xag::subtract(xag::wide(x.coefficient), xag::wide(y.coefficient)), exponent);
  return put(width, y.sign,
             xag::subtract(xag::wide(y.coefficient), xag::wide(x.coefficient)), exponent);
}

XagDeci xag_deci_negate(uint32_t width, XagDeci value) {
  const Shape shape = shapeOf(width);
  return value ^ (static_cast<XagDeci>(1) << (shape.bits - 1));
}

XagDeci xag_deci_sub(uint32_t width, XagDeci a, XagDeci b) {
  return xag_deci_add(width, a, xag_deci_negate(width, b));
}

XagDeci xag_deci_mul(uint32_t width, XagDeci a, XagDeci b) {
  const Taken x = take(width, a), y = take(width, b);
  const int sign = x.sign ^ y.sign;
  if (x.kind == Kind::NotANumber || y.kind == Kind::NotANumber)
    return packSpecial(width, 0, true);
  if (x.kind == Kind::Infinity || y.kind == Kind::Infinity) {
    const bool nothing = (x.kind == Kind::Finite && x.coefficient == 0) ||
                         (y.kind == Kind::Finite && y.coefficient == 0);
    return nothing ? packSpecial(width, 0, true) : packSpecial(width, sign, false);
  }
  // Multiplication prefers the sum of the two exponents.
  return put(width, sign, xag::multiply(x.coefficient, y.coefficient),
             x.exponent + y.exponent);
}

XagDeci xag_deci_div(uint32_t width, XagDeci a, XagDeci b) {
  const Shape shape = shapeOf(width);
  const Taken x = take(width, a), y = take(width, b);
  const int sign = x.sign ^ y.sign;
  if (x.kind == Kind::NotANumber || y.kind == Kind::NotANumber)
    return packSpecial(width, 0, true);
  if (x.kind == Kind::Infinity)
    return y.kind == Kind::Infinity ? packSpecial(width, 0, true)
                                    : packSpecial(width, sign, false);
  if (y.kind == Kind::Infinity)
    return put(width, sign, U256{}, 0);
  if (y.coefficient == 0)
    return x.coefficient == 0 ? packSpecial(width, 0, true)
                              : packSpecial(width, sign, false);
  if (x.coefficient == 0)
    return put(width, sign, U256{}, x.exponent - y.exponent);

  // Enough digits above to round on, and the remainder says whether the answer
  // stopped exactly or only nearly.
  const unsigned lift = shape.digits + 2;
  U256 quotient, left;
  xag::divide(xag::multiply(x.coefficient, xag::narrow(tenTo(lift))),
              xag::wide(y.coefficient), quotient, left);
  int32_t exponent = x.exponent - y.exponent - static_cast<int32_t>(lift);

  if (xag::isZero(left)) {
    // An exact answer is written at the exponent the standard prefers, or as
    // near to it as the digits allow: 1/8 is 0.125, not 0.1250000000000000.
    const int32_t preferred = x.exponent - y.exponent;
    while (exponent < preferred) {
      uint64_t remainder = 0;
      const U256 fewer = xag::divideSmall(quotient, 10, remainder);
      if (remainder != 0)
        break;
      quotient = fewer;
      ++exponent;
    }
  } else {
    // Nudge the last digit off a tie it has no right to sit on.
    quotient = xag::add(xag::shiftLeft(quotient, 1), xag::wide(1));
    U256 halved, ignored;
    xag::divide(quotient, xag::wide(2), halved, ignored);
    quotient = halved;
  }
  return put(width, sign, quotient, exponent);
}

XagDeci xag_deci_mod(uint32_t width, XagDeci a, XagDeci b) {
  const Taken x = take(width, a), y = take(width, b);
  if (x.kind != Kind::Finite || y.kind == Kind::NotANumber || y.coefficient == 0)
    return packSpecial(width, 0, true);
  if (y.kind == Kind::Infinity)
    return a;

  // What is left after taking away whole multiples, which is the truncated
  // remainder the whole-number types already answer with.
  XagDeci left = a;
  const XagDeci size = xag_deci_negate(width, b) & ~(static_cast<XagDeci>(0));
  const XagDeci magnitude = take(width, b).sign ? xag_deci_negate(width, b) : b;
  (void)size;
  const int sign = x.sign;
  while (xag_deci_compare(width, sign ? xag_deci_negate(width, left) : left, magnitude) >= 0) {
    XagDeci step = magnitude;
    while (true) {
      const XagDeci twice = xag_deci_add(width, step, step);
      if (xag_deci_compare(width, twice,
                           sign ? xag_deci_negate(width, left) : left) > 0)
        break;
      step = twice;
    }
    left = sign ? xag_deci_add(width, left, step) : xag_deci_sub(width, left, step);
  }
  return left;
}

XagDeci xag_deci_pow(uint32_t width, XagDeci base, XagDeci exponent) {
  const Taken e = take(width, exponent);
  if (e.kind != Kind::Finite)
    return packSpecial(width, 0, true);

  // Only a whole number is an exponent here.
  __uint128_t coefficient = e.coefficient;
  int32_t scale = e.exponent;
  while (scale < 0) {
    if (coefficient % 10)
      return packSpecial(width, 0, true);
    coefficient /= 10;
    ++scale;
  }
  while (scale > 0 && coefficient < (static_cast<__uint128_t>(1) << 60)) {
    coefficient *= 10;
    --scale;
  }
  if (scale != 0 || coefficient > 4096)
    return packSpecial(width, 0, true); // far past anything worth raising to

  XagDeci one = 0;
  xag_deci_reads(width, "1", 1, &one);
  XagDeci answer = one;
  XagDeci running = base;
  unsigned long long left = static_cast<unsigned long long>(coefficient);
  while (left > 0) {
    if (left & 1)
      answer = xag_deci_mul(width, answer, running);
    running = xag_deci_mul(width, running, running);
    left >>= 1;
  }
  return e.sign ? xag_deci_div(width, one, answer) : answer;
}

int32_t xag_deci_compare(uint32_t width, XagDeci a, XagDeci b) {
  const Shape shape = shapeOf(width);
  Taken x = take(width, a), y = take(width, b);
  if (x.kind == Kind::NotANumber || y.kind == Kind::NotANumber)
    return -3;
  if (x.kind == Kind::Infinity || y.kind == Kind::Infinity) {
    const int xv = x.kind == Kind::Infinity ? (x.sign ? -2 : 2) : 0;
    const int yv = y.kind == Kind::Infinity ? (y.sign ? -2 : 2) : 0;
    return xv < yv ? -1 : (xv > yv ? 1 : 0);
  }
  const bool xZero = x.coefficient == 0, yZero = y.coefficient == 0;
  if (xZero && yZero)
    return 0;
  if (x.sign != y.sign && !(xZero && yZero))
    return x.sign ? -1 : 1;

  align(shape, x, y);
  int order = xag::compare(xag::wide(x.coefficient), xag::wide(y.coefficient));
  if (x.exponent != y.exponent) // the alignment was capped; the larger scale wins
    order = x.exponent > y.exponent ? 1 : -1;
  return x.sign ? -order : order;
}

int32_t xag_deci_reads(uint32_t width, const char *text, uint64_t length, XagDeci *out) {
  const Shape shape = shapeOf(width);
  char buffer[600];
  if (length + 1 > sizeof(buffer))
    return 0;
  std::memcpy(buffer, text, length);
  buffer[length] = 0;

  auto answer = [&](XagDeci value) {
    if (out)
      *out = value;
    return 1;
  };
  if (std::strcmp(buffer, "infinity") == 0)
    return answer(packSpecial(width, 0, false));
  if (std::strcmp(buffer, "-infinity") == 0)
    return answer(packSpecial(width, 1, false));
  if (std::strcmp(buffer, "not-a-number") == 0)
    return answer(packSpecial(width, 0, true));

  const char *at = buffer;
  int sign = 0;
  if (*at == '+' || *at == '-') {
    sign = *at == '-';
    ++at;
  }

  U256 coefficient;
  int32_t exponent = 0;
  unsigned digits = 0;
  bool sawDigit = false, sawPoint = false;
  for (; *at; ++at) {
    if (*at == '.') {
      if (sawPoint)
        return 0;
      sawPoint = true;
      continue;
    }
    if (*at < '0' || *at > '9')
      break;
    sawDigit = true;
    // Past what the format holds there is no point keeping more; the exponent
    // carries what the digits cannot.
    if (digits < shape.digits + 4) {
      coefficient = xag::add(xag::multiply(xag::narrow(coefficient), 10),
                             xag::wide(static_cast<unsigned>(*at - '0')));
      ++digits;
      if (sawPoint)
        --exponent;
    } else if (!sawPoint) {
      ++exponent;
    }
  }
  if (!sawDigit)
    return 0;

  if (*at == 'e' || *at == 'E') {
    ++at;
    int expSign = 1;
    if (*at == '+' || *at == '-') {
      expSign = *at == '-' ? -1 : 1;
      ++at;
    }
    if (*at < '0' || *at > '9')
      return 0;
    int32_t written = 0;
    for (; *at >= '0' && *at <= '9'; ++at) {
      written = written * 10 + (*at - '0');
      if (written > 100000)
        written = 100000;
    }
    exponent += expSign * written;
  }
  if (*at != 0)
    return 0;
  return answer(put(width, sign, coefficient, exponent));
}

// The standard's own spelling: plain where the number reads plainly, and
// scientific where it does not. What is printed is the cohort member that was
// arrived at, so `1.10` prints as `1.10` and not as `1.1`.
void xag_print_deci(uint32_t width, XagDeci value) {
  const Taken x = take(width, value);
  if (x.kind == Kind::NotANumber) {
    std::fputs("not-a-number", output());
    return;
  }
  if (x.kind == Kind::Infinity) {
    std::fputs(x.sign ? "-infinity" : "infinity", output());
    return;
  }

  char digits[64];
  int32_t count = 0;
  U256 left = xag::wide(x.coefficient);
  if (xag::isZero(left)) {
    digits[count++] = '0';
  } else {
    while (!xag::isZero(left)) {
      uint64_t remainder = 0;
      left = xag::divideSmall(left, 10, remainder);
      digits[count++] = static_cast<char>('0' + static_cast<unsigned>(remainder));
    }
    for (int32_t i = 0; i < count / 2; ++i) {
      const char keep = digits[i];
      digits[i] = digits[count - 1 - i];
      digits[count - 1 - i] = keep;
    }
  }

  char written[160];
  char *put = written;
  if (x.sign)
    *put++ = '-';

  const int32_t adjusted = x.exponent + count - 1;
  if (x.exponent <= 0 && adjusted >= -6) {
    if (x.exponent == 0) {
      for (int32_t i = 0; i < count; ++i)
        *put++ = digits[i];
    } else if (adjusted >= 0) {
      const int32_t before = adjusted + 1;
      for (int32_t i = 0; i < before; ++i)
        *put++ = digits[i];
      *put++ = '.';
      for (int32_t i = before; i < count; ++i)
        *put++ = digits[i];
    } else {
      *put++ = '0';
      *put++ = '.';
      for (int32_t i = 0; i < -(adjusted + 1); ++i)
        *put++ = '0';
      for (int32_t i = 0; i < count; ++i)
        *put++ = digits[i];
    }
  } else {
    *put++ = digits[0];
    if (count > 1) {
      *put++ = '.';
      for (int32_t i = 1; i < count; ++i)
        *put++ = digits[i];
    }
    *put++ = 'e';
    int32_t e = adjusted;
    if (e < 0) {
      *put++ = '-';
      e = -e;
    } else {
      *put++ = '+';
    }
    char order[8];
    int32_t n = 0;
    do {
      order[n++] = static_cast<char>('0' + e % 10);
      e /= 10;
    } while (e);
    while (n)
      *put++ = order[--n];
  }
  *put = 0;
  std::fputs(written, output());
}

} // extern "C"
