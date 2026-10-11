/* The C side of tinygen_extern.goose. Pair is laid out as Goose lays out
   a fixed struct: packed, no padding. */
#include <stdint.h>

#pragma pack(push, 1)
typedef struct { int32_t a; double b; } Pair;
typedef struct { const uint8_t *data; int64_t len; } Bytes;
#pragma pack(pop)

uint32_t tg_checksum(Bytes s) {
    uint32_t h = 2166136261u;
    for (int64_t i = 0; i < s.len; i++) h = (h ^ s.data[i]) * 16777619u;
    return h;
}

Pair tg_pair_scale(Pair p, int32_t k) {
    Pair r = { p.a * k, p.b * k };
    return r;
}

void tg_pair_fill(Pair *out, int64_t n) {
    out->a = (int32_t)(n * 2);
    out->b = (double)n / 8;
}
