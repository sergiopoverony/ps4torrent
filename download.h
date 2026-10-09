#pragma once
#include <vector>
#include <stdint.h>
#include <stddef.h>
#include "torrent.h"
#include "storage.h"
#include "sha1.h"

// Недокачанный кусок: какие блоки уже получены (и записаны в итоговые файлы). Нужен, чтобы пауза,
// остановка и перезапуск не выбрасывали уже скачанную часть куска.
struct PartialPiece {
    uint32_t index = 0;
    uint32_t nblocks = 0;
    std::vector<unsigned char> got;     // got[b] == 1: блок b получен и лежит на диске
};

struct DownloadState {
    std::vector<bool> have;             // куски, проверенные по SHA-1
    uint32_t haveCount = 0;
    std::vector<uint32_t> recent;       // последние скачанные куски (для выборочной проверки после перезапуска)
    std::vector<PartialPiece> partials; // недокачанные куски (заполняется при сохранении / читается при старте)
    // Выбор файлов: куски, которые целиком принадлежат снятым с загрузки файлам, не нужны и не скачиваются.
    std::vector<bool> skip;             // skip[i] = кусок не нужен (пусто: нужны все)
    uint32_t needLeft = 0xFFFFFFFFu;    // сколько нужных кусков ещё не скачано (0xFFFFFFFF: ещё не посчитано)
};

// Пересчитывает, сколько нужных кусков осталось скачать (по have и skip).
inline void ds_recount(DownloadState& ds, uint32_t numPieces)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < numPieces; i++)
        if (!ds.have[i] && !(ds.skip.size() == numPieces && ds.skip[i])) n++;
    ds.needLeft = n;
}

// Проверка уже лежащих на диске данных (для докачки). Работает очень мелкими шагами: за один вызов
// step() читается и считается не больше порции в 128 КБ, поэтому экран и другие загрузки не ждут,
// даже если куски по 16 МБ.
class Rechecker {
public:
    // Полная проверка всех кусков.
    Rechecker(Storage& st, const Torrent& t, DownloadState& ds);

    // Выборочная проверка: проверяет только перечисленные куски (ds уже заполнен сохранёнными данными).
    // Если хотя бы один не сошёлся, failed() станет true.
    Rechecker(Storage& st, const Torrent& t, DownloadState& ds, const std::vector<uint32_t>& subset);

    // Делает очередную порцию (не меньше одной, пока не набрано maxBytes). true, когда всё проверено.
    bool step(size_t maxBytes);

    int percent() const;
    bool failed() const { return failed_; }
    bool is_subset() const { return subset_; }

private:
    Storage& st_;
    const Torrent& t_;
    DownloadState& ds_;
    bool subset_;
    bool failed_;
    std::vector<uint32_t> list_;        // кусочки для выборочной проверки
    size_t pos_;                        // номер в list_ (выборочно) или номер куска (полная)
    uint32_t off_;                      // сколько байт текущего куска уже обработано
    Sha1 sha_;
    bool bad_;                          // в текущем куске уже нашли проблему
    uint32_t lastPct_;
    std::vector<unsigned char> buf_;

    uint32_t current() const { return subset_ ? list_[pos_] : (uint32_t)pos_; }
    size_t total() const { return subset_ ? list_.size() : t_.numPieces; }
};

// Вызывается во время полной проверки; вернуть false, чтобы прервать.
typedef bool (*RecheckProgress)(void* user, uint32_t done, uint32_t total);

// Полная проверка целиком одним вызовом (для случаев, где ждать можно).
// Возвращает false, если проверку прервали.
bool recheck(Storage& st, const Torrent& t, DownloadState& ds, RecheckProgress cb, void* user);
