/* The same billion iterations, to compare against. */
#include <stdio.h>
#include <stdint.h>

int main(void) {
    int64_t total = 0;
    for (int64_t i = 1; i <= 1000000000; i++) {
        total += i % 7;
    }
    printf("total %lld\n", (long long)total);
    return 0;
}
