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
