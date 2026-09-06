// The decimal in this repository, built for a machine that has a decimal unit,
// answering the same questions it answers everywhere else.
//
// There is no operating system here. The runtime is compiled with
// `XAG_DECIMAL_HARDWARE`, so its arithmetic is the unit's instructions and its
// numbers are laid out the way the unit lays them out — and everything else
// about it is the same code that runs anywhere. Cases go out as text, which is
// the one thing both encodings agree on, and something on the other side works
// the same cases out in software and compares.

#include "xag_runtime.h"

typedef unsigned long u64;
typedef long i64;

static void hcall(u64 op, u64 a, u64 b, u64 c, u64 d) {
  register u64 r3 __asm__("r3") = op;
  register u64 r4 __asm__("r4") = a;
  register u64 r5 __asm__("r5") = b;
  register u64 r6 __asm__("r6") = c;
  register u64 r7 __asm__("r7") = d;
  __asm__ volatile("sc 1"
                   : "+r"(r3), "+r"(r4), "+r"(r5), "+r"(r6), "+r"(r7)
                   :
                   : "r0", "r8", "r9", "r10", "r11", "r12", "lr", "ctr",
                     "cr0", "cr1", "cr5", "cr6", "cr7", "memory");
}

// The console, which is the only way anything leaves here. The runtime's
// printing goes through this too — see `support_cxx.cpp`.
void xag_guest_write(const char *text, u64 count) {
  for (u64 i = 0; i < count; ++i)
    hcall(0x58, 0, 1, ((u64)(unsigned char)text[i]) << 56, 0);
}

static void say(const char *text) {
  u64 count = 0;
  while (text[count])
    ++count;
  xag_guest_write(text, count);
}

static u64 lengthOf(const char *text) {
  u64 count = 0;
  while (text[count])
    ++count;
  return count;
}

static char *putNumber(char *at, i64 value) {
  char digits[24];
  int n = 0;
  const int negative = value < 0;
  u64 left = negative ? (u64)(-value) : (u64)value;
  if (!left)
    digits[n++] = '0';
  while (left) {
    digits[n++] = (char)('0' + (left % 10));
    left /= 10;
  }
  if (negative)
    *at++ = '-';
  while (n)
    *at++ = digits[--n];
  return at;
}

// `1234567e-9`, which both encodings read the same way.
static void spellCase(char *out, i64 coefficient, int power) {
  char *at = putNumber(out, coefficient);
  *at++ = 'e';
  at = putNumber(at, power);
  *at = 0;
}

void run(void) {
  say("\r\ncases\r\n");

  u64 state = 0x2545F4914F6CDD1DUL;
  for (int i = 0; i < 30000; ++i) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    const u64 draw = state;

    // The three widths in turn, each with coefficients it can hold.
    const unsigned width = (i % 3) == 0 ? 32u : (i % 3) == 1 ? 64u : 128u;
    const u64 ceiling = width == 32    ? 10000000UL
                        : width == 64  ? 10000000000000000UL
                                       : 1000000000000000000UL;
    const int span = width == 32 ? 20 : width == 64 ? 60 : 200;

    i64 leftCoefficient = (i64)(draw % ceiling);
    i64 rightCoefficient = (i64)((draw >> 19) % ceiling);
    int leftPower = (int)((draw >> 41) % (u64)(2 * span + 1)) - span;
    int rightPower = (int)((draw >> 48) % (u64)(2 * span + 1)) - span;
    if (draw & 1)
      leftCoefficient = -leftCoefficient;
    if (draw & 2)
      rightCoefficient = -rightCoefficient;

    char leftText[48], rightText[48];
    spellCase(leftText, leftCoefficient, leftPower);
    spellCase(rightText, rightCoefficient, rightPower);

    XagDeci a = 0, b = 0;
    if (!xag_deci_reads(width, leftText, lengthOf(leftText), &a))
      continue;
    if (!xag_deci_reads(width, rightText, lengthOf(rightText), &b))
      continue;

    const int which = i & 3;
    const XagDeci answer = which == 0   ? xag_deci_add(width, a, b)
                           : which == 1 ? xag_deci_sub(width, a, b)
                           : which == 2 ? xag_deci_mul(width, a, b)
                                        : xag_deci_div(width, a, b);

    char head[24];
    char *at = putNumber(head, (i64)width);
    *at = 0;
    say(head);
    say(" ");
    say(leftText);
    say(which == 0 ? " + " : which == 1 ? " - " : which == 2 ? " x " : " / ");
    say(rightText);
    say(" = ");
    xag_print_deci(width, answer); // the runtime's own printing, to the console
    say("\r\n");
  }
  say("end\r\n");
}
