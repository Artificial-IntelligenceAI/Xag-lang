// Does this machine's decimal unit answer what our software decimal answers?
//
// Nothing is linked under this and nothing is loaded beside it. The firmware
// puts it at an address it chooses, so there is no static data here at all:
// every character is an immediate in the instruction stream, which is also
// exactly the shape PAPR's console hypercall wants its argument in.

typedef unsigned long u64;
typedef long i64;

// Four characters at a time, because a 32-bit constant is two instructions and
// a 64-bit one is a load from a constant pool — and a constant pool is data,
// which is the one thing this program cannot have.
#define P4(a, b, c, d)                                                                   \
  ((unsigned)(((unsigned)(a) << 24) | ((unsigned)(b) << 16) | ((unsigned)(c) << 8) |     \
              (unsigned)(d)))

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

static void say4(unsigned word, u64 count) {
  hcall(0x58, 0, count, ((u64)word) << 32, 0);
}
static void nl(void) { say4(P4('\r', '\n', 0, 0), 2); }

static void sayNumber(i64 value) {
  char digits[24];
  int at = 24;
  const int negative = value < 0;
  u64 left = negative ? (u64)(-value) : (u64)value;
  if (!left)
    digits[--at] = '0';
  while (left) {
    digits[--at] = (char)('0' + (left % 10));
    left /= 10;
  }
  if (negative)
    digits[--at] = '-';
  while (at < 24) {
    unsigned packed = 0;
    u64 count = 0;
    while (at < 24 && count < 4) {
      packed |= ((unsigned)(unsigned char)digits[at]) << (24 - 8 * count);
      ++at;
      ++count;
    }
    say4(packed, count);
  }
}

// A whole number into the decimal unit, and back out again.
static double decimalOf(i64 value) {
  // Opaque, so the number is not folded into a constant pool on the way to the
  // register — a pool is data, and this program has nowhere to put data.
  __asm__("" : "+r"(value));
  double held;
  __builtin_memcpy(&held, &value, 8);
  double out;
  __asm__("dcffix %0, %1" : "=d"(out) : "d"(held));
  return out;
}
static i64 wholeOf(double decimal) {
  double out;
  __asm__("dctfix %0, %1" : "=d"(out) : "d"(decimal));
  i64 value;
  __builtin_memcpy(&value, &out, 8);
  return value;
}
// A decimal64 built from a whole-number coefficient and a power of ten, and
// taken apart again the same way. `dcffix` puts an integer into the unit,
// `diex` writes the exponent it stands at, and `dxex` reads that back — so a
// value can cross between here and a program that holds it some other way
// without either of them having to know the other's encoding.
#define BIAS64 398

static double atPower(i64 coefficient, int power) {
  double value = decimalOf(coefficient);
  i64 biased = power + BIAS64;
  double held;
  __asm__("" : "+r"(biased));
  __builtin_memcpy(&held, &biased, 8);
  double out;
  __asm__("diex %0, %1, %2" : "=d"(out) : "d"(held), "d"(value));
  return out;
}

static i64 coefficientOf(double value) {
  i64 zero = BIAS64;
  double held;
  __asm__("" : "+r"(zero));
  __builtin_memcpy(&held, &zero, 8);
  double flat;
  __asm__("diex %0, %1, %2" : "=d"(flat) : "d"(held), "d"(value));
  return wholeOf(flat);
}

static i64 powerOf(double value) {
  double out;
  __asm__("dxex %0, %1" : "=d"(out) : "d"(value));
  i64 biased;
  __builtin_memcpy(&biased, &out, 8);
  return biased - BIAS64;
}

static double add(double a, double b) {
  double o; __asm__("dadd %0, %1, %2" : "=d"(o) : "d"(a), "d"(b)); return o;
}
static double sub(double a, double b) {
  double o; __asm__("dsub %0, %1, %2" : "=d"(o) : "d"(a), "d"(b)); return o;
}
static double mul(double a, double b) {
  double o; __asm__("dmul %0, %1, %2" : "=d"(o) : "d"(a), "d"(b)); return o;
}
static double divide(double a, double b) {
  double o; __asm__("ddiv %0, %1, %2" : "=d"(o) : "d"(a), "d"(b)); return o;
}

