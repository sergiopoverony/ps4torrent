#include "swarm.h"
#include "net.h"
#include "tracker.h"
#include "sha1.h"
#include "log.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/select.h>
#include <sys/mman.h>
#include "bigbuf.h"
#include "writer.h"
#include <pthread.h>
#include <atomic>
#include <memory>
#include <algorithm>
#include <map>
#include <string>
#include <vector>

#ifdef MSG_NOSIGNAL
#define SEND_FLAGS MSG_NOSIGNAL
#else
#define SEND_FLAGS 0
#endif

static const uint32_t BLOCK = 16384;
#define IS_AGAIN(e) ((e) == EAGAIN || (e) == EWOULDBLOCK || (e) == 35)

static const int DEFAULT_MAX_CONNS = 30;
static const int MAX_INFLIGHT = 24;                      // запросов в полёте на одного пира
// Сколько байт кусков одна раздача держит в памяти одновременно. Раньше было 8 МБ (наследие тех времён, когда буферы лежали в куче на
// ~10 МБ): у раздач с кусками по 4-8 МБ это всего 2 куска, и пиры, у которых нет именно этих кусков, простаивали. Теперь буферы в mmap.
#ifndef SWARM_ACTIVE_BYTES
#define SWARM_ACTIVE_BYTES (32u << 20)
#endif
// Защита по памяти: все буферы кусков всех раздач вместе (с очередью записи на диск) не должны превышать этот предел.
// Новые куски сверх предела не открываются (кроме двух на раздачу, чтобы она не остановилась совсем).
#ifndef MAX_BIGBUF_TOTAL
#define MAX_BIGBUF_TOTAL (128u << 20)
#endif
static const size_t MAX_ACTIVE_BYTES = SWARM_ACTIVE_BYTES;  // сколько кусков держим в памяти
static const size_t SPARE_ACTIVE = 2;                    // запасные слоты для пиров, которые не могут помочь с текущими кусками
static const int PIECE_STALE_SECONDS = 20;               // по куску давно ничего не приходит: перезапрашиваем недостающие блоки
static const int PIECE_DROP_SECONDS = 600;               // и только если не приходит 10 минут, выбрасываем кусок целиком
static const int CONNECT_TIMEOUT_SECONDS = 10;           // сколько ждём ответа на подключение (было 15)
static const int HANDSHAKE_TIMEOUT_SECONDS = 10;         // и на handshake (было 15)
static const int PEER_SILENT_SECONDS = 15;               // пир держит наши запросы, но блоков не шлёт: отпускаем (было 30 с с запроса)
static const int STALL_RESTART_SECONDS = 30;             // совсем нет данных: пересоздаём соединения
static const int ANNOUNCE_NO_PEERS_SECONDS = 45;         // как часто спрашивать трекер, когда никто не отдаёт
static const int ANNOUNCE_NORMAL_SECONDS = 120;

static uint32_t get32(const unsigned char* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static void put32(unsigned char* p, uint32_t v) {
    p[0] = (unsigned char)(v >> 24); p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);  p[3] = (unsigned char)v;
}

struct PieceBuf {
    bool active = false;
    uint32_t psize = 0, nblocks = 0, gotCount = 0;
    BigBuf buf;
    std::vector<unsigned char> state;   // 0 не запрошен, 1 запрошен, 2 получен
    time_t lastActivity = 0;            // когда по этому куску в последний раз что-то пришло
    std::vector<unsigned char> persisted;   // 1: блок уже записан в итоговый файл (сохранение недокачанных кусков)
    bool fromDisk = false;              // часть блоков восстановлена с диска (если сумма не сойдётся, виноваты не пиры)
    time_t lastData = 0;                // когда пришёл последний блок этого куска
    time_t softAt = 0;                  // когда в последний раз перезапрашивали недостающие блоки
    // Проверенный кусок записывается на диск в отдельном потоке (writer.cpp): запись на флешку занимает секунды,
    // и главный цикл не должен её ждать. Данные куска при этом передаются задаче (job).
    bool writing = false;               // кусок проверен и ждёт/идёт запись на диск
    WriteJobPtr job;
};

// Записи о кусках создаются только для кусков в работе. Раньше был массив на ВСЕ куски раздачи (144 байта на кусок: у God of War
// 25 953 кусков = 3,5 МБ из ~10 МБ кучи, а раздач бывает три), и память кончалась (std::bad_alloc). Теперь на кусок приходится
// один указатель, а запись живёт, пока кусок скачивается или пишется на диск.
class PieceTable {
public:
    PieceTable() {}
    PieceTable(const PieceTable&) = delete;
    PieceTable& operator=(const PieceTable&) = delete;
    ~PieceTable() { for (size_t i = 0; i < v_.size(); i++) delete v_[i]; }
    void resize(size_t n) { for (size_t i = 0; i < v_.size(); i++) delete v_[i]; v_.assign(n, (PieceBuf*)NULL); }
    PieceBuf& operator[](uint32_t i) { if (!v_[i]) v_[i] = new PieceBuf(); return *v_[i]; }          // создаёт запись при первом обращении
    bool active(uint32_t i) const { return v_[i] && v_[i]->active; }                                   // не создаёт запись
    void release(uint32_t i) { if (v_[i] && !v_[i]->active && !v_[i]->writing) { delete v_[i]; v_[i] = NULL; } }   // кусок больше не нужен: память в кучу
private:
    std::vector<PieceBuf*> v_;
};

struct Req {
    uint32_t piece;
    uint32_t block;
    time_t sent;
};

struct Conn {
    int fd = -1;
    int state = 0;                      // 0 подключается, 1 ждём handshake, 2 работает
    std::string key;
    time_t started = 0, lastRecv = 0, lastBlock = 0;
    std::string in, out;
    bool choked = true;
    std::vector<bool> has;
    std::vector<Req> pending;
    uint64_t bytes = 0;
    std::string failWhy;                // почему соединение пришлось закрыть (для журнала)
    std::string remoteId;               // peer id из handshake (20 байт), чтобы не держать дубликаты
    bool incoming = false;              // пир подключился к нам сам
};

