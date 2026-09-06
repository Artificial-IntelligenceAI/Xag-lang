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

// Ten times a wide value, staying wide. Doing this by narrowing to 128 bits and
// multiplying there was silently wrong from 10^39 up, because that is where a
// power of ten stops fitting in 128 bits — so every entry above it was rubbish,
// and `digitsIn` searches this table.
U256 timesTen(const U256 &value) {
  return xag::add(xag::shiftLeft(value, 3), xag::shiftLeft(value, 1));
}

// The largest power of ten a 64-bit number holds, which is how many can be
// applied at a time.
constexpr uint64_t kTenTo19 = 10000000000000000000ull;

U256 timesTenTo(U256 value, unsigned power) {
  while (power >= 19) {
    value = xag::multiplySmall(value, kTenTo19);
    power -= 19;
  }
  if (power) {
    uint64_t small = 1;
    for (unsigned i = 0; i < power; ++i)
      small *= 10;
    value = xag::multiplySmall(value, small);
  }
  return value;
}

const U256 *powersOfTen() {
  static U256 table[kPowers];
  static bool ready = false;
  if (!ready) {
    table[0] = xag::wide(1);
    for (unsigned i = 1; i < kPowers; ++i)
      table[i] = timesTen(table[i - 1]);
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

// A value part-way through a power, carried with more digits than the format
// keeps. Squaring a number twelve times and rounding to `p` digits after every
// one of them piles those roundings into the answer — `-0.7 ^ 345` came out
// with its last digit off, and `0.277^106` with its last three. Four spare
// digits put the accumulated error far below the one that gets written down.
struct Running {
  U256 coefficient;
  int32_t exponent = 0;
  int sign = 0;
};

Running trimTo(Running value, unsigned digits) {
  unsigned held = digitsIn(value.coefficient);
  while (held > digits) {
    const unsigned drop = held - digits;
    U256 quotient, remainder;
    xag::divide(value.coefficient, tenTo(drop), quotient, remainder);
    const U256 half = tenTo(drop - 1);
    const U256 fiveTimes = xag::add(xag::shiftLeft(half, 2), half);
    const int side = xag::compare(remainder, fiveTimes);
    if (side > 0 || (side == 0 && (xag::narrow(quotient) & 1)))
      quotient = xag::add(quotient, xag::wide(1));
    value.coefficient = quotient;
    value.exponent += static_cast<int32_t>(drop);
    held = digitsIn(value.coefficient);
  }
  return value;
}

// Past this, no format has anything to say, and squaring only goes further:
// raising by halving the count multiplies numbers that are all on one side of
// one, so a running value that has left the range never comes back into it.
// Holding the exponent here rather than letting it run keeps it inside what an
// `int32_t` says, and `put` turns it into an infinity or a nothing at the end.
constexpr int32_t kFarOut = 100000000;

Running times(const Running &a, const Running &b, unsigned digits) {
  Running out;
  out.sign = a.sign ^ b.sign;
  out.coefficient =
      xag::multiply(xag::narrow(a.coefficient), xag::narrow(b.coefficient));
  int64_t reach = static_cast<int64_t>(a.exponent) + b.exponent;
  if (reach > kFarOut)
    reach = kFarOut;
  if (reach < -kFarOut)
    reach = -kFarOut;
  out.exponent = static_cast<int32_t>(reach);
  return trimTo(out, digits);
}

// One over a running value, at the same working width.
Running oneOver(const Running &value, unsigned digits) {
  Running out;
  out.sign = value.sign;
  if (xag::isZero(value.coefficient))
    return out;
  const unsigned below = digitsIn(value.coefficient);
  const unsigned lift = digits + below;
  U256 quotient, ignored;
  xag::divide(tenTo(lift), value.coefficient, quotient, ignored);
  out.coefficient = quotient;
  out.exponent = -value.exponent - static_cast<int32_t>(lift);
  return trimTo(out, digits);
}

// Declared before they are used, and written below where the layout is decided.
XagDeci encodeFinite(const Shape &shape, int sign, __uint128_t held128, int32_t biased);
Taken decodeBits(const Shape &shape, XagDeci bits);
XagDeci encodeSpecial(const Shape &shape, int sign, bool notANumber);

XagDeci packSpecial(uint32_t width, int sign, bool notANumber) {
  return encodeSpecial(shapeOf(width), sign, notANumber);
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

  return encodeFinite(shape, sign, held128, biased);
}

// ---- how a decimal is laid out in bits, which is the one thing an
// implementation is allowed to disagree about
//
// Everything above this line works in signs, coefficients and powers of ten,
// which is what a decimal *is*. Only these three functions know how those are
// written down — so a build that has a decimal unit can put them somewhere else
// and change nothing else. See `xag_deci_power.h`.

#ifndef XAG_DECIMAL_HARDWARE

// BID: the coefficient as an ordinary binary integer, because the wide
// arithmetic underneath already speaks that language.
XagDeci encodeSpecial(const Shape &shape, int sign, bool notANumber) {
  XagDeci bits = 0;
  const unsigned top = shape.bits - 1;
  if (sign)
    bits |= static_cast<XagDeci>(1) << top;
  // 11110 says infinity; 11111 says not a number.
  const XagDeci mark = notANumber ? 0x1F : 0x1E;
  bits |= mark << (top - 5);
  return bits;
}

XagDeci encodeFinite(const Shape &shape, int sign, __uint128_t held128, int32_t biased) {
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

Taken decodeBits(const Shape &shape, XagDeci bits) {
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
    // The leading digits of this form are `100` followed by one bit, so the
    // coefficient starts at 8 or 9 times 2^t — not 4 or 5. Writing `0x4` here
    // read back every coefficient above 8·2^t as a little over half of itself,
    // and only those: `9564953` came out as `5370649`, and every smaller
    // number in the ordinary form was untouched, which is why round-trips
    // looked fine.
    coefficient = (static_cast<__uint128_t>(0x8 | (g & 0x1)) << shape.trailing) | trailing;
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

#else
#include "xag_deci_power.h"
#endif // XAG_DECIMAL_HARDWARE

Taken take(uint32_t width, XagDeci bits) {
  return decodeBits(shapeOf(width), bits);
}

// Bring two finite numbers to one exponent, without letting the scaling run
// away: past the digits the format holds, more zeros change no answer.
// Both coefficients over one power of ten, as wide as they need to be.
//
// The catch is that lifting the larger one exactly can take more digits than a
// coefficient holds — and every digit past `p + 2` is one no answer keeps. So
// beyond that the smaller is stood in for by a single unit, which is all that
// rounding still wants from it: something below the last kept digit, pulling
// the right way, reaching no tie on its own. `capped` says that happened, for
// the caller that cares which.
//
// This used to clamp the lift and then write the answer at the smaller
// exponent anyway, which quietly divided the larger number by everything the
// clamp left out — `-1e119 - 0` came back as `-1e18`.
struct LinedUp {
  U256 a, b;
  int32_t exponent = 0;
  bool capped = false;
};

LinedUp lineUp(const Shape &shape, const Taken &x, const Taken &y) {
  LinedUp out{xag::wide(x.coefficient), xag::wide(y.coefficient), x.exponent, false};
  if (x.exponent == y.exponent)
    return out;

  const bool xIsHigh = x.exponent > y.exponent;
  const Taken &high = xIsHigh ? x : y;
  const Taken &low = xIsHigh ? y : x;
  U256 &highSide = xIsHigh ? out.a : out.b;
  U256 &lowSide = xIsHigh ? out.b : out.a;

  // Nothing at the larger power is still nothing, and standing it next to the
  // other one at its own exponent is exact. Reaching for the cap here instead
  // made `1e-86 + -0` come back as `1e-9`.
  if (high.coefficient == 0) {
    highSide = U256{};
    out.exponent = low.exponent;
    return out;
  }

  const int32_t gap = high.exponent - low.exponent;
  const int32_t most = static_cast<int32_t>(shape.digits) + 2;

  // How far the smaller one can still reach. Exponents alone do not say it: a
  // single digit at 10^25 and sixteen digits at 10^1 are two exponents apart by
  // twenty-four and nine apart as numbers, and the second one is well inside
  // what the answer keeps. Counting only the exponents dropped it.
  const int32_t reach = most +
      static_cast<int32_t>(digitsIn(xag::wide(low.coefficient))) -
      static_cast<int32_t>(digitsIn(xag::wide(high.coefficient)));

  // Whenever the smaller one still reaches, lifting the larger exactly is at
  // most `p + 2 + digits(low)` digits, which is inside what 256 bits hold.
  if (gap <= reach) {
    highSide = timesTenTo(xag::wide(high.coefficient), static_cast<unsigned>(gap));
    out.exponent = low.exponent;
    return out;
  }

  highSide = timesTenTo(xag::wide(high.coefficient), static_cast<unsigned>(most));
  lowSide = low.coefficient == 0 ? U256{} : xag::wide(1);
  out.exponent = high.exponent - most;
  out.capped = true;
  return out;
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

  // Addition prefers the smaller of the two exponents, which is what keeps
  // `1.10 + 2.00` at two places rather than trimming it to `3.1`.
  const LinedUp lined = lineUp(shape, x, y);

  if (x.sign == y.sign)
    return put(width, x.sign, xag::add(lined.a, lined.b), lined.exponent);

  const int order = xag::compare(lined.a, lined.b);
  if (order == 0)
    return put(width, 0, U256{}, lined.exponent); // a difference of nothing is +0
  if (order > 0)
    return put(width, x.sign, xag::subtract(lined.a, lined.b), lined.exponent);
  return put(width, y.sign, xag::subtract(lined.b, lined.a), lined.exponent);
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
  // Anything finite over an infinity is nothing at all, written as small as the
  // format can say it — which is what the standard prefers when one side has no
  // exponent to take the preference from.
  if (y.kind == Kind::Infinity)
    return put(width, sign, U256{}, -shape.bias);
  if (y.coefficient == 0)
    return x.coefficient == 0 ? packSpecial(width, 0, true)
                              : packSpecial(width, sign, false);
  if (x.coefficient == 0)
    return put(width, sign, U256{}, x.exponent - y.exponent);

  // Enough digits above to round on, and the remainder says whether the answer
  // stopped exactly or only nearly.
  //
  // How far to lift depends on both sides: a small number over a large one
  // loses digits off the top of the quotient before it starts. Lifting by a
  // fixed `p + 2` gave `10 / 0.072592` as `137.756`, four digits short of what
  // a `deci32` holds.
  const unsigned want = shape.digits + 2;
  const unsigned above = digitsIn(xag::wide(x.coefficient));
  const unsigned below = digitsIn(xag::wide(y.coefficient));
  const unsigned lift = want + below > above ? want + below - above : 0u;

  U256 quotient, left;
  xag::divide(timesTenTo(xag::wide(x.coefficient), lift), xag::wide(y.coefficient),
              quotient, left);
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
    // There is more of this number below the digits kept, so a last digit
    // sitting exactly on a tie has no right to it. Marking a trailing zero as
    // a one says "and something after that" without moving the value anywhere
    // rounding would notice.
    //
    // What stood here before doubled the quotient, added one and halved it
    // again, which is the same number back: the tie it meant to break survived
    // untouched.
    uint64_t lastDigit = 0;
    xag::divideSmall(quotient, 10, lastDigit);
    if (lastDigit == 0)
      quotient = xag::add(quotient, xag::wide(1));
  }
  return put(width, sign, quotient, exponent);
}

XagDeci xag_deci_mod(uint32_t width, XagDeci a, XagDeci b) {
  const Shape shape = shapeOf(width);
  const Taken x = take(width, a), y = take(width, b);

  if (x.kind == Kind::NotANumber || y.kind == Kind::NotANumber ||
      x.kind == Kind::Infinity ||
      (y.kind == Kind::Finite && y.coefficient == 0))
    return packSpecial(width, 0, true);

  // Nothing goes into an infinity a whole number of times, so what is left is
  // everything that was there.
  if (y.kind == Kind::Infinity)
    return a;

  // Nothing taken away from nothing, at the exponent the standard prefers.
  if (x.coefficient == 0)
    return put(width, x.sign, U256{},
               x.exponent < y.exponent ? x.exponent : y.exponent);

  // Both are put over the same power of ten, and then this is whole-number
  // arithmetic: the remainder of `X` by `Y`, written back at that power. Taking
  // multiples away one at a time is what this used to do, and at sixteen digits
  // a step that rounds stops making the number smaller — so `43584353828519647
  // mod 1e-4` went round for ever instead of answering.
  const int32_t under = x.exponent < y.exponent ? x.exponent : y.exponent;
  const unsigned liftX = static_cast<unsigned>(x.exponent - under);
  const unsigned liftY = static_cast<unsigned>(y.exponent - under);

  // How many digits each would have once lifted. Past what 256 bits hold, the
  // count of whole multiples is past what the format holds too, which the
  // standard calls an invalid operation rather than a number.
  const unsigned digitsX = digitsIn(xag::wide(x.coefficient)) + liftX;
  const unsigned digitsY = digitsIn(xag::wide(y.coefficient)) + liftY;
  if (digitsX >= kPowers || digitsY >= kPowers)
    return digitsY > digitsX + shape.digits ? a : packSpecial(width, 0, true);

  const U256 lifted = timesTenTo(xag::wide(x.coefficient), liftX);
  const U256 by = timesTenTo(xag::wide(y.coefficient), liftY);

  U256 whole, left;
  xag::divide(lifted, by, whole, left);

  // "The integer part of the quotient has no more than `p` digits, or there is
  // no answer" — which is what stops a remainder standing for a division nobody
  // could have written down.
  if (!xag::isZero(whole) && digitsIn(whole) > shape.digits)
    return packSpecial(width, 0, true);

  // What is left keeps the sign of what was divided, and is written at the
  // smaller of the two exponents, which is the one the standard prefers.
  return put(width, x.sign, left, under);
}

XagDeci xag_deci_pow(uint32_t width, XagDeci base, XagDeci exponent) {
  const Shape shape = shapeOf(width);
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
  // An exponent past anything worth multiplying out. Which way it goes is
  // settled by the size of what is being raised, not by the raising — so the
  // answer is still there to be had, and refusing to say it was wrong.
  if (scale != 0 || coefficient > 1000000000000000000ull) {
    const Taken b = take(width, base);
    if (b.kind == Kind::NotANumber)
      return packSpecial(width, 0, true);

    // An exponent this large is even unless it is a bare odd coefficient.
    const bool odd = scale == 0 && (coefficient & 1);
    const int sign = (b.sign && odd) ? 1 : 0;
    const bool up = !e.sign; // whether the exponent runs to the large end

    if (b.kind == Kind::Infinity)
      return up ? packSpecial(width, sign, false) : put(width, sign, U256{}, -shape.bias);
    if (b.coefficient == 0)
      return up ? put(width, sign, U256{}, 0) // exactly nothing, not a small one
                : packSpecial(width, sign, false); // nothing, raised the other way

    XagDeci unit = 0;
    xag_deci_reads(width, "1", 1, &unit);
    const XagDeci size = b.sign ? xag_deci_negate(width, base) : base;
    const int32_t against = xag_deci_compare(width, size, unit);
    if (against == 0) // one, however many times, is one
      return sign ? xag_deci_negate(width, unit) : unit;
    const bool away = (against > 0) == up; // growing rather than shrinking
    return away ? packSpecial(width, sign, false)
                : put(width, sign, U256{}, -shape.bias);
  }

  const Taken raised = take(width, base);
  if (raised.kind == Kind::Finite && raised.coefficient == 0) {
    if (coefficient == 0)
      return packSpecial(width, 0, true); // nothing raised to nothing says nothing
    if (e.sign) // one over nothing, however many times, and still signed
      return packSpecial(width, (raised.sign && (coefficient & 1)) ? 1 : 0, false);
    // A negative nothing stays negative an odd number of times over.
    return put(width, (raised.sign && (coefficient & 1)) ? 1 : 0, U256{}, 0);
  }

  // Four digits wider than the format, so that what each step rounds away
  // never reaches the digits the answer keeps.
  const unsigned work = shape.digits + 4;
  Running answer{xag::wide(1), 0, 0};
  Running running{xag::wide(raised.coefficient), raised.exponent, raised.sign};
  unsigned long long left = static_cast<unsigned long long>(coefficient);
  while (left > 0) {
    if (left & 1)
      answer = times(answer, running, work);
    left >>= 1;
    if (left)
      running = times(running, running, work);
  }
  if (e.sign)
    answer = oneOver(answer, work);

  // A power is written at the exponent the standard prefers: the base's own,
  // as many times over as it was raised. `-353.00 ^ 1` keeps its two places,
  // and `-0.90 ^ 6` reaches for twelve and stops at the seven a `deci32` has.
  const int64_t times = e.sign ? -static_cast<int64_t>(coefficient)
                               : static_cast<int64_t>(coefficient);
  int64_t ideal = static_cast<int64_t>(raised.exponent) * times;
  if (ideal > shape.maxExponent)
    ideal = shape.maxExponent;
  if (ideal < -shape.bias)
    ideal = -shape.bias;

  // Down toward it while there are only zeros to lose, and up toward it while
  // there is room for another digit.
  while (answer.exponent < ideal) {
    uint64_t over = 0;
    const U256 fewer = xag::divideSmall(answer.coefficient, 10, over);
    if (over != 0)
      break;
    answer.coefficient = fewer;
    ++answer.exponent;
  }
  while (answer.exponent > ideal && !xag::isZero(answer.coefficient) &&
         digitsIn(answer.coefficient) < shape.digits) {
    answer.coefficient = timesTen(answer.coefficient);
    --answer.exponent;
  }
  return put(width, answer.sign, answer.coefficient, answer.exponent);
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
  // One of them is nothing. Which way that goes is the other one's sign, and
  // nothing else — the reading below would put them over a common power of ten
  // first, where a zero says nothing about which is larger.
  if (xZero != yZero)
    return xZero ? (y.sign ? 1 : -1) : (x.sign ? -1 : 1);

  const LinedUp lined = lineUp(shape, x, y);
  // Past the cap the two cannot be equal, and neither can the smaller reach the
  // larger: with both coefficients non-zero, whichever stands at the greater
  // power is the greater number.
  const int order = lined.capped ? (x.exponent > y.exponent ? 1 : -1)
                                 : xag::compare(lined.a, lined.b);
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
uint64_t xag_deci_writes(char *out, uint64_t room, uint32_t width, XagDeci value) {
  const Taken x = take(width, value);
  if (x.kind == Kind::NotANumber)
    return xag_text_out(out, room, "not-a-number", 12);
  if (x.kind == Kind::Infinity)
    return x.sign ? xag_text_out(out, room, "-infinity", 9)
                  : xag_text_out(out, room, "infinity", 8);

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
  return xag_text_out(out, room, written, std::strlen(written));
}

void xag_print_deci(uint32_t width, XagDeci value) {
  char written[XAG_NUMBER_ROOM];
  (void)xag_deci_writes(written, sizeof(written), width, value);
  std::fputs(written, output());
}

} // extern "C"
