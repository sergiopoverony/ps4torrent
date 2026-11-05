#include "metadata.h"
#include "bencode.h"
#include "log.h"
#include "net.h"
#include "sha1.h"
#include "tracker.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>

static const int MAX_PEERS_TRIED = 60;
static const int WORKERS = 6;
static const int CONNECT_MS = 4000;
static const int PEER_IDLE_S = 12;            // пир молчит дольше - бросаем
static const unsigned MSG_MAX = 256 * 1024;   // чужие сообщения больше (bitfield у огромных раздач) не читаем
static const unsigned PIECE = 16384;

struct Shared {
    pthread_mutex_t mu;
    std::vector<Peer> peers;
    size_t next;
    bool done;
    std::string info;
    unsigned char ih[20];
    unsigned char pid[20];
    time_t deadline;
    bool (*stop)();
    int connected;
    std::string lastErr;
};

static bool quit_now(Shared* sh)
{
    if (sh->done) return true;
    if (time(NULL) >= sh->deadline) return true;
    if (sh->stop && sh->stop()) return true;
    return false;
}

static bool send_all(int fd, const void* d, size_t n)
{
    const char* p = (const char*)d;
    while (n > 0) {
        ssize_t k = send(fd, p, n, 0);
        if (k <= 0) return false;
        p += k; n -= (size_t)k;
    }
    return true;
}

// Читает ровно n байт; ждёт не дольше PEER_IDLE_S на каждый кусок и проверяет флаги остановки каждую секунду.
static bool recv_exact(Shared* sh, int fd, void* buf, size_t n)
{
    char* p = (char*)buf;
    time_t last = time(NULL);
    while (n > 0) {
        if (quit_now(sh)) return false;
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(fd, &rf);
        struct timeval tv = {1, 0};
        int r = select(fd + 1, &rf, NULL, NULL, &tv);
        if (r < 0) return false;
        if (r == 0) { if (time(NULL) - last >= PEER_IDLE_S) return false; continue; }
        ssize_t k = recv(fd, p, n, 0);
        if (k <= 0) return false;
        p += k; n -= (size_t)k;
        last = time(NULL);
    }
    return true;
}

static bool send_ext(int fd, int extId, const std::string& payload)
{
    std::string m;
    unsigned len = (unsigned)payload.size() + 2;
    m += (char)(len >> 24); m += (char)(len >> 16); m += (char)(len >> 8); m += (char)len;
    m += (char)20;
    m += (char)extId;
    m += payload;
    return send_all(fd, m.data(), m.size());
}

// Работа с одним пиром. true: описание получено и проверено (лежит в sh->info).
static bool try_peer(Shared* sh, const Peer& p)
{
    char ip[24];
    snprintf(ip, sizeof(ip), "%u.%u.%u.%u", p.ip[0], p.ip[1], p.ip[2], p.ip[3]);
    int fd = net_connect(ip, p.port, CONNECT_MS);
    if (fd < 0) return false;
    pthread_mutex_lock(&sh->mu); sh->connected++; pthread_mutex_unlock(&sh->mu);

    bool ok = false;
    std::string meta;                 // собираемое описание
    std::vector<bool> got;
    unsigned mdSize = 0, nPieces = 0, gotCount = 0;
    int peerExtId = 0;

    do {
        unsigned char hs[68];
        hs[0] = 19;
        memcpy(hs + 1, "BitTorrent protocol", 19);
        memset(hs + 20, 0, 8);
        hs[25] |= 0x10;               // расширения (BEP 10)
        memcpy(hs + 28, sh->ih, 20);
        memcpy(hs + 48, sh->pid, 20);
        if (!send_all(fd, hs, 68)) break;
        unsigned char rh[68];
        if (!recv_exact(sh, fd, rh, 68)) break;
        if (rh[0] != 19 || memcmp(rh + 1, "BitTorrent protocol", 19) != 0 || memcmp(rh + 28, sh->ih, 20) != 0) break;
        if (!(rh[25] & 0x10)) break;  // пир не умеет расширения: описание у него не взять
        if (!send_ext(fd, 0, "d1:md11:ut_metadatai1eee")) break;

        for (;;) {
            unsigned char lb[4];
            if (!recv_exact(sh, fd, lb, 4)) break;
            unsigned len = ((unsigned)lb[0] << 24) | ((unsigned)lb[1] << 16) | ((unsigned)lb[2] << 8) | lb[3];
            if (len == 0) continue;
            if (len > MSG_MAX + 4096) break;
            std::string msg(len, '\0');
            if (!recv_exact(sh, fd, &msg[0], len)) break;
            if ((unsigned char)msg[0] != 20 || len < 2) continue;      // не расширение: пропускаем (bitfield, have, ...)
            int eid = (unsigned char)msg[1];
            std::string pl = msg.substr(2);
            if (eid == 0) {                                            // их handshake расширений
                BNode r;
                if (!bdecode(pl, r) || r.type != BNode::DICT) break;
                const BNode* m = r.find("m");
                const BNode* ms = r.find("metadata_size");
                const BNode* um = m ? m->find("ut_metadata") : NULL;
                if (!um || um->type != BNode::INT || um->i <= 0 || um->i > 255) break;
                if (!ms || ms->type != BNode::INT || ms->i <= 0 || ms->i > (long long)METADATA_MAX_BYTES) break;
                peerExtId = (int)um->i;
                mdSize = (unsigned)ms->i;
                nPieces = (mdSize + PIECE - 1) / PIECE;
                meta.assign(mdSize, '\0');
                got.assign(nPieces, false);
                bool sentOk = true;
                for (unsigned i = 0; i < nPieces && sentOk; i++)
                    sentOk = send_ext(fd, peerExtId, "d8:msg_typei0e5:piecei" + std::to_string(i) + "ee");
                if (!sentOk) break;
            } else if (eid == 1 && mdSize > 0) {                      // ответ ut_metadata (наш номер = 1)
                BNode r;
                if (!bdecode(pl, r) || r.type != BNode::DICT) break;
                const BNode* mt = r.find("msg_type");
                const BNode* pc = r.find("piece");
                if (!mt || mt->type != BNode::INT || !pc || pc->type != BNode::INT) break;
                if (mt->i == 2) break;                                 // reject: у этого пира описания нет
                if (mt->i != 1 || pc->i < 0 || (unsigned)pc->i >= nPieces) continue;
                unsigned idx = (unsigned)pc->i;
                size_t dataOff = r.end;
                size_t want = (idx + 1 == nPieces) ? mdSize - idx * PIECE : PIECE;
                if (pl.size() - dataOff != want) break;
                if (!got[idx]) {
                    memcpy(&meta[(size_t)idx * PIECE], pl.data() + dataOff, want);
                    got[idx] = true;
                    gotCount++;
                }
                if (gotCount == nPieces) {
                    unsigned char h[20];
                    sha1((const unsigned char*)meta.data(), meta.size(), h);
                    if (memcmp(h, sh->ih, 20) == 0) ok = true;
                    break;                                             // хэш не сошёлся: пир врёт, бросаем
                }
            }
        }
    } while (0);
    close(fd);

    if (ok) {
        pthread_mutex_lock(&sh->mu);
        if (!sh->done) { sh->done = true; sh->info = meta; }
        pthread_mutex_unlock(&sh->mu);
    }
    return ok;
}

