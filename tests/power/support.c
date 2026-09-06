// What the runtime asks of an operating system, when there is not one.
//
// Four functions, which is all `xag_deci.cpp` uses. Printing goes nowhere here:
// the guest reads answers out as coefficients and powers, not as text.

typedef unsigned long u64;

void *memcpy(void *to, const void *from, u64 count) {
  char *out = (char *)to;
  const char *in = (const char *)from;
  for (u64 i = 0; i < count; ++i)
    out[i] = in[i];
  return to;
}

void *memset(void *at, int value, u64 count) {
  char *out = (char *)at;
  for (u64 i = 0; i < count; ++i)
    out[i] = (char)value;
  return at;
}

unsigned long strlen(const char *text) {
  unsigned long n = 0;
  while (text[n])
    ++n;
  return n;
}

int strcmp(const char *a, const char *b) {
  while (*a && *a == *b) {
    ++a;
    ++b;
  }
  return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

// The runtime's own way of asking where output goes. There is nowhere.
void *xag_output_file(void) { return 0; }
void xag_stop(const char *why) {
  (void)why;
  for (;;) {
  }
}
