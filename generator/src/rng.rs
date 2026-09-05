//! xoshiro256** — small, fast, and good enough for choosing what to write next.
//!
//! Seeded per case rather than per run, so any finding can be reproduced from
//! its seed alone without keeping the program around.

pub struct Rng {
    state: [u64; 4],
}

impl Rng {
    pub fn from_seed(seed: u64) -> Rng {
        // SplitMix64 to spread one number into four that are not obviously related.
        let mut x = seed.wrapping_add(0x9E37_79B9_7F4A_7C15);
        let mut next = || {
            x = x.wrapping_add(0x9E37_79B9_7F4A_7C15);
            let mut z = x;
            z = (z ^ (z >> 30)).wrapping_mul(0xBF58_476D_1CE4_E5B9);
            z = (z ^ (z >> 27)).wrapping_mul(0x94D0_49BB_1331_11EB);
            z ^ (z >> 31)
        };
        Rng { state: [next(), next(), next(), next()] }
    }

    #[inline]
    pub fn next_u64(&mut self) -> u64 {
        let s = &mut self.state;
        let answer = s[1].wrapping_mul(5).rotate_left(7).wrapping_mul(9);
        let t = s[1] << 17;
        s[2] ^= s[0];
        s[3] ^= s[1];
        s[1] ^= s[2];
        s[0] ^= s[3];
        s[2] ^= t;
        s[3] = s[3].rotate_left(45);
        answer
    }

    /// A number below `n`, by multiply-shift rather than by remainder.
    #[inline]
    pub fn below(&mut self, n: u32) -> u32 {
        (((self.next_u64() >> 32) * n as u64) >> 32) as u32
    }

    #[inline]
    pub fn chance(&mut self, percent: u32) -> bool {
        self.below(100) < percent
    }

    #[inline]
    pub fn between(&mut self, low: i64, high: i64) -> i64 {
        low + self.below((high - low + 1) as u32) as i64
    }
}