struct Ctx {
    const Torrent& t;
    Storage& st;
    DownloadState& ds;
    PieceTable pieces;                  // записи о кусках в работе (по требованию)
    std::vector<uint32_t> activeList;
    std::vector<uint32_t> writing;      // проверенные куски, которые сейчас пишет поток записи
    int writeFailed = 0;                // сколько кусков не удалось записать (ошибка диска, флешка пропала)
    size_t maxActive = 8;
    std::map<std::string, time_t> failedAt;
    uint64_t totalBytes = 0;
    bool fatal = false;
    std::string tag;                    // короткая метка раздачи для строк лога
    unsigned char selfId[20];           // наш peer id (чтобы не соединиться с самим собой)
    unsigned incomingTotal = 0;         // сколько входящих соединений принято за сессию
    std::map<std::string, int> failStats;   // причины закрытых соединений за последнюю минуту
    time_t lastFailLog = 0;
    Ctx(const Torrent& t_, Storage& st_, DownloadState& ds_) : t(t_), st(st_), ds(ds_) { memset(selfId, 0, sizeof(selfId)); }
};

static std::string peer_key(const Peer& p) {
    char b[40];
    snprintf(b, sizeof(b), "%u.%u.%u.%u:%u", p.ip[0], p.ip[1], p.ip[2], p.ip[3], p.port);
    return b;
}

static void release_pending(Ctx& x, Conn& c) {
    for (size_t i = 0; i < c.pending.size(); i++) {
        const Req& r = c.pending[i];
        if (!x.pieces.active(r.piece)) continue;
        PieceBuf& pb = x.pieces[r.piece];
        if (r.block < pb.nblocks && pb.state[r.block] == 1) pb.state[r.block] = 0;
    }
    c.pending.clear();
}

static void conn_close(Ctx& x, Conn& c, const char* why) {
    if (c.fd < 0) return;
    release_pending(x, c);
    if (strcmp(why, "restart") != 0) {
        static const char* stage[] = {"connecting", "handshake", "active"};
        x.failStats[std::string(stage[c.state >= 0 && c.state < 3 ? c.state : 2]) + ": " + why]++;
    }
    if (c.state == 2)
        logf_("    [%s] peer %s closed (%s), got %llu KB", x.tag.c_str(), c.key.c_str(), why,
              (unsigned long long)(c.bytes / 1024));
    close(c.fd);
    c.fd = -1;
    x.failedAt[c.key] = time(NULL);
}

static bool flush_out(Conn& c) {
    while (!c.out.empty()) {
        int r = send(c.fd, c.out.data(), c.out.size(), SEND_FLAGS);
        if (r > 0) {
            c.out.erase(0, (size_t)r);
        } else if (r < 0 && IS_AGAIN(errno)) {
            return true;
        } else {
            char b[40];
            snprintf(b, sizeof(b), "send error errno %d", errno);
            c.failWhy = b;
            return false;
        }
    }
    return true;
}

static bool next_block(Ctx& x, Conn& c, uint32_t& pi, uint32_t& bi) {
    for (size_t k = 0; k < x.activeList.size(); k++) {
        uint32_t p = x.activeList[k];
        if (!c.has[p]) continue;
        PieceBuf& pb = x.pieces[p];
        for (uint32_t b = 0; b < pb.nblocks; b++) {
            if (pb.state[b] == 0) { pi = p; bi = b; return true; }
        }
    }
    // Новый кусок можно начать, если есть свободный слот. Если этот пир вообще не имеет ни одного из
    // текущих кусков (то есть помочь с ними не может), даём ему ещё несколько запасных слотов,
    // иначе при замолчавших пирах остальные простаивали бы.
    bool canServeActive = false;
    for (size_t k = 0; k < x.activeList.size(); k++)
        if (c.has[x.activeList[k]]) { canServeActive = true; break; }
    size_t limit = x.maxActive + (canServeActive ? 0 : SPARE_ACTIVE);

    const bool memOk = x.activeList.size() < 2 || bigbuf_total().load() + x.t.pieceLength <= (size_t)MAX_BIGBUF_TOTAL;
    if (x.activeList.size() < limit && memOk) {
        for (uint32_t i = 0; i < x.t.numPieces; i++) {
            if (x.ds.have[i] || x.pieces.active(i) || !c.has[i] || (!x.ds.skip.empty() && x.ds.skip[i])) continue;      // ненужные куски не качаем
            PieceBuf& pb = x.pieces[i];
            pb.lastActivity = time(NULL);
            pb.lastData = pb.lastActivity;
            pb.softAt = 0;
            pb.active = true;
            pb.psize = x.st.piece_size(i);
            pb.nblocks = (pb.psize + BLOCK - 1) / BLOCK;
            pb.gotCount = 0;
            if (!pb.buf.alloc(pb.psize)) {
                static time_t lastWarn = 0;
                if (time(NULL) - lastWarn >= 30) { lastWarn = time(NULL); logf_("  [%s] cannot allocate %u bytes for piece %u", x.tag.c_str(), (unsigned)pb.psize, (unsigned)i); }
                pb.active = false;
                return false;
            }
            pb.state.assign(pb.nblocks, 0);
            pb.persisted.assign(pb.nblocks, 0);
            x.activeList.push_back(i);
            pi = i;
            bi = 0;
            return true;
        }
    }
    return false;
}

static bool fill_requests(Ctx& x, Conn& c) {
    if (c.state != 2 || c.choked) return true;
    while ((int)c.pending.size() < MAX_INFLIGHT) {
        uint32_t pi, bi;
        if (!next_block(x, c, pi, bi)) break;
        PieceBuf& pb = x.pieces[pi];
        uint32_t begin = bi * BLOCK;
        uint32_t len = std::min<uint32_t>(BLOCK, pb.psize - begin);
        unsigned char req[17];
        put32(req, 13);
        req[4] = 6;
        put32(req + 5, pi);
        put32(req + 9, begin);
        put32(req + 13, len);
        c.out.append((const char*)req, 17);
        pb.state[bi] = 1;
        Req r;
        r.piece = pi;
        r.block = bi;
        r.sent = time(NULL);
        c.pending.push_back(r);
    }
    return flush_out(c);
}

static uint64_t mono_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

