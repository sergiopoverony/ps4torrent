#include "incoming.h"
#include "log.h"
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <vector>

namespace {

struct Pending {
    int fd;
    time_t started;
    std::string key;     // "ip:port" пира
    std::string buf;     // что уже прислал пир
};

int g_lfd = -1;
int g_port = 0;
std::vector<Pending> g_pending;
unsigned g_accepted = 0, g_rejected = 0;
int g_acceptFails = 0;                // подряд неудачных accept()
bool g_broken = false;                // слушающий сокет сломан: главный поток должен создать его заново

const size_t MAX_PENDING = 32;        // столько соединений одновременно ждут handshake
const int HANDSHAKE_WAIT_SECONDS = 10;
const size_t HANDSHAKE_LEN = 68;

void set_nonblock(int fd)
{
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

void drop(size_t i)
{
    close(g_pending[i].fd);
    g_pending.erase(g_pending.begin() + (long)i);
    g_rejected++;
}

} // namespace

bool incoming_start(int port, int* err)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) { *err = errno; return false; }
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
#ifndef __linux__
    sa.sin_len = sizeof(sa);
#endif
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)port);
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, (struct sockaddr*)&sa, sizeof(sa)) < 0) { *err = errno; close(s); return false; }
    if (listen(s, 16) < 0) { *err = errno; close(s); return false; }
    set_nonblock(s);

    g_lfd = s;
    g_port = port;
    g_acceptFails = 0;
    g_broken = false;
    return true;
}

void incoming_stop()
{
    for (size_t i = 0; i < g_pending.size(); i++) close(g_pending[i].fd);
    g_pending.clear();
    if (g_lfd >= 0) close(g_lfd);
    g_lfd = -1;
    g_port = 0;
}

int incoming_port() { return g_port; }
bool incoming_take_broken() { bool b = g_broken; g_broken = false; return b; }
unsigned incoming_accepted() { return g_accepted; }
unsigned incoming_rejected() { return g_rejected; }

int incoming_prepare(fd_set& rf)
{
    int maxfd = -1;
    if (g_lfd >= 0) { FD_SET(g_lfd, &rf); maxfd = g_lfd; }
    for (size_t i = 0; i < g_pending.size(); i++) {
        FD_SET(g_pending[i].fd, &rf);
        if (g_pending[i].fd > maxfd) maxfd = g_pending[i].fd;
    }
    return maxfd;
}

void incoming_process(time_t now, fd_set& rf, IncomingAdoptFn adopt)
{
    if (g_lfd < 0) return;

    // 1. Новые соединения.
    if (FD_ISSET(g_lfd, &rf)) {
        for (int k = 0; k < 8; k++) {
            struct sockaddr_in pa;
            socklen_t pl = sizeof(pa);
            memset(&pa, 0, sizeof(pa));
            int c;
#ifdef HOST_TEST
            {   // тесты: пока в файле число больше нуля, accept() "ломается" (как после сна консоли)
                FILE* ff = fopen("/tmp/ft/incoming_accept_fail", "r");
                int n = 0;
                if (ff) { if (fscanf(ff, "%d", &n) != 1) n = 0; fclose(ff); }
                if (n > 0) {
                    ff = fopen("/tmp/ft/incoming_accept_fail", "w");
                    if (ff) { fprintf(ff, "%d", n - 1); fclose(ff); }
                    c = -1;
                    errno = 163;
                } else {
                    c = accept(g_lfd, (struct sockaddr*)&pa, &pl);
                }
            }
#else
            c = accept(g_lfd, (struct sockaddr*)&pa, &pl);
#endif
            if (c < 0) {
                int e = errno;
                if (e == EAGAIN || e == EWOULDBLOCK || e == EINTR || e == ECONNABORTED) break;
                if (++g_acceptFails >= 3) {
                    logf_("incoming: accept keeps failing (errno %d), the listening socket is broken", e);
                    close(g_lfd);              // иначе select() будет срабатывать впустую и грузить процессор
                    g_lfd = -1;
                    g_broken = true;
                }
                break;
            }
            g_acceptFails = 0;
            if (c >= FD_SETSIZE || g_pending.size() >= MAX_PENDING) { close(c); g_rejected++; continue; }
            set_nonblock(c);
#ifdef SO_NOSIGPIPE
            int one = 1;
            setsockopt(c, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
            char key[48];
            snprintf(key, sizeof(key), "%s:%u", inet_ntoa(pa.sin_addr), (unsigned)ntohs(pa.sin_port));
            Pending p;
            p.fd = c;
            p.started = now;
            p.key = key;
            g_pending.push_back(p);
        }
    }

    // 2. Ожидающие handshake: читаем, проверяем, передаём раздаче.
    for (size_t i = g_pending.size(); i > 0; i--) {
        size_t idx = i - 1;
        Pending& p = g_pending[idx];

        if (now - p.started > HANDSHAKE_WAIT_SECONDS) { drop(idx); continue; }
        if (!FD_ISSET(p.fd, &rf)) continue;

        char tmp[2048];
        ssize_t n = recv(p.fd, tmp, sizeof(tmp), 0);
        if (n == 0) { drop(idx); continue; }
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            drop(idx);
            continue;
        }
        p.buf.append(tmp, (size_t)n);

        // Это точно BitTorrent? Проверяем по мере поступления данных.
        static const char proto[] = "\x13" "BitTorrent protocol";
        size_t check = p.buf.size() < 20 ? p.buf.size() : 20;
        if (memcmp(p.buf.data(), proto, check) != 0) { drop(idx); continue; }
        if (p.buf.size() < HANDSHAKE_LEN) continue;       // ждём остальное

        const unsigned char* ih = (const unsigned char*)p.buf.data() + 28;
        int r = adopt ? adopt(ih, p.fd, p.key, p.buf) : 0;
        if (r == 0) { drop(idx); continue; }              // нет такой раздачи сейчас: закрываем
        // Сокет теперь принадлежит раздаче (она его закроет сама).
        if (r == 1) g_accepted++; else g_rejected++;
        g_pending.erase(g_pending.begin() + (long)idx);
    }
}
