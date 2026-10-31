/* g10_shim.c — C shim за G10 тест: extern fn с bytes параметър и връщан тип. */
#include <stdint.h>
#include <stdlib.h>

typedef struct { unsigned char *data; int64_t len; } baga_bytes;

int64_t g10_sum(baga_bytes b) {
    int64_t s = 0;
    for (int64_t i = 0; i < b.len; i++) s += b.data[i];
    return s;
}

baga_bytes g10_reverse(baga_bytes b) {
    baga_bytes r;
    r.len = b.len;
    r.data = (unsigned char *)malloc(b.len ? (size_t)b.len : 1);
    for (int64_t i = 0; i < b.len; i++) r.data[i] = b.data[b.len - 1 - i];
    return r;
}

int64_t g10_len(baga_bytes b) { return b.len; }