// Кусок записан и проверен: учитываем его и освобождаем память.
static void finalize_piece(Ctx& x, uint32_t idx) {
    PieceBuf& pb = x.pieces[idx];
    x.ds.have[idx] = true;
    x.ds.haveCount++;
    if (x.ds.needLeft > 0 && x.ds.needLeft != 0xFFFFFFFFu && (x.ds.skip.empty() || !x.ds.skip[idx])) x.ds.needLeft--;
    x.ds.recent.push_back(idx);                       // запоминаем для выборочной проверки после перезапуска
    if (x.ds.recent.size() > 16) x.ds.recent.erase(x.ds.recent.begin());
    pb.active = false;
    pb.writing = false;
    pb.job.reset();
    pb.buf.release();
    std::vector<unsigned char>().swap(pb.state);
    std::vector<unsigned char>().swap(pb.persisted);
    pb.fromDisk = false;
    for (size_t k = 0; k < x.activeList.size(); k++) {
        if (x.activeList[k] == idx) { x.activeList.erase(x.activeList.begin() + k); break; }
    }
    x.pieces.release(idx);                              // запись о куске больше не нужна: возвращаем память
}

// Смотрит, какие куски поток записи уже записал: завершает их. false: ошибка записи (x.fatal выставлен).
static bool poll_write_jobs(Ctx& x) {
    for (size_t i = 0; i < x.writing.size(); ) {          // и после первой ошибки: записанные куски всё равно засчитываем
        uint32_t idx = x.writing[i];
        PieceBuf& pb = x.pieces[idx];
        int st = pb.job ? pb.job->state.load() : 2;
        if (st == 2) {
            x.writing.erase(x.writing.begin() + i);
            finalize_piece(x, idx);
        } else if (st == 3) {
            // Этот кусок не записался: он будет скачан заново. Остальные, уже записанные, всё равно засчитываем
            // (раньше цикл прекращался на первой ошибке, и записанные позади куски терялись).
            logf_("    [%s] piece %u WRITE FAILED", x.tag.c_str(), idx);
            x.fatal = true;
            x.writeFailed++;
            x.writing.erase(x.writing.begin() + i);
            pb.active = false;
            pb.writing = false;
            pb.job.reset();
            std::vector<unsigned char>().swap(pb.state);
            std::vector<unsigned char>().swap(pb.persisted);
            for (size_t k = 0; k < x.activeList.size(); k++)
                if (x.activeList[k] == idx) { x.activeList.erase(x.activeList.begin() + k); break; }
            x.pieces.release(idx);
        } else {
            i++;
        }
    }
    return !x.fatal;
}

static bool finish_piece(Ctx& x, uint32_t idx) {
    PieceBuf& pb = x.pieces[idx];
    unsigned char h[20];
    sha1(pb.buf.data(), pb.buf.size(), h);
    if (memcmp(h, x.t.pieces.data() + (size_t)idx * 20, 20) != 0) {
        pb.gotCount = 0;
        std::fill(pb.state.begin(), pb.state.end(), (unsigned char)0);
        std::fill(pb.persisted.begin(), pb.persisted.end(), (unsigned char)0);   // на диске теперь устаревшие данные
        if (pb.fromDisk) {
            // Часть куска была прочитана с диска (сохранена раньше): значит, виноваты наши данные, а не пир.
            logf_("    [%s] piece %u failed verification after restoring it from disk, downloading it again", x.tag.c_str(), idx);
            pb.fromDisk = false;
            return true;
        }
        logf_("    [%s] piece %u HASH MISMATCH, dropping peer", x.tag.c_str(), idx);
        return false;
    }

    // Кусок верный. На диск идут только те блоки, которых там ещё нет (остальные записаны раньше при сохранении
    // недокачанных кусков, второй раз их писать незачем), причём подряд идущие блоки одной серией.
    std::vector<std::pair<uint32_t, uint32_t> > runsLocal;
    uint32_t b = 0;
    while (b < pb.nblocks) {
        if (b < pb.persisted.size() && pb.persisted[b]) { b++; continue; }
        uint32_t start = b;
        while (b < pb.nblocks && !(b < pb.persisted.size() && pb.persisted[b])) b++;
        uint32_t off = start * BLOCK;
        uint32_t end = std::min<uint32_t>(b * BLOCK, pb.psize);
        runsLocal.push_back(std::make_pair(off, end - off));
    }
    if (runsLocal.empty()) { finalize_piece(x, idx); return true; }       // всё уже на диске

    // Данные куска передаём потоку записи (задача становится их владельцем), главный цикл идёт дальше.
    WriteJobPtr job(new WriteJob());
    job->st = &x.st;
    job->index = idx;
    job->buf = std::move(pb.buf);
    job->runs = runsLocal;
    pb.job = job;
    pb.writing = true;
    x.writing.push_back(idx);
    writer_submit(job);
    return true;
}

static bool on_block(Ctx& x, Conn& c, uint32_t index, uint32_t begin,
                     const unsigned char* d, uint32_t dlen) {
    if (index >= x.t.numPieces || begin % BLOCK != 0) return true;
    uint32_t b = begin / BLOCK;

    for (size_t i = 0; i < c.pending.size(); i++) {
        if (c.pending[i].piece == index && c.pending[i].block == b) {
            c.pending.erase(c.pending.begin() + i);
            break;
        }
    }

    if (!x.pieces.active(index)) return true;
    PieceBuf& pb = x.pieces[index];
    if (b >= pb.nblocks) return true;
    uint32_t expect = std::min<uint32_t>(BLOCK, pb.psize - begin);
    if (dlen != expect || pb.state[b] == 2) return true;

    memcpy(pb.buf.data() + begin, d, dlen);
    pb.lastActivity = time(NULL);
    pb.lastData = pb.lastActivity;
    pb.state[b] = 2;
    pb.gotCount++;
    c.bytes += dlen;
    c.lastBlock = time(NULL);
    x.totalBytes += dlen;
    if (pb.gotCount == pb.nblocks) return finish_piece(x, index);
    return true;
}

static bool on_message(Ctx& x, Conn& c, const unsigned char* m, uint32_t len) {
    if (len == 0) return true;   // keep-alive
    unsigned char id = m[0];
    uint32_t np = x.t.numPieces;
    if (id == 0) {
        c.choked = true;
        release_pending(x, c);
    } else if (id == 1) {
        c.choked = false;
    } else if (id == 4 && len >= 5) {
        uint32_t i = get32(m + 1);
        if (i < np) c.has[i] = true;
    } else if (id == 5) {
        for (uint32_t i = 0; i < np; i++) {
            size_t byte = 1 + i / 8;
            if (byte < len && (m[byte] & (0x80 >> (i % 8)))) c.has[i] = true;
        }
    } else if (id == 7 && len >= 9) {
        return on_block(x, c, get32(m + 1), get32(m + 5), m + 9, len - 9);
    }
    return true;
}

