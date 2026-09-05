// IEEE 754 binary128, written out because this machine's compiler has none.
//
// A value is carried as its bits and taken apart when it is worked on: a sign,
// an integer significand, and the power of two that scales it. Everything
// rounds to nearest with ties to even, which is what the standard means when it
// says nothing else.

#include "xag_runtime.h"
#include "xag_wide.h"

#include <cstdio>
#include <cstring>

namespace {
std::FILE *output() { return static_cast<std::FILE *>(xag_output_file()); }
} // namespace

namespace {

using xag::U256;

constexpr unsigned kPrecision = 113;   // bits of significand, implicit one included
constexpr int32_t kBias = 16383;
constexpr int32_t kMaxExponent = 16383;
constexpr int32_t kMinExponent = -16382;
constexpr unsigned kFractionBits = 112;
constexpr unsigned kWorking = 240; // room to be wrong in, well below the last bit

enum class Kind { Zero, Finite, Infinity, NotANumber };

struct Taken {
  int sign = 0;
  Kind kind = Kind::Zero;
  __uint128_t significand = 0; // an integer
  int32_t exponent = 0;        // value = significand * 2^exponent
};

__uint128_t fractionMask() {
  return (static_cast<__uint128_t>(1) << kFractionBits) - 1;
}

Taken take(XagBin128 bits) {
  Taken out;
  out.sign = static_cast<int>((bits >> 127) & 1);
  const int32_t stored = static_cast<int32_t>((bits >> kFractionBits) & 0x7FFF);
  const __uint128_t fraction = bits & fractionMask();

  if (stored == 0x7FFF) {
    out.kind = fraction ? Kind::NotANumber : Kind::Infinity;
    return out;
  }
  if (stored == 0) {
    if (fraction == 0)
      return out; // a zero, with its sign kept
    out.kind = Kind::Finite;
    out.significand = fraction;
    out.exponent = kMinExponent - static_cast<int32_t>(kFractionBits);
    return out;
  }
  out.kind = Kind::Finite;
  out.significand = fraction | (static_cast<__uint128_t>(1) << kFractionBits);
  out.exponent = stored - kBias - static_cast<int32_t>(kFractionBits);
  return out;
}

XagBin128 zero(int sign) {
  return static_cast<XagBin128>(sign) << 127;
}

XagBin128 infinity(int sign) {
  return (static_cast<XagBin128>(sign) << 127) |
         (static_cast<XagBin128>(0x7FFF) << kFractionBits);
}

XagBin128 notANumber() {
  return (static_cast<XagBin128>(0x7FFF) << kFractionBits) |
         (static_cast<XagBin128>(1) << (kFractionBits - 1));
}

// The one place rounding happens: a significand of any width, the power of two
// that scales it, and whether anything was already thrown away below it.
XagBin128 put(int sign, U256 significand, int32_t exponent, bool sticky) {
  if (xag::isZero(significand) && !sticky)
    return zero(sign);

  unsigned width = xag::bits(significand);

  // Bring it down to the precision the format has, keeping what falls off.
  if (width > kPrecision) {
    const unsigned drop = width - kPrecision;
    if (xag::anyBelow(significand, drop))
      sticky = true;
    const bool half = xag::bitAt(significand, drop - 1);
    if (drop > 1 && xag::anyBelow(significand, drop - 1))
      sticky = true;
    significand = xag::shiftRight(significand, drop);
    exponent += static_cast<int32_t>(drop);
    // To nearest, and a tie goes to the even one.
    if (half && (sticky || xag::bitAt(significand, 0))) {
      significand = xag::add(significand, xag::wide(1));
      if (xag::bits(significand) > kPrecision) {
        significand = xag::shiftRight(significand, 1);
        exponent += 1;
      }
    }
    width = xag::bits(significand);
  }

  // And up, if it is short of it and there is room in the exponent.
  while (width < kPrecision && exponent + static_cast<int32_t>(width) - 1 > kMinExponent) {
    significand = xag::shiftLeft(significand, 1);
    exponent -= 1;
    width += 1;
  }

  // Where the leading bit sits once the significand is read as 1.xxx
  int32_t unbiased = exponent + static_cast<int32_t>(width) - 1;

  if (width < kPrecision) {
    // Subnormal: no implicit one, and the exponent is pinned.
    const int32_t pinned = kMinExponent - static_cast<int32_t>(kFractionBits);
    if (exponent < pinned) {
      const unsigned drop = static_cast<unsigned>(pinned - exponent);
      if (xag::anyBelow(significand, drop))
        sticky = true;
      const bool half = drop > 0 && xag::bitAt(significand, drop - 1);
      significand = xag::shiftRight(significand, drop);
      if (half && (sticky || xag::bitAt(significand, 0)))
        significand = xag::add(significand, xag::wide(1));
    } else {
      significand = xag::shiftLeft(significand, static_cast<unsigned>(exponent - pinned));
    }
    const __uint128_t kept = xag::narrow(significand);
    if (kept >> kFractionBits)
      return (static_cast<XagBin128>(sign) << 127) |
             (static_cast<XagBin128>(1) << kFractionBits) | (kept & fractionMask());
    return (static_cast<XagBin128>(sign) << 127) | kept;
  }

  if (unbiased > kMaxExponent)
    return infinity(sign);
  if (unbiased < kMinExponent) {
    // Fell below what a normal can say: try again as a subnormal.
    const int32_t pinned = kMinExponent - static_cast<int32_t>(kFractionBits);
    const unsigned drop = static_cast<unsigned>(pinned - exponent);
    if (drop >= 256)
      return zero(sign);
    if (xag::anyBelow(significand, drop))
      sticky = true;
    const bool half = drop > 0 && xag::bitAt(significand, drop - 1);
    U256 kept = xag::shiftRight(significand, drop);
    if (half && (sticky || xag::bitAt(kept, 0)))
      kept = xag::add(kept, xag::wide(1));
    const __uint128_t held = xag::narrow(kept);
    if (held >> kFractionBits) // rounded its way back up to the smallest normal
      return (static_cast<XagBin128>(sign) << 127) |
             (static_cast<XagBin128>(1) << kFractionBits) | (held & fractionMask());
    return (static_cast<XagBin128>(sign) << 127) | held;
  }

  const __uint128_t held = xag::narrow(significand) & fractionMask();
  return (static_cast<XagBin128>(sign) << 127) |
         (static_cast<XagBin128>(unbiased + kBias) << kFractionBits) | held;
}

// ---- what the four operations are made of

XagBin128 sum(Taken a, Taken b, int flip) {
  b.sign ^= flip;

  if (a.kind == Kind::NotANumber || b.kind == Kind::NotANumber)
    return notANumber();
  if (a.kind == Kind::Infinity || b.kind == Kind::Infinity) {
    if (a.kind == Kind::Infinity && b.kind == Kind::Infinity)
      return a.sign == b.sign ? infinity(a.sign) : notANumber();
    return infinity(a.kind == Kind::Infinity ? a.sign : b.sign);
  }
  if (a.kind == Kind::Zero && b.kind == Kind::Zero)
    return zero(a.sign & b.sign); // −0 only when both were
  if (a.kind == Kind::Zero)
    return put(b.sign, xag::wide(b.significand), b.exponent, false);
  if (b.kind == Kind::Zero)
    return put(a.sign, xag::wide(a.significand), a.exponent, false);

  // Three bits of room below, so a tie can be told from something near one.
  U256 x = xag::shiftLeft(xag::wide(a.significand), 3);
  U256 y = xag::shiftLeft(xag::wide(b.significand), 3);
  int32_t ex = a.exponent - 3, ey = b.exponent - 3;
  bool sticky = false;

  if (ex < ey) {
    U256 t = x; x = y; y = t;
    int32_t te = ex; ex = ey; ey = te;
    const int ts = a.sign; a.sign = b.sign; b.sign = ts;
  }
  const int32_t apart = ex - ey;
  if (apart > 0) {
    if (apart >= 256) {
      sticky = !xag::isZero(y);
      y = U256{};
    } else {
      if (xag::anyBelow(y, static_cast<unsigned>(apart)))
        sticky = true;
      y = xag::shiftRight(y, static_cast<unsigned>(apart));
    }
  }

  if (a.sign == b.sign)
    return put(a.sign, xag::add(x, y), ex, sticky);

  const int order = xag::compare(x, y);
  if (order == 0 && !sticky)
    return zero(0);
  if (order >= 0)
    return put(a.sign, xag::subtract(x, y), ex, sticky);
  return put(b.sign, xag::subtract(y, x), ex, sticky);
}

// Multiplying or dividing by ten, done in far more bits than the format has so
// that thousands of steps still land inside the last bit that matters.
void byPowerOfTen(U256 &significand, int32_t &exponent, bool &sticky, int32_t power) {
  while (power > 0) {
    if (xag::bits(significand) > kWorking) {
      const unsigned drop = xag::bits(significand) - kWorking;
      if (xag::anyBelow(significand, drop))
        sticky = true;
      significand = xag::shiftRight(significand, drop);
      exponent += static_cast<int32_t>(drop);
    }
    significand = xag::add(xag::shiftLeft(significand, 3), xag::shiftLeft(significand, 1));
    --power;
  }
  while (power < 0) {
    const unsigned width = xag::bits(significand);
    if (width < kWorking) {
      const unsigned lift = kWorking - width;
      significand = xag::shiftLeft(significand, lift);
      exponent -= static_cast<int32_t>(lift);
    }
    uint64_t left = 0;
    significand = xag::divideSmall(significand, 10, left);
    if (left != 0)
      sticky = true;
    ++power;
  }
}

// About how many digits stand before the point.
int32_t roughlyLog10(const Taken &value) {
  const int32_t top = value.exponent +
                      static_cast<int32_t>(xag::bits(xag::wide(value.significand))) - 1;
  // log10(2) is near enough at this precision, and the answer is corrected after.
  return static_cast<int32_t>(static_cast<double>(top) * 0.30102999566398119521);
}

} // namespace

