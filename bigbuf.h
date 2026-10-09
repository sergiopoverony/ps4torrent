#pragma once
// Буфер куска раздачи, выделенный напрямую у системы (mmap): на консоли обычный malloc/new суммарно даёт
// процессу всего ~10 МБ, а куски раздач бывают по 8-16 МБ.
#include <stddef.h>
#include <sys/mman.h>
#include <atomic>

// Сколько байт сейчас занимают все буферы кусков (всех раздач и очереди записи). По нему раздачи ограничивают друг друга по памяти.
inline std::atomic<size_t>& bigbuf_total() { static std::atomic<size_t> t(0); return t; }

// mmap выдаёт память, заполненную нулями, так что отдельно обнулять не нужно.
class BigBuf {
public:
    BigBuf() : p_(NULL), len_(0), map_(0) {}
    BigBuf(BigBuf&& o) noexcept : p_(o.p_), len_(o.len_), map_(o.map_) { o.p_ = NULL; o.len_ = 0; o.map_ = 0; }
    BigBuf& operator=(BigBuf&& o) noexcept {
        if (this != &o) { release(); p_ = o.p_; len_ = o.len_; map_ = o.map_; o.p_ = NULL; o.len_ = 0; o.map_ = 0; }
        return *this;
    }
    BigBuf(const BigBuf&) = delete;
    BigBuf& operator=(const BigBuf&) = delete;
    ~BigBuf() { release(); }

    bool alloc(size_t size) {
        release();
        if (size == 0) return false;
        size_t page = 16384;                                   // страница PS4; кратность 16 КБ подходит и для 4 КБ
        size_t m = (size + page - 1) / page * page;
        void* q = mmap(NULL, m, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
        if (q == MAP_FAILED) return false;
        p_ = (unsigned char*)q;
        len_ = size;
        map_ = m;
        bigbuf_total() += m;
        return true;
    }
    void release() {
        if (p_) { munmap(p_, map_); bigbuf_total() -= map_; p_ = NULL; len_ = 0; map_ = 0; }
    }
    unsigned char* data() { return p_; }
    size_t size() const { return len_; }

private:
    unsigned char* p_;
    size_t len_;
    size_t map_;
};