static bool parse_input(Ctx& x, Conn& c) {
    size_t pos = 0;
    if (c.state == 1) {
        if (c.in.size() < 68) return true;
        const unsigned char* h = (const unsigned char*)c.in.data();
        if (h[0] != 19 || memcmp(h + 1, "BitTorrent protocol", 19) != 0 ||
            memcmp(h + 28, x.t.infoHash, 20) != 0) {
            c.failWhy = "handshake rejected (not BitTorrent, wrong torrent or encrypted)";
            return false;
        }
        if (memcmp(h + 48, x.selfId, 20) == 0) {
            c.failWhy = "connected to ourselves";
            return false;
        }
        c.remoteId.assign((const char*)h + 48, 20);
        pos = 68;
        c.state = 2;
        static const unsigned char interested[5] = {0, 0, 0, 1, 2};
        c.out.append((const char*)interested, 5);
    }
    while (c.state == 2) {
        if (c.in.size() - pos < 4) break;
        const unsigned char* p = (const unsigned char*)c.in.data() + pos;
        uint32_t len = get32(p);
        if (len > 2 * 1024 * 1024) { c.failWhy = "message too big"; return false; }
        if (c.in.size() - pos - 4 < len) break;
        if (!on_message(x, c, p + 4, len)) { c.failWhy = "protocol error"; return false; }
        pos += 4 + len;
    }
    if (pos) c.in.erase(0, pos);
    if (c.in.capacity() > 98304 && c.in.size() < 32768) { std::string small(c.in); c.in.swap(small); }      // быстрый пир раздул буфер: возвращаем память
    return true;
}

static bool do_read(Ctx& x, Conn& c) {
    static unsigned char rbuf[65536];
    for (int k = 0; k < 4; k++) {
        int r = recv(c.fd, rbuf, sizeof(rbuf), 0);
        if (r > 0) {
            c.in.append((const char*)rbuf, (size_t)r);
            c.lastRecv = time(NULL);
            if (r < (int)sizeof(rbuf)) break;
        } else if (r == 0) {
            c.failWhy = "closed by peer";
            return false;
        } else {
            if (IS_AGAIN(errno)) break;
            char b[40];
            snprintf(b, sizeof(b), "recv error errno %d", errno);
            c.failWhy = b;
            return false;
        }
    }
    return parse_input(x, c);
}

// Бросает кусок: освобождает память и снимает его запросы у всех соединений.
static void evict_piece(Ctx& x, std::vector<Conn>& conns, uint32_t idx) {
    if (!x.pieces.active(idx)) return;
    PieceBuf& pb = x.pieces[idx];
    for (size_t i = 0; i < conns.size(); i++) {
        std::vector<Req>& pend = conns[i].pending;
        for (size_t k = pend.size(); k > 0; k--)
            if (pend[k - 1].piece == idx) pend.erase(pend.begin() + (k - 1));
    }
    pb.active = false;
    pb.gotCount = 0;
    pb.buf.release();
    std::vector<unsigned char>().swap(pb.state);
    std::vector<unsigned char>().swap(pb.persisted);
    pb.fromDisk = false;
    for (size_t k = 0; k < x.activeList.size(); k++) {
        if (x.activeList[k] == idx) { x.activeList.erase(x.activeList.begin() + k); break; }
    }
    x.pieces.release(idx);
}

// Мягкое освобождение: уже полученные блоки остаются, снимаются только запросы, на которые так и не
// пришёл ответ, чтобы их мог взять другой пир. (Раньше кусок выбрасывался целиком, и при кусках по
// 8 МБ и капризных пирах несколько мегабайт загрузки пропадало зря.)
static void soften_piece(Ctx& x, std::vector<Conn>& conns, uint32_t idx) {
    if (!x.pieces.active(idx)) return;
    PieceBuf& pb = x.pieces[idx];
    for (size_t i = 0; i < conns.size(); i++) {
        std::vector<Req>& pend = conns[i].pending;
        for (size_t k = pend.size(); k > 0; k--)
            if (pend[k - 1].piece == idx) pend.erase(pend.begin() + (k - 1));
    }
    for (size_t b = 0; b < pb.state.size(); b++)
        if (pb.state[b] == 1) pb.state[b] = 0;
}

// Давно нет никаких данных: закрываем все соединения, забываем текущие куски и "чёрный список" пиров.
// Это то же самое, что раньше давали пауза и старт, только без перепроверки файлов.
static void soft_restart(Ctx& x, std::vector<Conn>& conns, std::vector<Peer>& candidates) {
    for (size_t i = 0; i < conns.size(); i++) conn_close(x, conns[i], "restart");
    conns.clear();
    candidates.clear();
    // Полученные блоки текущих кусков сохраняем (запросы уже сняты при закрытии соединений).
    x.failedAt.clear();
}

// Запрос к трекерам идёт в отдельном потоке: ответ может занять секунды (у каждого трекера до 8 с на
// подключение и столько же на ответ), и раньше на это время замирала вся программа, в том числе
// кнопки веб-страницы. Задача общая для потока и раздачи (shared_ptr): если раздачу остановили, пока
// поток ещё ждёт трекер, поток спокойно доработает и результат просто выбросится.
struct AnnounceJob {
    std::vector<std::string> urls;
    unsigned char infoHash[20];
    unsigned char peerId[20];
    uint64_t left;
    std::vector<Peer> peers;
    std::atomic<int> done;
    AnnounceJob() : left(0), done(0) { memset(infoHash, 0, 20); memset(peerId, 0, 20); }
};

static void* announce_thread(void* arg) {
    std::shared_ptr<AnnounceJob>* sp = (std::shared_ptr<AnnounceJob>*)arg;
    std::shared_ptr<AnnounceJob> job = *sp;
    delete sp;
    try {
        for (size_t k = 0; k < job->urls.size(); k++) {
            const std::string& url = job->urls[k];
            AnnounceResult r;
            if (tracker_announce_http(url, job->infoHash, job->peerId, job->left, r)) {
                logf_("  tracker %s: %d peers", url.c_str(), (int)r.peers.size());
                job->peers.insert(job->peers.end(), r.peers.begin(), r.peers.end());
            } else {
                logf_("  tracker %s failed: %s", url.c_str(), r.error.c_str());
            }
        }
    } catch (...) {
        logf_("  tracker request failed (exception)");
    }
    job->done.store(1);
    return NULL;
}

