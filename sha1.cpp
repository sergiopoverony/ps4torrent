#include "sha1.h"
#include <string.h>

static inline uint32_t rol(uint32_t v, int b) { return (v << b) | (v >> (32 - b)); }

static void block(uint32_t h[5], const unsigned char* p) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i*4] << 24) | ((uint32_t)p[i*4+1] << 16) |
               ((uint32_t)p[i*4+2] << 8) | (uint32_t)p[i*4+3];
    for (int i = 16; i < 80; i++)
        w[i] = rol(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);

    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20)      { f = (b & c) | (~b & d);          k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d;                   k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
        else             { f = b ^ c ^ d;                   k = 0xCA62C1D6; }
        uint32_t t = rol(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol(b, 30); b = a; a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}

void Sha1::reset() {
    h[0] = 0x67452301; h[1] = 0xEFCDAB89; h[2] = 0x98BADCFE; h[3] = 0x10325476; h[4] = 0xC3D2E1F0;
    total = 0;
    buflen = 0;
}

void Sha1::update(const unsigned char* data, size_t len) {
    total += len;
    if (buflen > 0) {
        size_t take = 64 - buflen;
        if (take > len) take = len;
        memcpy(buf + buflen, data, take);
        buflen += take;
        data += take;
        len -= take;
        if (buflen == 64) { block(h, buf); buflen = 0; }
    }
    while (len >= 64) { block(h, data); data += 64; len -= 64; }
    if (len > 0) { memcpy(buf, data, len); buflen = len; }
}

void Sha1::final(unsigned char out[20]) {
    unsigned char tail[128];
    size_t rem = buflen;
    memcpy(tail, buf, rem);
    tail[rem] = 0x80;
    size_t tl = (rem < 56) ? 64 : 128;
    memset(tail + rem + 1, 0, tl - rem - 1);
    uint64_t bits = total * 8;
    for (int i = 0; i < 8; i++) tail[tl - 1 - i] = (unsigned char)(bits >> (8 * i));
    block(h, tail);
    if (tl == 128) block(h, tail + 64);

    for (int i = 0; i < 5; i++) {
        out[i*4]   = (unsigned char)(h[i] >> 24);
        out[i*4+1] = (unsigned char)(h[i] >> 16);
        out[i*4+2] = (unsigned char)(h[i] >> 8);
        out[i*4+3] = (unsigned char)(h[i]);
    }
}

void sha1(const unsigned char* data, size_t len, unsigned char out[20]) {
    Sha1 s;
    s.update(data, len);
    s.final(out);
}