extern "C" {

XagBin128 xag_bin128_add(XagBin128 a, XagBin128 b) { return sum(take(a), take(b), 0); }
XagBin128 xag_bin128_sub(XagBin128 a, XagBin128 b) { return sum(take(a), take(b), 1); }

XagBin128 xag_bin128_mul(XagBin128 a, XagBin128 b) {
  const Taken x = take(a), y = take(b);
  const int sign = x.sign ^ y.sign;
  if (x.kind == Kind::NotANumber || y.kind == Kind::NotANumber)
    return notANumber();
  if (x.kind == Kind::Infinity || y.kind == Kind::Infinity) {
    if (x.kind == Kind::Zero || y.kind == Kind::Zero)
      return notANumber(); // nothing times everything
    return infinity(sign);
  }
  if (x.kind == Kind::Zero || y.kind == Kind::Zero)
    return zero(sign);
  return put(sign, xag::multiply(x.significand, y.significand), x.exponent + y.exponent,
             false);
}

XagBin128 xag_bin128_div(XagBin128 a, XagBin128 b) {
  const Taken x = take(a), y = take(b);
  const int sign = x.sign ^ y.sign;
  if (x.kind == Kind::NotANumber || y.kind == Kind::NotANumber)
    return notANumber();
  if (x.kind == Kind::Infinity)
    return y.kind == Kind::Infinity ? notANumber() : infinity(sign);
  if (y.kind == Kind::Infinity)
    return zero(sign);
  if (y.kind == Kind::Zero)
    return x.kind == Kind::Zero ? notANumber() : infinity(sign);
  if (x.kind == Kind::Zero)
    return zero(sign);

  // Enough room above for a quotient with bits to spare, and the remainder
  // says whether anything was left over.
  constexpr unsigned kLift = 140;
  U256 quotient, left;
  xag::divide(xag::shiftLeft(xag::wide(x.significand), kLift), xag::wide(y.significand),
              quotient, left);
  return put(sign, quotient, x.exponent - y.exponent - static_cast<int32_t>(kLift),
             !xag::isZero(left));
}

// What is left over after taking away as many whole multiples as will go.
XagBin128 xag_bin128_mod(XagBin128 a, XagBin128 b) {
  const Taken x = take(a), y = take(b);
  if (x.kind == Kind::NotANumber || y.kind == Kind::NotANumber ||
      x.kind == Kind::Infinity || y.kind == Kind::Zero)
    return notANumber();
  if (y.kind == Kind::Infinity || x.kind == Kind::Zero)
    return a;

  XagBin128 left = a;
  const XagBin128 size = b & ~(static_cast<XagBin128>(1) << 127); // its magnitude
  const int sign = x.sign;
  // Take away the largest doubling that still fits, over and over.
  while (true) {
    const Taken here = take(left);
    if (here.kind != Kind::Finite || here.significand == 0)
      break;
    XagBin128 step = size;
    if (xag_bin128_compare(step, left & ~(static_cast<XagBin128>(1) << 127)) > 0)
      break;
    while (true) {
      const XagBin128 twice = xag_bin128_add(step, step);
      if (xag_bin128_compare(twice, left & ~(static_cast<XagBin128>(1) << 127)) > 0)
        break;
      step = twice;
    }
    left = sign ? xag_bin128_add(left, step) : xag_bin128_sub(left, step);
  }
  return left;
}

// Whether a number is whole, and what whole number it is.
bool wholeValue(const Taken &x, long long &out) {
  if (x.kind != Kind::Finite)
    return false;
  if (x.significand == 0) {
    out = 0;
    return true;
  }
  __uint128_t significand = x.significand;
  int32_t exponent = x.exponent;
  while (exponent < 0) {
    if (significand & 1)
      return false; // a fraction, and no power of ours takes one
    significand >>= 1;
    ++exponent;
  }
  while (exponent > 0) {
    if (significand > (static_cast<__uint128_t>(1) << 62))
      return false; // far larger than any exponent worth raising to
    significand <<= 1;
    --exponent;
  }
  if (significand > static_cast<__uint128_t>(1) << 62)
    return false;
  out = static_cast<long long>(significand);
  if (x.sign)
    out = -out;
  return true;
}

XagBin128 xag_bin128_pow(XagBin128 base, XagBin128 exponent) {
  const Taken e = take(exponent);
  long long times = 0;
  if (!wholeValue(e, times))
    return notANumber();

  XagBin128 one = 0;
  xag_bin128_reads("1", 1, &one);
  XagBin128 answer = one;
  XagBin128 running = base;
  unsigned long long left =
      times < 0 ? static_cast<unsigned long long>(-times) : static_cast<unsigned long long>(times);
  while (left > 0) {
    if (left & 1)
      answer = xag_bin128_mul(answer, running);
    running = xag_bin128_mul(running, running);
    left >>= 1;
  }
  return times < 0 ? xag_bin128_div(one, answer) : answer;
}

// -2, -1, 0 or 1, and -3 when the two cannot be ordered at all.
int32_t xag_bin128_compare(XagBin128 a, XagBin128 b) {
  const Taken x = take(a), y = take(b);
  if (x.kind == Kind::NotANumber || y.kind == Kind::NotANumber)
    return -3;
  if (x.kind == Kind::Zero && y.kind == Kind::Zero)
    return 0; // and −0 is 0
  if (x.kind == Kind::Infinity || y.kind == Kind::Infinity) {
    const int xv = x.kind == Kind::Infinity ? (x.sign ? -2 : 2) : (x.sign ? -1 : 1);
    const int yv = y.kind == Kind::Infinity ? (y.sign ? -2 : 2) : (y.sign ? -1 : 1);
    if (x.kind == Kind::Infinity && y.kind == Kind::Infinity)
      return xv == yv ? 0 : (xv < yv ? -1 : 1);
    return xv < yv ? -1 : (xv > yv ? 1 : 0);
  }
  if (x.sign != y.sign)
    return x.sign ? -1 : 1;

  // Same sign, so the larger magnitude decides, and the sign turns it round.
  const int32_t xTop = x.exponent + static_cast<int32_t>(xag::bits(xag::wide(x.significand)));
  const int32_t yTop = y.exponent + static_cast<int32_t>(xag::bits(xag::wide(y.significand)));
  int order;
  if (x.kind == Kind::Zero)
    order = y.kind == Kind::Zero ? 0 : -1;
  else if (y.kind == Kind::Zero)
    order = 1;
  else if (xTop != yTop)
    order = xTop < yTop ? -1 : 1;
  else {
    // Line them up and look.
    const int32_t low = x.exponent < y.exponent ? x.exponent : y.exponent;
    const U256 xs = xag::shiftLeft(xag::wide(x.significand),
                                   static_cast<unsigned>(x.exponent - low));
    const U256 ys = xag::shiftLeft(xag::wide(y.significand),
                                   static_cast<unsigned>(y.exponent - low));
    order = xag::compare(xs, ys);
  }
  return x.sign ? -order : order;
}

XagBin128 xag_bin128_from_double(double value) {
  if (value != value)
    return notANumber();
  uint64_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  const int sign = static_cast<int>(bits >> 63);
  const int32_t stored = static_cast<int32_t>((bits >> 52) & 0x7FF);
  const uint64_t fraction = bits & ((static_cast<uint64_t>(1) << 52) - 1);
  if (stored == 0x7FF)
    return infinity(sign);
  if (stored == 0 && fraction == 0)
    return zero(sign);
  const __uint128_t significand =
      stored == 0 ? fraction : (fraction | (static_cast<uint64_t>(1) << 52));
  const int32_t exponent = (stored == 0 ? 1 : stored) - 1023 - 52;
  return put(sign, xag::wide(significand), exponent, false);
}

double xag_bin128_to_double(XagBin128 value) {
  const Taken x = take(value);
  if (x.kind == Kind::NotANumber)
    return 0.0 / 0.0;
  if (x.kind == Kind::Infinity)
    return x.sign ? -1.0 / 0.0 : 1.0 / 0.0;
  if (x.kind == Kind::Zero)
    return x.sign ? -0.0 : 0.0;
  double answer = 0;
  // Built from the top down, which keeps every bit that a double can hold.
  const unsigned width = xag::bits(xag::wide(x.significand));
  const unsigned keep = width > 53 ? width - 53 : 0;
  __uint128_t significand = x.significand;
  int32_t exponent = x.exponent;
  if (keep) {
    const __uint128_t dropped = significand & ((static_cast<__uint128_t>(1) << keep) - 1);
    significand >>= keep;
    exponent += static_cast<int32_t>(keep);
    const __uint128_t half = static_cast<__uint128_t>(1) << (keep - 1);
    if (dropped > half || (dropped == half && (significand & 1)))
      significand += 1;
  }
  answer = static_cast<double>(static_cast<unsigned long long>(significand));
  return x.sign ? -__builtin_ldexp(answer, exponent) : __builtin_ldexp(answer, exponent);
}

int32_t xag_bin128_reads(const char *text, uint64_t length, XagBin128 *out) {
  char buffer[600];
  if (length + 1 > sizeof(buffer))
    return 0;
  std::memcpy(buffer, text, length);
  buffer[length] = 0;

  auto answer = [&](XagBin128 value) {
    if (out)
      *out = value;
    return 1;
  };
  if (std::strcmp(buffer, "infinity") == 0)
    return answer(infinity(0));
  if (std::strcmp(buffer, "-infinity") == 0)
    return answer(infinity(1));
  if (std::strcmp(buffer, "not-a-number") == 0)
    return answer(notANumber());

  const char *at = buffer;
  int sign = 0;
  if (*at == '+' || *at == '-') {
    sign = *at == '-';
    ++at;
  }

  U256 significand;
  bool sticky = false;
  int32_t decimals = 0;
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
    if (digits < 45) {
      significand = xag::add(xag::add(xag::shiftLeft(significand, 3),
                                      xag::shiftLeft(significand, 1)),
                             xag::wide(static_cast<unsigned>(*at - '0')));
      ++digits;
      if (sawPoint)
        --decimals;
    } else {
      // Past what is worth keeping; it only matters that something was there.
      if (*at != '0')
        sticky = true;
      if (!sawPoint)
        ++decimals;
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
        written = 100000; // far past anything the format can say
    }
    decimals += expSign * written;
  }
  if (*at != 0)
    return 0;

  int32_t exponent = 0;
  byPowerOfTen(significand, exponent, sticky, decimals);
  return answer(put(sign, significand, exponent, sticky));
}