// Запускает опрос трекеров в потоке. false: подходящих трекеров нет или поток создать не удалось.
static bool start_announce(const Torrent& t, const unsigned char peerId[20], uint64_t left,
                           std::shared_ptr<AnnounceJob>& out) {
    std::shared_ptr<AnnounceJob> job(new AnnounceJob());
    for (size_t k = 0; k < t.trackers.size(); k++) {
        const std::string& url = t.trackers[k];
        if (url.compare(0, 7, "http://") != 0) continue;
        // адреса локальной сети провайдера (retracker.local) не разрешаются
        if (url.find(".local") != std::string::npos) continue;
        job->urls.push_back(url);
    }
    if (job->urls.empty()) return false;
    memcpy(job->infoHash, t.infoHash, 20);
    memcpy(job->peerId, peerId, 20);
    job->left = left;

    std::shared_ptr<AnnounceJob>* arg = new std::shared_ptr<AnnounceJob>(job);
    pthread_t th;
    if (pthread_create(&th, NULL, announce_thread, arg) != 0) { delete arg; return false; }
    pthread_detach(th);
    out = job;
    return true;
}

// ================================================================================================
// Swarm: загрузка одной раздачи. Всё, что раньше было локальными переменными одной большой
// функции, теперь лежит в Impl, а цикл разбит на prepare() и process() вокруг общего select().
// ================================================================================================

struct Swarm::Impl {
    Ctx x;
    unsigned char peerId[20];
    std::vector<Conn> conns;
    std::vector<Peer> candidates;
    std::shared_ptr<AnnounceJob> pendingAnnounce;   // идущий сейчас опрос трекеров (если есть)

    time_t t0, lastAnnounce, lastLog, lastProgress, lastDataTime, lastSpeedT;
    uint64_t lastBytes, prevTotal, lastSpeedBytes;
    uint32_t lastHave;
    int curSpeedKB, lastReady, lastSending, maxConns;
    double smoothKB = 0;                 // сглаженная скорость (данные приходят пачками)
    bool finished;
    std::string reason;

    Impl(const Torrent& t, Storage& st, DownloadState& ds, const unsigned char pid[20])
        : x(t, st, ds), t0(time(NULL)), lastAnnounce(0), lastLog(t0), lastProgress(t0),
          lastDataTime(t0), lastSpeedT(t0), lastBytes(0), prevTotal(0), lastSpeedBytes(0),
          lastHave(ds.haveCount), curSpeedKB(0), lastReady(0), lastSending(0), maxConns(DEFAULT_MAX_CONNS),
          finished(false)
    {
        memcpy(peerId, pid, 20);
    }

    void finish(const char* why) {
        if (finished) return;
        finished = true;
        reason = why;
        logf_("  [%s] swarm finished: %s", x.tag.c_str(), why);
    }
};

Swarm::Swarm(const Torrent& t, Storage& st, DownloadState& ds, const unsigned char peerId[20])
    : p(new Impl(t, st, ds, peerId))
{
    Ctx& x = p->x;
    memcpy(x.selfId, peerId, 20);
    x.pieces.resize(t.numPieces);
    size_t ma = MAX_ACTIVE_BYTES / t.pieceLength;
    if (ma < 2) ma = 2;
    if (ma > 64) ma = 64;
    x.maxActive = ma;
    x.tag = t.infoHashHex.size() >= 6 ? t.infoHashHex.substr(0, 6) : std::string("------");
    logf_("  [%s] up to %d pieces of %u KB (%u MB) are held in memory at once", x.tag.c_str(), (int)ma, t.pieceLength / 1024, (unsigned)((uint64_t)ma * t.pieceLength >> 20));

    // Восстанавливаем недокачанные куски, сохранённые раньше: блоки читаем обратно из итоговых файлов.
    // Если что-то не совпадёт, ничего страшного: готовый кусок всё равно проверяется по SHA-1.
    unsigned restoredPieces = 0, restoredBlocks = 0;
    for (size_t i = 0; i < ds.partials.size(); i++) {
        const PartialPiece& pp = ds.partials[i];
        if (pp.index >= t.numPieces || ds.have[pp.index] || x.pieces.active(pp.index) || (!ds.skip.empty() && ds.skip[pp.index])) continue;
        uint32_t psize = st.piece_size(pp.index);
        uint32_t nb = (psize + BLOCK - 1) / BLOCK;
        if (pp.nblocks != nb || pp.got.size() != nb) continue;
        if (x.activeList.size() >= x.maxActive + SPARE_ACTIVE) break;

        PieceBuf& pb = x.pieces[pp.index];
        if (!pb.buf.alloc(psize)) break;
        pb.psize = psize;
        pb.nblocks = nb;
        pb.gotCount = 0;
        pb.state.assign(nb, 0);
        pb.persisted.assign(nb, 0);
        for (uint32_t b = 0; b < nb; b++) {
            if (!pp.got[b]) continue;
            uint32_t off = b * BLOCK;
            uint32_t len = std::min<uint32_t>(BLOCK, psize - off);
            if (st.read_piece_part(pp.index, off, pb.buf.data() + off, len)) {
                pb.state[b] = 2;
                pb.persisted[b] = 1;
                pb.gotCount++;
            }
        }
        if (pb.gotCount == 0) {
            pb.buf.release();
            std::vector<unsigned char>().swap(pb.state);
            std::vector<unsigned char>().swap(pb.persisted);
    pb.fromDisk = false;
            x.pieces.release(pp.index);
            continue;
        }
        pb.active = true;
        pb.fromDisk = true;
        pb.lastActivity = pb.lastData = time(NULL);
        pb.softAt = 0;
        x.activeList.push_back(pp.index);
        restoredPieces++;
        restoredBlocks += pb.gotCount;
        if (pb.gotCount == pb.nblocks) finish_piece(x, pp.index);     // были получены все блоки: можно завершать
    }
    ds.partials.clear();
    if (restoredPieces) logf_("  [%s] restored %u partial pieces (%u blocks) from the previous run", x.tag.c_str(), restoredPieces, restoredBlocks);
}

