// The decimal layout and arithmetic a POWER machine already has.
//
// Included into `xag_deci.cpp` in place of the written-out BID encoding when
// `XAG_DECIMAL_HARDWARE` is asked for. Everything above the seam works in
// signs, coefficients and powers of ten, so only these three functions change —
// and the arithmetic below stops being written out at all.
//
// The unit stores a coefficient as DPD, three digits packed into ten bits.
// Nothing here has to know that: a coefficient crosses as a whole number
// through `dcffix` and `dctfix`, and the power it stands at through `diex` and
// `dxex`, which is what a decimal is in either encoding.
//
// What the unit does is add, subtract, multiply, divide and compare, at
// decimal64 and decimal128. A decimal32 is widened, worked out and rounded
// back. A remainder and a power have no instruction on any machine, so they
// stay as they are written above and reach the unit through these.

extern "C" {
uint64_t xag_dfp64_add(uint64_t a, uint64_t b);
uint64_t xag_dfp64_sub(uint64_t a, uint64_t b);
uint64_t xag_dfp64_mul(uint64_t a, uint64_t b);
uint64_t xag_dfp64_div(uint64_t a, uint64_t b);
int64_t xag_dfp64_compare(uint64_t a, uint64_t b);
uint64_t xag_dfp64_from_whole(int64_t value);
int64_t xag_dfp64_to_whole(uint64_t value);
uint64_t xag_dfp64_set_power(int64_t biased, uint64_t value);
int64_t xag_dfp64_power(uint64_t value);

void xag_dfp128_add(uint64_t ah, uint64_t al, uint64_t bh, uint64_t bl, uint64_t *out);
void xag_dfp128_sub(uint64_t ah, uint64_t al, uint64_t bh, uint64_t bl, uint64_t *out);
void xag_dfp128_mul(uint64_t ah, uint64_t al, uint64_t bh, uint64_t bl, uint64_t *out);
void xag_dfp128_div(uint64_t ah, uint64_t al, uint64_t bh, uint64_t bl, uint64_t *out);
int64_t xag_dfp128_compare(uint64_t ah, uint64_t al, uint64_t bh, uint64_t bl);
void xag_dfp128_from_halves(int64_t high17, int64_t low17, uint64_t *out);
int64_t xag_dfp128_high17(uint64_t hi, uint64_t lo);
int64_t xag_dfp128_low17(uint64_t hi, uint64_t lo);
void xag_dfp128_set_power(int64_t biased, uint64_t hi, uint64_t lo, uint64_t *out);
int64_t xag_dfp128_power(uint64_t hi, uint64_t lo);
uint64_t xag_dfp32_widen(uint32_t value);
uint32_t xag_dfp32_narrow(uint64_t value);
}

// Included inside the file's own unnamed namespace, so nothing here opens
// another one.
constexpr __uint128_t kSeventeen = 100000000000000000ULL; // 10^17

XagDeci signBitOf(const Shape &shape) {
  return static_cast<XagDeci>(1) << (shape.bits - 1);
}

// The five bits that say infinity or not-a-number sit in the same place and
// mean the same thing in both encodings, so this is the one piece that does not
// change at all.
XagDeci encodeSpecial(const Shape &shape, int sign, bool notANumber) {
  XagDeci bits = sign ? signBitOf(shape) : 0;
  bits |= static_cast<XagDeci>(notANumber ? 0x1F : 0x1E) << (shape.bits - 6);
  return bits;
}

XagDeci encodeFinite(const Shape &shape, int sign, __uint128_t held, int32_t biased) {
  XagDeci bits = 0;
  if (shape.digits == 34) {
    // Through the wide layer rather than `/` and `%`, so nothing here asks the
    // compiler for a 128-bit division routine it would have to link.
    U256 high256, low256;
    xag::divide(xag::wide(held), xag::wide(kSeventeen), high256, low256);
    const int64_t high = static_cast<int64_t>(xag::narrow(high256));
    const int64_t low = static_cast<int64_t>(xag::narrow(low256));
    uint64_t whole[2];
    xag_dfp128_from_halves(high, low, whole);
    uint64_t placed[2];
    xag_dfp128_set_power(biased, whole[0], whole[1], placed);
    bits = (static_cast<XagDeci>(placed[0]) << 64) | placed[1];
  } else {
    const uint64_t whole = xag_dfp64_from_whole(static_cast<int64_t>(held));
    const uint64_t placed = xag_dfp64_set_power(shape.digits == 7 ? biased + 297 : biased,
                                                whole);
    bits = shape.digits == 7 ? static_cast<XagDeci>(xag_dfp32_narrow(placed))
                             : static_cast<XagDeci>(placed);
  }
  // Built without a sign and given one, because the bit sits in the same place
  // whatever holds the digits.
  return sign ? (bits | signBitOf(shape)) : bits;
}

