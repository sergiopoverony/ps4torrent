#include "upload.h"
#include <time.h>
#include <atomic>

const int UPLOAD_STEPS[7] = { 24, 32, 64, 128, 256, 512, 1024 };

static std::atomic<bool> g_on(false);
static std::atomic<int>  g_kb(UPLOAD_DEFAULT_KB);

static double   g_tokens = 0;          // байт, которые можно отправить
static uint64_t g_lastMs = 0;
static uint64_t g_total = 0;
static uint64_t g_speedBase = 0;
static uint64_t g_speedMs = 0;
static double   g_smooth = 0;

static uint64_t mono_ms()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

int upload_snap_kb(int kb)
{
    int best = UPLOAD_STEPS[1];
    int bestD = 1 << 30;
    for (int i = 0; i < 7; i++) {
        int d = kb > UPLOAD_STEPS[i] ? kb - UPLOAD_STEPS[i] : UPLOAD_STEPS[i] - kb;
        if (d < bestD) { bestD = d; best = UPLOAD_STEPS[i]; }
    }
    return best;
}

void upload_set(bool enabled, int limitKB)
{
    g_on = enabled;
    g_kb = upload_snap_kb(limitKB);
}

bool upload_enabled() { return g_on.load(); }
int  upload_limit_kb() { return g_kb.load(); }

// Запас ("корзина") не больше половины секунды, но не меньше двух блоков, иначе при малом пределе блок не поместился бы никогда.
static void refill()
{
    uint64_t now = mono_ms();
    if (g_lastMs == 0) g_lastMs = now;
    uint64_t dt = now - g_lastMs;
    g_lastMs = now;
    double rate = (double)g_kb.load() * 1024.0;                // байт в секунду
    g_tokens += rate * (double)dt / 1000.0;
    double cap = rate / 2;
    if (cap < 2 * 16384.0 + 64) cap = 2 * 16384.0 + 64;
    if (g_tokens > cap) g_tokens = cap;
}

size_t upload_available()
{
    refill();
    return g_tokens > 0 ? (size_t)g_tokens : 0;
}

void upload_consume(size_t n)
{
    g_tokens -= (double)n;
    g_total += n;
}

uint64_t upload_total_bytes() { return g_total; }
int upload_speed_kb() { return (int)(g_smooth + 0.5); }

void upload_tick()
{
    uint64_t now = mono_ms();
    if (g_speedMs == 0) { g_speedMs = now; g_speedBase = g_total; return; }
    uint64_t dt = now - g_speedMs;
    if (dt < 1000) return;
    double inst = (double)(g_total - g_speedBase) / 1024.0 * 1000.0 / (double)dt;
    // та же сглаживающая формула, что у скорости загрузки: 15% нового, 85% прежнего за секунду
    unsigned steps = (unsigned)(dt / 1000);
    for (unsigned k = 0; k < steps && k < 30; k++) g_smooth = g_smooth * 0.85 + inst * 0.15;
    g_speedBase = g_total;
    g_speedMs = now;
}