void xag_print_bin128(XagBin128 value) {
  const Taken x = take(value);
  if (x.kind == Kind::NotANumber) {
    std::fputs("not-a-number", output());
    return;
  }
  if (x.kind == Kind::Infinity) {
    std::fputs(x.sign ? "-infinity" : "infinity", output());
    return;
  }
  if (x.kind == Kind::Zero) {
    std::fputs(x.sign ? "-0" : "0", output());
    return;
  }

  // Thirty-six digits is what binary128 needs before a spelling is certain to
  // read back; fewer are tried afterwards, and the shortest that survives wins.
  constexpr int kMost = 36;
  int32_t place = roughlyLog10(x);
  char digits[64];
  int32_t at = 0;

  for (int attempt = 0; attempt < 3; ++attempt) {
    U256 significand = xag::wide(x.significand);
    int32_t exponent = x.exponent;
    bool sticky = false;
    byPowerOfTen(significand, exponent, sticky, kMost - 1 - place);
    if (exponent > 0) {
      significand = xag::shiftLeft(significand, static_cast<unsigned>(exponent));
    } else if (exponent < 0) {
      const unsigned drop = static_cast<unsigned>(-exponent);
      const bool half = drop > 0 && drop <= 256 && xag::bitAt(significand, drop - 1);
      const bool below = drop > 1 && xag::anyBelow(significand, drop - 1);
      significand = drop >= 256 ? U256{} : xag::shiftRight(significand, drop);
      if (half && (below || sticky || xag::bitAt(significand, 0)))
        significand = xag::add(significand, xag::wide(1));
    }

    at = 0;
    U256 left = significand;
    while (!xag::isZero(left) && at < static_cast<int32_t>(sizeof(digits))) {
      uint64_t remainder = 0;
      left = xag::divideSmall(left, 10, remainder);
      digits[at++] = static_cast<char>('0' + static_cast<unsigned>(remainder));
    }
    if (at == kMost)
      break;
    place += at - kMost; // the estimate was a digit out; say so and try again
  }
  // digits[] runs backwards; put it the way round a reader expects.
  for (int32_t i = 0; i < at / 2; ++i) {
    const char keep = digits[i];
    digits[i] = digits[at - 1 - i];
    digits[at - 1 - i] = keep;
  }

  // The shortest spelling that reads back as this very value.
  char written[80];
  for (int32_t keep = 1; keep <= at; ++keep) {
    char rounded[64];
    std::memcpy(rounded, digits, static_cast<size_t>(at));
    int32_t shown = keep, exponent = place;
    if (keep < at && rounded[keep] >= '5') {
      int32_t i = keep - 1;
      while (i >= 0 && rounded[i] == '9')
        rounded[i--] = '0';
      if (i < 0) {
        std::memmove(rounded + 1, rounded, static_cast<size_t>(at));
        rounded[0] = '1';
        ++exponent;
      } else {
        ++rounded[i];
      }
    }
    while (shown > 1 && rounded[shown - 1] == '0')
      --shown;

    char *put = written;
    if (x.sign)
      *put++ = '-';
    if (exponent >= -5 && exponent < 21) {
      if (exponent >= 0) {
        for (int32_t i = 0; i <= exponent; ++i)
          *put++ = i < shown ? rounded[i] : '0';
        if (shown > exponent + 1) {
          *put++ = '.';
          for (int32_t i = exponent + 1; i < shown; ++i)
            *put++ = rounded[i];
        }
      } else {
        *put++ = '0';
        *put++ = '.';
        for (int32_t i = 0; i < -exponent - 1; ++i)
          *put++ = '0';
        for (int32_t i = 0; i < shown; ++i)
          *put++ = rounded[i];
      }
    } else {
      *put++ = rounded[0];
      if (shown > 1) {
        *put++ = '.';
        for (int32_t i = 1; i < shown; ++i)
          *put++ = rounded[i];
      }
      *put++ = 'e';
      int32_t e = exponent;
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

    XagBin128 back = 0;
    if (xag_bin128_reads(written, static_cast<uint64_t>(put - written), &back) &&
        back == value) {
      std::fputs(written, output());
      return;
    }
  }
  std::fputs(written, output());
}

} // extern "C"
