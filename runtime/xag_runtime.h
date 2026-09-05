// The one place a value is made, joined, counted, printed and freed.
//
// All three engines call these, so none of them can disagree about what joining
// means — only about control flow and the order things happen in, which is what
// the oracle is for. The cost of that is the other side of the same coin: a bug
// in here is a bug in all three at once, and no vote will find it. Hence the
// direct tests, and hence the allocation balance.

#ifndef XAG_RUNTIME_H
#define XAG_RUNTIME_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// A `str` owns its bytes outright. Ownership is settled before anything runs,
// so there is no count to keep: one owner, one free. `capacity` is what lets a
// `refmut` grow text where it stands.
typedef struct {
  char *bytes;
  uint64_t length;
  uint64_t capacity;
} XagStr;

// Anything that produces a `str` writes it through a pointer rather than
// returning it. A struct returned by value has a platform ABI to match, and
// matching one by hand in generated IR is a bug nobody would find quickly.
// A `many` owns every place it has. Its length is settled when it is made and
// never changes after, so unlike a `str` there is no capacity to keep: one
// owner, one free, and the places in between are always all there.
typedef struct {
  void *places;
  uint64_t length;
} XagMany;

// Which place an index names, or a stop. `wraps` is the `out-of-range` setting,
// handed in rather than looked up so that native code has it as a constant and
// the branch folds away where the index provably fits.
//
// An empty `many` stops under both settings: wrapping needs somewhere to land,
// and there is nowhere.
uint64_t xag_many_place(int64_t index, uint64_t length, int32_t wraps);

// The half of that which stops, on its own, so that native code can write the
// half that does not as a compare and a branch the optimiser can see through.
// An index inside the array never reaches here.
void xag_many_out_of_range(int64_t index, uint64_t length);

// `stride` is one place in bytes. The places arrive zeroed, because a `many`
// that has been made holds a value everywhere and a zero is the one thing every
// type can start as.
void xag_many_new(XagMany *out, uint64_t length, uint64_t stride);
void xag_many_drop(XagMany *m);     // the buffer only, for places that copy
void xag_many_drop_str(XagMany *m); // every `str` in it, and then the buffer

// The same value in every place. Only a value that copies gets here, so this is
// a copy of `stride` bytes and nothing more.
void xag_many_fill(XagMany *m, uint64_t stride, const void *one);

// An engine that keeps a `many` in its own memory rather than in the runtime's
// says so here, so that one balance covers all three of them and a leak in any
// one is a leak the tally reports.
void xag_note_taken(void);
void xag_note_given(void);

void xag_str_from(XagStr *out, const char *bytes, uint64_t length);
void xag_str_join(XagStr *out, const XagStr *pieces, uint64_t count);
int64_t xag_str_count(const XagStr *text);

// Less than, equal to, or greater than — as one implementation, so that no
// engine has its own idea of how text orders.
int64_t xag_str_compare(const XagStr *left, const XagStr *right);
void xag_str_push(XagStr *text, const XagStr *tail); // grows in place when it can
void xag_str_drop(XagStr *text);
void xag_print(const XagStr *text);
void xag_print_bool(int truth);

// Where printing goes. One implementation for every engine, so that what a
// program says cannot depend on which of them said it.
void xag_set_output(void *file);

// Where printing goes, as a `FILE *`. Shared so that every part of the runtime
// writes to the same place, whoever redirected it.
void *xag_output_file(void);

// Every whole number travels in one carrier, however wide it was written, with
// its width and its signedness alongside. Adding and multiplying are not here:
// under `overflow = "wrap"` a machine's own instructions are already exactly
// that, so making every engine call a function for them would buy nothing and
// cost the optimiser everything.
//
// What is here is what a choice was made about: dividing by zero stops, a
// remainder is truncated, and a whole number raised to a negative power has no
// answer at all.
typedef __int128 XagInt;

// A value cut down to what its type holds, wrapping as a machine would.
XagInt xag_int_fit(XagInt value, uint32_t width, int32_t is_signed);

XagInt xag_int_div(XagInt a, XagInt b, uint32_t width, int32_t is_signed);
XagInt xag_int_mod(XagInt a, XagInt b, uint32_t width, int32_t is_signed);
XagInt xag_int_pow(XagInt base, XagInt exponent, uint32_t width, int32_t is_signed);
void xag_print_int(XagInt value, uint32_t width, int32_t is_signed);

