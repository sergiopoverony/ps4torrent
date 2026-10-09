#include "download.h"
#include "log.h"
#include <string.h>
#include <algorithm>

static const uint32_t CHUNK = 128 * 1024;

Rechecker::Rechecker(Storage& st, const Torrent& t, DownloadState& ds)
    : st_(st), t_(t), ds_(ds), subset_(false), failed_(false), pos_(0), off_(0), bad_(false), lastPct_(0)
{
    ds_.have.assign(t_.numPieces, false);
    ds_.haveCount = 0;
    ds_.recent.clear();
}

Rechecker::Rechecker(Storage& st, const Torrent& t, DownloadState& ds, const std::vector<uint32_t>& subset)
    : st_(st), t_(t), ds_(ds), subset_(true), failed_(false), list_(subset), pos_(0), off_(0), bad_(false), lastPct_(0)
{
}

bool Rechecker::step(size_t maxBytes)
{
    size_t spent = 0;
    while (pos_ < total() && spent < maxBytes) {
        uint32_t idx = current();
        uint32_t psz = st_.piece_size(idx);

        if (off_ == 0) { sha_.reset(); bad_ = false; }

        uint32_t want = std::min<uint32_t>(CHUNK, psz - off_);
        if (!bad_) {
            buf_.resize(want);
            if (!st_.read_piece_part(idx, off_, buf_.data(), want)) {
                bad_ = true;                 // нет файла или данных: остальную часть куска не читаем
                spent += 4096;
            } else {
                sha_.update(buf_.data(), want);
                spent += want;
            }
        }
        off_ += want;
        if (bad_) off_ = psz;

        if (off_ >= psz) {
            bool good = false;
            if (!bad_) {
                unsigned char h[20];
                sha_.final(h);
                good = (memcmp(h, t_.pieces.data() + (size_t)idx * 20, 20) == 0);
            }

            if (subset_) {
                if (!good) { failed_ = true; pos_ = total(); return true; }
            } else if (good) {
                ds_.have[idx] = true;
                ds_.haveCount++;
            }

            pos_++;
            off_ = 0;

            if (!subset_) {
                uint32_t pct = (uint32_t)((uint64_t)pos_ * 100 / t_.numPieces);
                if (pct / 10 > lastPct_ / 10) logf_("  checking existing data: %u%%", pct);
                lastPct_ = pct;
            }
        }
    }
    return pos_ >= total();
}

int Rechecker::percent() const
{
    size_t n = total();
    if (n == 0) return 100;
    return (int)((uint64_t)pos_ * 100 / n);
}

bool recheck(Storage& st, const Torrent& t, DownloadState& ds, RecheckProgress cb, void* user)
{
    Rechecker rc(st, t, ds);
    for (;;) {
        if (cb && !cb(user, (uint32_t)((uint64_t)rc.percent() * t.numPieces / 100), t.numPieces)) return false;
        if (rc.step(1 << 20)) break;
    }
    if (cb) cb(user, t.numPieces, t.numPieces);
    return true;
}