void Swarm::network_changed()
{
    p->x.failedAt.clear();
    p->lastAnnounce = 0;
}

void Swarm::persist_partials_async(std::vector<PartialJobInfo>& out)
{
    Ctx& x = p->x;
    out.clear();
    const uint32_t MAX_RUN_BLOCKS = 256;                // серия не длиннее 4 МБ
    for (size_t k = 0; k < x.activeList.size(); k++) {
        uint32_t idx = x.activeList[k];
        PieceBuf& pb = x.pieces[idx];
        if (!pb.active || pb.writing || pb.gotCount == 0 || pb.gotCount >= pb.nblocks) continue;
        if (pb.persisted.size() != pb.nblocks) pb.persisted.assign(pb.nblocks, 0);

        PartialJobInfo pi;
        pi.index = idx;
        pi.nblocks = pb.nblocks;
        pi.before.assign(pb.nblocks, 0);
        pi.after.assign(pb.nblocks, 0);
        std::vector<std::pair<uint32_t, uint32_t> > runs;
        uint32_t b = 0;
        while (b < pb.nblocks) {
            if (pb.persisted[b]) { pi.before[b] = pi.after[b] = 1; b++; continue; }
            if (pb.state[b] != 2) { b++; continue; }
            uint32_t start = b;
            while (b < pb.nblocks && pb.state[b] == 2 && !pb.persisted[b] && (b - start) < MAX_RUN_BLOCKS) { pi.after[b] = 1; b++; }
            uint32_t off = start * BLOCK;
            uint32_t end = std::min<uint32_t>(b * BLOCK, pb.psize);
            runs.push_back(std::make_pair(off, end - off));
        }
        if (!runs.empty()) {
            WriteJobPtr job(new WriteJob());
            job->st = &x.st;
            job->index = idx;
            job->buf = std::move(pb.buf);                // раздача останавливается: буфер теперь принадлежит задаче
            job->runs = runs;
            pi.job = job;
            writer_submit(job);
        }
        out.push_back(pi);
    }
}

void Swarm::flush_writes(int budgetMs)
{
    Ctx& x = p->x;
    uint64_t t0 = mono_ms();
    while (!x.writing.empty() && !x.fatal) {
        if (!poll_write_jobs(x)) break;
        if (x.writing.empty()) break;
        if (budgetMs > 0 && mono_ms() - t0 >= (uint64_t)budgetMs) {
            logf_("  [%s] %d verified pieces are still being written to the drive, giving up waiting (%d ms)", x.tag.c_str(),
                  (int)x.writing.size(), budgetMs);
            break;
        }
        usleep(5000);
    }
}

void Swarm::forget_persisted()
{
    Ctx& x = p->x;
    for (size_t k = 0; k < x.activeList.size(); k++) {
        PieceBuf& pb = x.pieces[x.activeList[k]];
        if (!pb.writing) std::fill(pb.persisted.begin(), pb.persisted.end(), (unsigned char)0);
    }
}

int Swarm::pending_writes() const { return (int)p->x.writing.size(); }
int Swarm::failed_writes() const { return p->x.writeFailed; }

int Swarm::poll_writes()
{
    Ctx& x = p->x;
    if (!x.writing.empty()) poll_write_jobs(x);
    return (int)x.writing.size();
}

void Swarm::stop_network()
{
    Impl& s = *p;
    for (size_t i = 0; i < s.conns.size(); i++) conn_close(s.x, s.conns[i], "restart");
    s.conns.clear();
    s.candidates.clear();
    s.pendingAnnounce.reset();
}

Swarm::~Swarm()
{
    for (size_t i = 0; i < p->conns.size(); i++)
        if (p->conns[i].fd >= 0) close(p->conns[i].fd);
    delete p;
}

int Swarm::adopt(int fd, const std::string& key, const std::string& initial)
{
    Impl& s = *p;
    Ctx& x = s.x;
    if (s.finished || fd < 0 || fd >= FD_SETSIZE) return 0;
    if (initial.size() < 68) return 0;

    // Сверх обычного лимита разрешаем немного входящих, но не бесконечно.
    if (s.conns.size() >= (size_t)s.maxConns + 3) return 0;

    // Дубликат: этот пир (по peer id) уже подключён.
    std::string rid = initial.substr(48, 20);
    for (size_t i = 0; i < s.conns.size(); i++) {
        if (s.conns[i].fd >= 0 && !s.conns[i].remoteId.empty() && s.conns[i].remoteId == rid) {
            close(fd);
            return 2;
        }
    }

    time_t now = time(NULL);
    Conn c;
    c.fd = fd;
    c.state = 1;
    c.key = key;
    c.started = now;
    c.lastRecv = now;
    c.incoming = true;
    c.in = initial;
    c.has.assign(x.t.numPieces, false);

    unsigned char hs[68];
    hs[0] = 19;
    memcpy(hs + 1, "BitTorrent protocol", 19);
    memset(hs + 20, 0, 8);
    memcpy(hs + 28, x.t.infoHash, 20);
    memcpy(hs + 48, s.peerId, 20);
    c.out.append((const char*)hs, 68);

    s.conns.push_back(c);
    Conn& r = s.conns.back();

    // Handshake пира уже у нас в буфере: разбираем его сразу и отвечаем своим.
    bool ok = parse_input(x, r) && flush_out(r);
    if (!ok) {
        conn_close(x, r, r.failWhy.empty() ? "incoming handshake failed" : r.failWhy.c_str());
        return 2;
    }

    x.incomingTotal++;
    if (x.incomingTotal <= 20 || x.incomingTotal % 10 == 0)
        logf_("  [%s] incoming peer %s connected (%u incoming so far)", x.tag.c_str(), key.c_str(), x.incomingTotal);
    return 1;
}

bool Swarm::finished() const { return p->finished; }
bool Swarm::complete() const { return p->x.ds.needLeft == 0; }
const char* Swarm::reason() const { return p->reason.c_str(); }
void Swarm::set_max_conns(int n) { p->maxConns = n < 4 ? 4 : n; }

