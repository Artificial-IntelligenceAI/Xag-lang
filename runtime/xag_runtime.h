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

XagStr xag_str_from(const char *bytes, uint64_t length);
XagStr xag_str_join(const XagStr *pieces, uint64_t count);
int64_t xag_str_count(const XagStr *text);
void xag_str_push(XagStr *text, const XagStr *tail); // grows in place when it can
void xag_str_drop(XagStr *text);
void xag_print(const XagStr *text);
void xag_print_i64(int64_t number);
void xag_print_bool(int truth);

// Where printing goes. One implementation for every engine, so that what a
// program says cannot depend on which of them said it.
void xag_set_output(void *file);

// Arithmetic that has an answer worth agreeing on. Division and remainder are
// truncated, a sum that does not fit wraps, and a whole number raised to a
// negative power has no answer at all.
int64_t xag_i64_add(int64_t a, int64_t b);
int64_t xag_i64_sub(int64_t a, int64_t b);
int64_t xag_i64_mul(int64_t a, int64_t b);
int64_t xag_i64_div(int64_t a, int64_t b);
int64_t xag_i64_mod(int64_t a, int64_t b);
int64_t xag_i64_pow(int64_t base, int64_t exponent);

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
