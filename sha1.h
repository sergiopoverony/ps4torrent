#pragma once
#include <stddef.h>
#include <stdint.h>

// SHA-1 целиком одним вызовом.
void sha1(const unsigned char* data, size_t len, unsigned char out[20]);

// SHA-1 по частям: нужен, чтобы проверять большой кусок маленькими порциями и не подвешивать экран.
struct Sha1 {
    uint32_t h[5];
    uint64_t total;
    unsigned char buf[64];
    size_t buflen;

    Sha1() { reset(); }
    void reset();
    void update(const unsigned char* data, size_t len);
    void final(unsigned char out[20]);
};