// Нехватка памяти: закрываем все соединения (их буферы и списки кусков); раздача подключится заново, когда память освободится.
void Swarm::trim_memory()
{
    for (size_t i = 0; i < p->conns.size(); i++)
        if (p->conns[i].fd >= 0) conn_close(p->x, p->conns[i], "out of memory");
    // Переподключиться не через минуту (штатное правило "60 секунд без данных"), а через ~5 секунд: за это время память освободится.
    p->prevTotal = p->x.totalBytes;                    // чтобы следующий проход не принял уже полученные байты за свежие данные
    p->lastDataTime = time(NULL) - STALL_RESTART_SECONDS + 5;
}

SwarmStatus Swarm::status() const
{
    SwarmStatus ss;
    ss.totalBytes = p->x.totalBytes;
    ss.peers = (int)p->conns.size();
    ss.ready = p->lastReady;
    ss.sending = p->lastSending;
    ss.haveCount = p->x.ds.haveCount;
    ss.speedKB = p->curSpeedKB;
    return ss;
}

int Swarm::prepare(time_t now, fd_set& rf, fd_set& wf)
{
    Impl& s = *p;
    Ctx& x = s.x;
    const Torrent& t = x.t;
    DownloadState& ds = x.ds;

    if (s.finished) return -1;
    if (ds.needLeft == 0) { s.finish("complete"); return -1; }
    if (x.fatal) { s.finish("write failed"); return -1; }

    if (x.totalBytes != s.prevTotal) { s.prevTotal = x.totalBytes; s.lastDataTime = now; }

    // Куски, по которым давно ничего не приходит: сначала перезапрашиваем недостающие блоки
    // (полученные остаются), и только если совсем ничего нет 10 минут, выбрасываем кусок.
    for (size_t k = x.activeList.size(); k > 0; k--) {
        uint32_t pidx = x.activeList[k - 1];
        PieceBuf& pb = x.pieces[pidx];
        if (pb.writing) continue;                         // проверенный кусок ждёт записи на диск
        int idle = (int)(now - pb.lastData);
        if (idle > PIECE_DROP_SECONDS) {
            logf_("  [%s] piece %u got nothing for %d s, dropping it (%u of %u blocks lost)", x.tag.c_str(), pidx,
                  idle, pb.gotCount, pb.nblocks);
            evict_piece(x, s.conns, pidx);
        } else if (idle > PIECE_STALE_SECONDS && now - pb.softAt > PIECE_STALE_SECONDS) {
            pb.softAt = now;
            logf_("  [%s] piece %u stalled for %d s, re-requesting missing blocks (keeping %u of %u)", x.tag.c_str(),
                  pidx, idle, pb.gotCount, pb.nblocks);
            soften_piece(x, s.conns, pidx);
        }
    }

    // Совсем нет данных: пересоздаём соединения и сразу спрашиваем у трекера свежий список пиров.
    if (now - s.lastDataTime > STALL_RESTART_SECONDS) {
        logf_("  [%s] no data for %d s, restarting connections", x.tag.c_str(), STALL_RESTART_SECONDS);
        soft_restart(x, s.conns, s.candidates);
        s.lastAnnounce = 0;
        s.lastDataTime = now;
    }

    if (now - s.t0 > 86400) { s.finish("time limit"); return -1; }
    // Прогресс = завершённый кусок ИЛИ хоть какие-то данные (при кусках по 8 МБ и медленных пирах
    // кусок может собираться дольше 5 минут).
    if (now - std::max(s.lastProgress, s.lastDataTime) > 300) { s.finish("no data for 5 minutes"); return -1; }

    // Запрос новых пиров у трекеров: идёт в отдельном потоке, здесь только запускаем его и забираем результат.
    size_t wantMore = (size_t)(s.maxConns < 10 ? s.maxConns : 10);
    if (!s.pendingAnnounce && s.candidates.empty() && s.conns.size() < wantMore &&
        (s.lastAnnounce == 0 ||
         now - s.lastAnnounce >= (s.lastReady == 0 ? ANNOUNCE_NO_PEERS_SECONDS : ANNOUNCE_NORMAL_SECONDS))) {
        uint64_t done = (uint64_t)ds.haveCount * t.pieceLength;
        uint64_t left = done >= t.totalSize ? 0 : t.totalSize - done;
        if (!start_announce(t, s.peerId, left, s.pendingAnnounce)) s.lastAnnounce = now;     // нечего спрашивать: повтор позже
    }
    if (s.pendingAnnounce && s.pendingAnnounce->done.load() == 1) {
        std::vector<Peer> got;
        got.swap(s.pendingAnnounce->peers);
        s.pendingAnnounce.reset();
        now = time(NULL);
        s.lastAnnounce = now;
        int added = 0;
        // Память: забываем давние неудачные адреса, а список ожидающих пиров держим небольшим.
        for (std::map<std::string, time_t>::iterator f = x.failedAt.begin(); f != x.failedAt.end();) {
            if (now - f->second >= 120) x.failedAt.erase(f++); else ++f;
        }
        for (size_t i = 0; i < got.size(); i++) {
            if (s.candidates.size() >= 150) break;
            std::string key = peer_key(got[i]);
            bool skip = false;
            std::map<std::string, time_t>::iterator f = x.failedAt.find(key);
            if (f != x.failedAt.end() && now - f->second < 120) skip = true;
            for (size_t j = 0; j < s.conns.size() && !skip; j++)
                if (s.conns[j].fd >= 0 && s.conns[j].key == key) skip = true;
            for (size_t j = 0; j < s.candidates.size() && !skip; j++)
                if (peer_key(s.candidates[j]) == key) skip = true;
            if (skip) continue;
            s.candidates.push_back(got[i]);
            added++;
        }
        logf_("  [%s] announce: %d peers, %d new candidates", x.tag.c_str(), (int)got.size(), added);
    }

    // Новые соединения
    while (!s.candidates.empty() && s.conns.size() < (size_t)s.maxConns) {
        Peer pr = s.candidates.back();
        s.candidates.pop_back();
        std::string key = peer_key(pr);
        char ip[32];
        snprintf(ip, sizeof(ip), "%u.%u.%u.%u", pr.ip[0], pr.ip[1], pr.ip[2], pr.ip[3]);
        int fd = net_connect_nb(ip, pr.port);
        if (fd < 0) {
            char w[48];
            snprintf(w, sizeof(w), "connecting: immediate error %d", errno);
            x.failStats[w]++;
            x.failedAt[key] = now;
            continue;
        }
        if (fd >= FD_SETSIZE) { close(fd); continue; }
        Conn c;
        c.fd = fd;
        c.state = 0;
        c.key = key;
        c.started = now;
        c.lastRecv = now;
        c.has.assign(t.numPieces, false);
        s.conns.push_back(c);
    }

    // Наши сокеты для общего select
    int maxfd = -1;
    for (size_t i = 0; i < s.conns.size(); i++) {
        Conn& c = s.conns[i];
        if (c.fd < 0) continue;
        if (c.state == 0) FD_SET(c.fd, &wf);
        else FD_SET(c.fd, &rf);
        if (c.state > 0 && !c.out.empty()) FD_SET(c.fd, &wf);
        if (c.fd > maxfd) maxfd = c.fd;
    }
    return maxfd;
}