// IEEE 754 binary, carried at binary64 and cut back to the width that was
// written after every step — which is what makes a `bin32` sum a `bin32` sum
// rather than a `bin64` one that was stored narrowly.
//
// Nothing here stops. Dividing by zero is infinity, not an error: `infinity`
// and `not-a-number` are values of these types rather than accidents of them.
double xag_bin_fit(double value, uint32_t width);
double xag_bin_mod(double a, double b, uint32_t width);
double xag_bin_pow(double base, double exponent, uint32_t width);
void xag_print_bin(double value, uint32_t width);

// Whether a written value is one the width can hold without becoming infinite.
int32_t xag_bin_reads(const char *text, uint64_t length, uint32_t width, double *out);

// IEEE 754 binary128, written out in software because this machine's compiler
// has no type for it. The bits travel as a plain 128-bit integer: a struct
// would have a platform ABI to match, and this has none.
typedef unsigned __int128 XagBin128;

XagBin128 xag_bin128_add(XagBin128 a, XagBin128 b);
XagBin128 xag_bin128_sub(XagBin128 a, XagBin128 b);
XagBin128 xag_bin128_mul(XagBin128 a, XagBin128 b);
XagBin128 xag_bin128_div(XagBin128 a, XagBin128 b);
XagBin128 xag_bin128_mod(XagBin128 a, XagBin128 b);

// A power takes a whole-number exponent. Xag has no transcendental functions,
// so raising to a fraction has no answer to give and says so.
XagBin128 xag_bin128_pow(XagBin128 base, XagBin128 exponent);

// -1, 0 or 1, and -3 when the two cannot be ordered at all.
int32_t xag_bin128_compare(XagBin128 a, XagBin128 b);

XagBin128 xag_bin128_from_double(double value);
double xag_bin128_to_double(XagBin128 value);
int32_t xag_bin128_reads(const char *text, uint64_t length, XagBin128 *out);
void xag_print_bin128(XagBin128 value);

// IEEE 754 decimal32, decimal64 and decimal128. `width` says which of the three,
// and the bits travel as a 128-bit integer whichever it is.
//
// A decimal keeps the cohort it arrived at: `1.10` and `1.1` are equal and are
// not the same, and telling them apart is the point of the type.
//
// There are two implementations behind these names and the choice is made once,
// when this library is built, so that calling one costs nothing over the other:
//
//   software  IEEE 754 decimal written out here, in the BID encoding — the
//             coefficient stored as an ordinary binary integer, because the wide
//             arithmetic underneath already speaks that language. Runs anywhere.
//
//   hardware  IBM's decimal floating-point unit, which z/Architecture has from
//             z9 and POWER from POWER6. Both encode in DPD rather than BID, so
//             the bits under `XagDeci` are not the same ones — but no program
//             can see them, and IEEE 754 decimal is specified closely enough
//             that both must answer every operation identically. Where they do
//             not, one of them is wrong, which is what makes this worth a knob
//             rather than a preference.
//
// What the unit does is add, subtract, multiply, divide and compare, at
// decimal64 and decimal128. A `deci32` is worked out at decimal64 and rounded
// back either way, and `xag_deci_mod` and `xag_deci_pow` are built out of those
// five either way — no machine has an instruction for either.
typedef unsigned __int128 XagDeci;

// Whether this library was built against a decimal unit. There is no guessing
// at it from outside: a program asking for hardware decimal is refused unless
// this says yes.
int xag_decimal_is_hardware(void);

XagDeci xag_deci_add(uint32_t width, XagDeci a, XagDeci b);
XagDeci xag_deci_sub(uint32_t width, XagDeci a, XagDeci b);
XagDeci xag_deci_mul(uint32_t width, XagDeci a, XagDeci b);
XagDeci xag_deci_div(uint32_t width, XagDeci a, XagDeci b);
XagDeci xag_deci_mod(uint32_t width, XagDeci a, XagDeci b);
XagDeci xag_deci_pow(uint32_t width, XagDeci base, XagDeci exponent);
XagDeci xag_deci_negate(uint32_t width, XagDeci value);
int32_t xag_deci_compare(uint32_t width, XagDeci a, XagDeci b);
int32_t xag_deci_reads(uint32_t width, const char *text, uint64_t length, XagDeci *out);
void xag_print_deci(uint32_t width, XagDeci value);

// What is still held. A program that ends with anything outstanding has a drop
// that did not happen, and one that goes negative has a drop that happened
// twice — neither of which three engines agreeing would ever notice.
int64_t xag_live_allocations(void);
int xag_balance_is_clear(void);

// A stop, in the same place and for the same reason in every engine.
void xag_stop(const char *why);

#ifdef __cplusplus
}
#endif

#endif
