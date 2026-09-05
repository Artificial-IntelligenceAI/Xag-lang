// Wide unsigned arithmetic, for the formats a machine has no register for.
//
// Not part of the runtime's public face: binary128 needs it, decimal will need
// it, and nothing outside this directory should.
//
// 226 bits is where the requirement comes from — two 113-bit significands
// multiplied — so 256 is the smallest round number that holds a product.

#ifndef XAG_WIDE_H
#define XAG_WIDE_H

#include <stdint.h>

namespace xag {

struct U256 {
  uint64_t w[4] = {0, 0, 0, 0}; // little end first
};

inline U256 wide(__uint128_t value) {
  U256 out;
  out.w[0] = static_cast<uint64_t>(value);
  out.w[1] = static_cast<uint64_t>(value >> 64);
  return out;
}

inline __uint128_t narrow(const U256 &value) {
  return (static_cast<__uint128_t>(value.w[1]) << 64) | value.w[0];
}

inline bool isZero(const U256 &value) {
  return (value.w[0] | value.w[1] | value.w[2] | value.w[3]) == 0;
}

inline int compare(const U256 &a, const U256 &b) {
  for (int i = 3; i >= 0; --i)
    if (a.w[i] != b.w[i])
      return a.w[i] < b.w[i] ? -1 : 1;
  return 0;
}

inline U256 add(const U256 &a, const U256 &b) {
  U256 out;
  unsigned __int128 carry = 0;
  for (int i = 0; i < 4; ++i) {
    const unsigned __int128 sum =
        static_cast<unsigned __int128>(a.w[i]) + b.w[i] + carry;
    out.w[i] = static_cast<uint64_t>(sum);
    carry = sum >> 64;
  }
  return out;
}

inline U256 subtract(const U256 &a, const U256 &b) {
  U256 out;
  unsigned __int128 borrow = 0;
  for (int i = 0; i < 4; ++i) {
    const unsigned __int128 taken =
        static_cast<unsigned __int128>(a.w[i]) - b.w[i] - borrow;
    out.w[i] = static_cast<uint64_t>(taken);
    borrow = (taken >> 64) ? 1 : 0;
  }
  return out;
}

// The full product of two 128-bit numbers, which is where 256 bits are wanted.
inline U256 multiply(__uint128_t a, __uint128_t b) {
  const uint64_t parts[4] = {static_cast<uint64_t>(a), static_cast<uint64_t>(a >> 64),
                             static_cast<uint64_t>(b), static_cast<uint64_t>(b >> 64)};
  U256 out;
  for (int i = 0; i < 2; ++i) {
    unsigned __int128 carry = 0;
    for (int j = 0; j < 2; ++j) {
      const unsigned __int128 here =
          static_cast<unsigned __int128>(parts[i]) * parts[2 + j] + out.w[i + j] + carry;
      out.w[i + j] = static_cast<uint64_t>(here);
      carry = here >> 64;
    }
    out.w[i + 2] += static_cast<uint64_t>(carry);
  }
  return out;
}

inline U256 shiftLeft(const U256 &value, unsigned by) {
  U256 out;
  if (by >= 256)
    return out;
  const unsigned words = by / 64, bits = by % 64;
  for (int i = 3; i >= 0; --i) {
    uint64_t here = 0;
    const int from = i - static_cast<int>(words);
    if (from >= 0) {
      here = value.w[from] << bits;
      if (bits && from > 0)
        here |= value.w[from - 1] >> (64 - bits);
    }
    out.w[i] = here;
  }
  return out;
}

inline U256 shiftRight(const U256 &value, unsigned by) {
  U256 out;
  if (by >= 256)
    return out;
  const unsigned words = by / 64, bits = by % 64;
  for (int i = 0; i < 4; ++i) {
    uint64_t here = 0;
    const unsigned from = i + words;
    if (from < 4) {
      here = value.w[from] >> bits;
      if (bits && from + 1 < 4)
        here |= value.w[from + 1] << (64 - bits);
    }
    out.w[i] = here;
  }
  return out;
}

// One past the highest bit that is set, or zero.
inline unsigned bits(const U256 &value) {
  for (int i = 3; i >= 0; --i)
    if (value.w[i])
      return static_cast<unsigned>(i) * 64 + (64 - __builtin_clzll(value.w[i]));
  return 0;
}

inline bool bitAt(const U256 &value, unsigned at) {
  return at < 256 && ((value.w[at / 64] >> (at % 64)) & 1);
}

// Dividing by something that fits in one limb, which is most of what decimal
// asks for. Limb by limb with a 128-bit remainder, rather than bit by bit.
inline U256 divideSmall(const U256 &top, uint64_t bottom, uint64_t &left) {
  U256 out;
  unsigned __int128 carry = 0;
  for (int i = 3; i >= 0; --i) {
    const unsigned __int128 here = (carry << 64) | top.w[i];
    out.w[i] = static_cast<uint64_t>(here / bottom);
    carry = here % bottom;
  }
  left = static_cast<uint64_t>(carry);
  return out;
}

// Shift and subtract, which is slow and short and hard to be wrong about.
inline void divide(const U256 &top, const U256 &bottom, U256 &quotient, U256 &left) {
  quotient = U256{};
  left = U256{};
  if (isZero(bottom))
    return;
  for (int i = static_cast<int>(bits(top)) - 1; i >= 0; --i) {
    left = shiftLeft(left, 1);
    if (bitAt(top, static_cast<unsigned>(i)))
      left.w[0] |= 1;
    if (compare(left, bottom) >= 0) {
      left = subtract(left, bottom);
      quotient.w[i / 64] |= static_cast<uint64_t>(1) << (i % 64);
    }
  }
}

// Whether anything was thrown away below `by` bits — the sticky bit, which is
// what tells a halfway case from one that only looks like one.
inline bool anyBelow(const U256 &value, unsigned by) {
  for (unsigned i = 0; i < by && i < 256; ++i)
    if (bitAt(value, i))
      return true;
  return false;
}

} // namespace xag

#endif