void Swarm::process(time_t now, fd_set& rf, fd_set& wf)
{
    Impl& s = *p;
    if (s.finished) return;

    Ctx& x = s.x;
    const Torrent& t = x.t;
    DownloadState& ds = x.ds;

    // Какие проверенные куски поток записи уже записал на диск.
    if (!x.writing.empty() && !poll_write_jobs(x)) { s.finish("write failed"); return; }

    // Скорость за последнюю секунду
    if (now != s.lastSpeedT) {
        // Данные приходят пачками, поэтому скорость за одну секунду сильно прыгает (то 0, то 600 КБ/с).
        // Показываем экспоненциальное среднее: за каждую прошедшую секунду 15% новой, 85% прежней.
        uint64_t dt = (uint64_t)(now - s.lastSpeedT);
        double inst = (double)(x.totalBytes - s.lastSpeedBytes) / 1024.0 / (double)dt;
        for (uint64_t k = 0; k < dt && k < 30; k++) s.smoothKB = s.smoothKB * 0.85 + inst * 0.15;
        s.curSpeedKB = (int)(s.smoothKB + 0.5);
        s.lastSpeedBytes = x.totalBytes;
        s.lastSpeedT = now;
    }

    for (size_t i = 0; i < s.conns.size(); i++) {
        Conn& c = s.conns[i];
        if (c.fd < 0) continue;
        if (c.state == 0) {
            if (!FD_ISSET(c.fd, &wf)) continue;
            int err = 0;
            socklen_t len = sizeof(err);
            getsockopt(c.fd, SOL_SOCKET, SO_ERROR, &err, &len);
            if (err) {
                char w[48];
                snprintf(w, sizeof(w), "connect failed (error %d)", err);
                conn_close(x, c, w);
                continue;
            }
            c.state = 1;
            c.started = now;
            c.lastRecv = now;
            unsigned char hs[68];
            hs[0] = 19;
            memcpy(hs + 1, "BitTorrent protocol", 19);
            memset(hs + 20, 0, 8);
            memcpy(hs + 28, t.infoHash, 20);
            memcpy(hs + 48, s.peerId, 20);
            c.out.append((const char*)hs, 68);
            if (!flush_out(c)) conn_close(x, c, c.failWhy.empty() ? "send failed" : c.failWhy.c_str());
            continue;
        }
        if (FD_ISSET(c.fd, &rf)) {
            if (!do_read(x, c)) { conn_close(x, c, c.failWhy.empty() ? "read error or closed" : c.failWhy.c_str()); continue; }
        }
        if (c.fd >= 0 && !c.out.empty() && FD_ISSET(c.fd, &wf)) {
            if (!flush_out(c)) conn_close(x, c, c.failWhy.empty() ? "send failed" : c.failWhy.c_str());
        }
    }

    // Новые запросы блоков и таймауты
    int ready = 0, sending = 0;
    for (size_t i = 0; i < s.conns.size(); i++) {
        Conn& c = s.conns[i];
        if (c.fd < 0) continue;
        if (c.state == 0) {
            if (now - c.started > CONNECT_TIMEOUT_SECONDS) conn_close(x, c, "connect timeout");
        } else if (c.state == 1) {
            if (now - c.started > HANDSHAKE_TIMEOUT_SECONDS) conn_close(x, c, "handshake timeout");
        } else {
            if (now - c.lastRecv > 120) { conn_close(x, c, "idle"); continue; }
            // Тишина считается от последнего пришедшего блока (или от отправки самого старого запроса, если он новее):
            // медленный, но живой пир, который шлёт блоки, не отпускается.
            if (!c.pending.empty() && now - std::max(c.lastBlock, c.pending[0].sent) > PEER_SILENT_SECONDS) {
                conn_close(x, c, "request timeout");
                continue;
            }
            if (!fill_requests(x, c)) { conn_close(x, c, c.failWhy.empty() ? "send failed" : c.failWhy.c_str()); continue; }
            ready++;
            if (c.lastBlock != 0 && now - c.lastBlock <= 10) sending++;
        }
    }
    s.lastReady = ready;
    s.lastSending = sending;
    for (size_t i = s.conns.size(); i > 0; i--)
        if (s.conns[i - 1].fd < 0) s.conns.erase(s.conns.begin() + (i - 1));

    if (ds.haveCount != s.lastHave) { s.lastHave = ds.haveCount; s.lastProgress = now; }

    if (now - s.lastLog >= 5) {
        uint64_t kbps = (x.totalBytes - s.lastBytes) / 1024 / (uint64_t)(now - s.lastLog);
        logf_("  [%s] %u/%u pieces (%u%%), peers %d (ready %d, sending %d), %llu KB/s",
              x.tag.c_str(), ds.haveCount, t.numPieces, ds.haveCount * 100 / t.numPieces,
              (int)s.conns.size(), ready, sending, (unsigned long long)kbps);
        s.lastBytes = x.totalBytes;
        s.lastLog = now;
    }

    if (x.lastFailLog == 0) x.lastFailLog = now;
    if (now - x.lastFailLog >= 60) {
        if (!x.failStats.empty()) {
            std::string line;
            for (std::map<std::string, int>::iterator it = x.failStats.begin(); it != x.failStats.end(); ++it) {
                char n[16];
                snprintf(n, sizeof(n), " x%d", it->second);
                line += "\n      " + it->first + n;
            }
            logf_("  [%s] connections closed in the last %d s:%s", x.tag.c_str(), (int)(now - x.lastFailLog), line.c_str());
            x.failStats.clear();
        }
        x.lastFailLog = now;
    }

    if (x.fatal) s.finish("write failed");
    else if (ds.needLeft == 0) s.finish("complete");
}