Taken decodeBits(const Shape &shape, XagDeci bits) {
  Taken out;
  const unsigned top = shape.bits - 1;
  out.sign = static_cast<int>((bits >> top) & 1);
  const unsigned g = static_cast<unsigned>((bits >> (top - 5)) & 0x1F);
  if ((g & 0x1E) == 0x1E) {
    out.kind = (g & 0x1) ? Kind::NotANumber : Kind::Infinity;
    return out;
  }

  const XagDeci without = bits & ~signBitOf(shape);
  if (shape.digits == 34) {
    const uint64_t hi = static_cast<uint64_t>(without >> 64);
    const uint64_t lo = static_cast<uint64_t>(without);
    out.exponent = static_cast<int32_t>(xag_dfp128_power(hi, lo)) - shape.bias;
    uint64_t flat[2];
    xag_dfp128_set_power(shape.bias, hi, lo, flat); // stand it at ten to the nought
    const __uint128_t high =
        static_cast<__uint128_t>(xag_dfp128_high17(flat[0], flat[1]));
    const __uint128_t low =
        static_cast<__uint128_t>(xag_dfp128_low17(flat[0], flat[1]));
    out.coefficient = xag::narrow(
        xag::add(xag::multiply(high, kSeventeen), xag::wide(low)));
  } else {
    const uint64_t wide = shape.digits == 7
                              ? xag_dfp32_widen(static_cast<uint32_t>(without))
                              : static_cast<uint64_t>(without);
    const int32_t bias64 = 398;
    out.exponent = static_cast<int32_t>(xag_dfp64_power(wide)) - bias64;
    if (shape.digits == 7)
      out.exponent += 0; // widening keeps the power it stood at
    out.coefficient =
        static_cast<__uint128_t>(xag_dfp64_to_whole(xag_dfp64_set_power(bias64, wide)));
  }
  return out;
}

// ---- the arithmetic, which is now four instructions and a widening
//
// These do not go through a coefficient at all: the unit holds the number and
// answers about it, which is the whole reason to ask for this build.

XagDeci onTheUnit(uint32_t width, XagDeci a, XagDeci b, unsigned which) {
  if (width == 128) {
    uint64_t out[2];
    const uint64_t ah = static_cast<uint64_t>(a >> 64), al = static_cast<uint64_t>(a);
    const uint64_t bh = static_cast<uint64_t>(b >> 64), bl = static_cast<uint64_t>(b);
    switch (which) {
    case 0: xag_dfp128_add(ah, al, bh, bl, out); break;
    case 1: xag_dfp128_sub(ah, al, bh, bl, out); break;
    case 2: xag_dfp128_mul(ah, al, bh, bl, out); break;
    default: xag_dfp128_div(ah, al, bh, bl, out); break;
    }
    return (static_cast<XagDeci>(out[0]) << 64) | out[1];
  }

  const bool narrow = width == 32;
  const uint64_t x = narrow ? xag_dfp32_widen(static_cast<uint32_t>(a))
                            : static_cast<uint64_t>(a);
  const uint64_t y = narrow ? xag_dfp32_widen(static_cast<uint32_t>(b))
                            : static_cast<uint64_t>(b);
  const uint64_t got = which == 0   ? xag_dfp64_add(x, y)
                       : which == 1 ? xag_dfp64_sub(x, y)
                       : which == 2 ? xag_dfp64_mul(x, y)
                                    : xag_dfp64_div(x, y);
  return narrow ? static_cast<XagDeci>(xag_dfp32_narrow(got))
                : static_cast<XagDeci>(got);
}
