// Where the runtime's printing goes when there is no operating system: the same
// console hypercall everything else here uses.
#include <cstdio>

extern "C" void xag_guest_write(const char *text, unsigned long count);

namespace std {
int fputs(const char *text, FILE *where) {
  (void)where;
  unsigned long count = 0;
  while (text[count])
    ++count;
  xag_guest_write(text, count);
  return 0;
}
} // namespace std