// Counted in a local and handed back, because a static would live in memory the
// firmware puts wherever it likes and this program addresses none.
static int check(unsigned tagA, unsigned tagB, i64 got, i64 wanted) {
  say4(tagA, 4);
  say4(tagB, 4);
  sayNumber(got);
  if (got != wanted) {
    say4(P4(' ', 'B', 'A', 'D'), 4);
    say4(P4(':', ' ', ' ', ' '), 2);
    sayNumber(wanted);
    nl();
    return 1;
  }
  nl();
  return 0;
}

void run(void) {
  nl();
  say4(P4('d', 'f', 'p', ' '), 4);
  say4(P4('h', 'e', 'r', 'e'), 4);
  nl();

  int wrong = 0;

  const double five = decimalOf(5), seven = decimalOf(7);
  wrong += check(P4('5', '+', '7', ' '), P4(' ', ' ', '=', ' '), wholeOf(add(five, seven)), 12);
  wrong += check(P4('5', '-', '7', ' '), P4(' ', ' ', '=', ' '), wholeOf(sub(five, seven)), -2);
  wrong += check(P4('5', 'x', '7', ' '), P4(' ', ' ', '=', ' '), wholeOf(mul(five, seven)), 35);
  wrong += check(P4('1', '0', '0', '/'), P4('4', ' ', '=', ' '),
        wholeOf(divide(decimalOf(100), decimalOf(4))), 25);

  // The one that says this is decimal and not binary: a tenth three times over
  // is exactly three tenths, so ten of that is exactly three.
  const double tenth = divide(decimalOf(1), decimalOf(10));
  const double three = add(add(tenth, tenth), tenth);
  wrong += check(P4('.', '1', 'x', '3'), P4('x', '1', '0', '='),
        wholeOf(mul(three, decimalOf(10))), 3);

  wrong += check(P4('r', 'o', 'u', 'n'), P4('d', 't', 'r', 'p'),
        wholeOf(decimalOf(1234567890123456L)), 1234567890123456L);

  say4(wrong ? P4('S', 'O', 'M', 'E') : P4('a', 'l', 'l', ' '), 4);
  say4(wrong ? P4(' ', 'B', 'A', 'D') : P4('g', 'o', 'o', 'd'), 4);
  nl();

  // Now the part nothing here can check: every case is written out as a
  // coefficient and a power of ten, which is what a decimal is whatever
  // encoding holds it, and something on the other side compares the unit's
  // answer against the one worked out in software.
  say4(P4('c', 'a', 's', 'e'), 4);
  say4(P4('s', ' ', ' ', ' '), 2);
  nl();

  u64 state = 0x2545F4914F6CDD1DUL;
  for (int i = 0; i < 20000; ++i) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    const u64 draw = state;

    // Coefficients up to sixteen digits, which is what a decimal64 holds, and
    // powers close enough together that both sides have something to say.
    i64 leftCoefficient = (i64)(draw % 10000000000000000UL);
    i64 rightCoefficient = (i64)((draw >> 17) % 10000000000000000UL);
    int leftPower = (int)((draw >> 40) % 41) - 20;
    int rightPower = (int)((draw >> 47) % 41) - 20;
    if (draw & 1)
      leftCoefficient = -leftCoefficient;
    if (draw & 2)
      rightCoefficient = -rightCoefficient;

    const double a = atPower(leftCoefficient, leftPower);
    const double b = atPower(rightCoefficient, rightPower);
    double r;
    const int which = i & 3;
    if (which == 0)
      r = add(a, b);
    else if (which == 1)
      r = sub(a, b);
    else if (which == 2)
      r = mul(a, b);
    else
      r = divide(a, b);

    sayNumber(leftCoefficient);   say4(P4('e', 0, 0, 0), 1);
    sayNumber(leftPower);         say4(P4(' ', 0, 0, 0), 1);
    say4(which == 0 ? P4('+', 0, 0, 0)
         : which == 1 ? P4('-', 0, 0, 0)
         : which == 2 ? P4('x', 0, 0, 0) : P4('/', 0, 0, 0), 1);
    say4(P4(' ', 0, 0, 0), 1);
    sayNumber(rightCoefficient);  say4(P4('e', 0, 0, 0), 1);
    sayNumber(rightPower);        say4(P4(' ', '=', ' ', 0), 3);
    sayNumber(coefficientOf(r));  say4(P4('e', 0, 0, 0), 1);
    sayNumber(powerOf(r));
    nl();
  }
  say4(P4('e', 'n', 'd', 0), 3);
  nl();
}