static void* worker(void* arg)
{
    Shared* sh = (Shared*)arg;
    for (;;) {
        Peer p;
        pthread_mutex_lock(&sh->mu);
        bool have = sh->next < sh->peers.size() && !sh->done;
        if (have) p = sh->peers[sh->next++];
        pthread_mutex_unlock(&sh->mu);
        if (!have || quit_now(sh)) break;
        try_peer(sh, p);
    }
    return NULL;
}

bool metadata_fetch(const unsigned char infoHash[20], const std::vector<std::string>& trackers, int budgetSec,
                    bool (*should_stop)(), std::string& info, std::string& err)
{
    info.clear();
    Shared sh;
    pthread_mutex_init(&sh.mu, NULL);
    sh.next = 0;
    sh.done = false;
    memcpy(sh.ih, infoHash, 20);
    sh.deadline = time(NULL) + budgetSec;
    sh.stop = should_stop;
    sh.connected = 0;
    // peer id: "-PT" + случайные символы (клиентский префикс, как у обычных клиентов)
    memcpy(sh.pid, "-PM1860-", 8);
    for (int i = 8; i < 20; i++) sh.pid[i] = (unsigned char)('0' + rand() % 10);

    std::string trErr;
    for (size_t t = 0; t < trackers.size(); t++) {
        if (quit_now(&sh)) break;
        AnnounceResult ar;
        if (!tracker_announce_http(trackers[t], infoHash, sh.pid, 1, ar)) { trErr = trackers[t] + ": " + ar.error; continue; }
        for (size_t k = 0; k < ar.peers.size(); k++) {
            bool dup = false;
            for (size_t j = 0; j < sh.peers.size(); j++)
                if (!memcmp(sh.peers[j].ip, ar.peers[k].ip, 4) && sh.peers[j].port == ar.peers[k].port) { dup = true; break; }
            if (!dup) sh.peers.push_back(ar.peers[k]);
        }
    }
    if (sh.peers.empty()) {
        err = trErr.empty() ? "tracker returned no peers" : trErr;
        pthread_mutex_destroy(&sh.mu);
        return false;
    }
    if (sh.peers.size() > (size_t)MAX_PEERS_TRIED) sh.peers.resize(MAX_PEERS_TRIED);

    pthread_t th[WORKERS];
    int started = 0;
    for (int i = 0; i < WORKERS; i++)
        if (pthread_create(&th[started], NULL, worker, &sh) == 0) started++;
    if (started == 0) worker(&sh);
    for (int i = 0; i < started; i++) pthread_join(th[i], NULL);

    bool ok = sh.done;
    if (ok) info = sh.info;
    else err = sh.connected == 0 ? "no peer reachable (" + std::to_string(sh.peers.size()) + " from tracker)"
                                  : "peers reached, but none gave the metadata";
    pthread_mutex_destroy(&sh.mu);
    return ok;
}
