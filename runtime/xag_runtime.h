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

// -1, 0 or 1, and -3 when the two cannot be ordered at all.
int32_t xag_bin128_compare(XagBin128 a, XagBin128 b);

XagBin128 xag_bin128_from_double(double value);
double xag_bin128_to_double(XagBin128 value);
int32_t xag_bin128_reads(const char *text, uint64_t length, XagBin128 *out);
void xag_print_bin128(XagBin128 value);

// IEEE 754 decimal32, decimal64 and decimal128, in the BID encoding — the
// coefficient stored as an ordinary binary integer. `width` says which of the
// three, and the bits travel as a 128-bit integer whichever it is.
//
// A decimal keeps the cohort it arrived at: `1.10` and `1.1` are equal and are
// not the same, and telling them apart is the point of the type.
typedef unsigned __int128 XagDeci;

XagDeci xag_deci_add(uint32_t width, XagDeci a, XagDeci b);
XagDeci xag_deci_sub(uint32_t width, XagDeci a, XagDeci b);
XagDeci xag_deci_mul(uint32_t width, XagDeci a, XagDeci b);
XagDeci xag_deci_div(uint32_t width, XagDeci a, XagDeci b);
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
