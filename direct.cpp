// Прямые загрузки по ссылке http:// (см. direct.h).
#include "direct.h"
#include "net.h"
#include "log.h"
#include "sha1.h"
#include <atomic>
#include <set>
#include <vector>
#include <stdio.h>
#include <ctype.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/statvfs.h>

#ifndef LOG_DIR
#define LOG_DIR "/data/ps4torrent"
#endif
#define DIRECT_FILE LOG_DIR "/direct.txt"

#ifndef DIRECT_MAX
#define DIRECT_MAX 2                    // сколько прямых загрузок идёт одновременно (остальные ждут в очереди)
#endif
#ifndef DIRECT_RETRY_S
#define DIRECT_RETRY_S 15               // первый повтор после обрыва; дальше вдвое дольше, но не больше 5 минут
#endif

static const int IO_TIMEOUT_S = 30;     // столько ждём данных от сервера, потом считаем связь оборванной
static const size_t RECV_BUF = 64 * 1024;

// ---------------------------------------------------------------- адрес и запрос

struct Url { std::string host, path; int port = 80; };

bool direct_url_ok(const std::string& url, std::string& err)
{
    if (url.size() > 2000) { err = "the link is too long"; return false; }
    for (size_t i = 0; i < url.size(); i++)
        if ((unsigned char)url[i] < 0x21 || url[i] == 0x7f) { err = "the link contains spaces or control characters"; return false; }
    if (url.compare(0, 8, "https://") == 0 || url.compare(0, 8, "HTTPS://") == 0) { err = "https:// links are not supported yet, only http://"; return false; }
    if (url.compare(0, 7, "http://") != 0 && url.compare(0, 7, "HTTP://") != 0) { err = "only http:// links are supported"; return false; }
    if (url.size() <= 7 || url[7] == '/') { err = "the link has no server name"; return false; }
    return true;
}

static bool parse_url(const std::string& url, Url& u, std::string& err)
{
    if (!direct_url_ok(url, err)) return false;
    std::string rest = url.substr(7);
    size_t slash = rest.find('/');
    std::string hp = slash == std::string::npos ? rest : rest.substr(0, slash);
    u.path = slash == std::string::npos ? "/" : rest.substr(slash);
    size_t hash = u.path.find('#');
    if (hash != std::string::npos) u.path.erase(hash);
    size_t at = hp.rfind('@');
    if (at != std::string::npos) hp = hp.substr(at + 1);          // логин в адресе не поддерживаем: просто отбрасываем
    u.port = 80;
    size_t colon = hp.find(':');
    if (colon != std::string::npos) {
        u.port = atoi(hp.c_str() + colon + 1);
        hp = hp.substr(0, colon);
    }
    u.host = hp;
    if (u.host.empty() || u.port <= 0 || u.port > 65535) { err = "bad server name or port in the link"; return false; }
    return true;
}

// Байты больше 0x7F (русские буквы в адресе) кодируем как %XX
static std::string encode_path(const std::string& p)
{
    static const char* hx = "0123456789ABCDEF";
    std::string o;
    for (size_t i = 0; i < p.size(); i++) {
        unsigned char c = (unsigned char)p[i];
        if (c > 0x7e) { o += '%'; o += hx[c >> 4]; o += hx[c & 15]; } else o += (char)c;
    }
    return o;
}

static std::string lower(std::string s) { for (size_t i = 0; i < s.size(); i++) if (s[i] >= 'A' && s[i] <= 'Z') s[i] = (char)(s[i] - 'A' + 'a'); return s; }

static std::string header_value(const std::string& head, const char* name)
{
    std::string lh = lower(head), key = std::string("\n") + name + ":";
    size_t p = lh.find(key);
    if (p == std::string::npos) return "";
    p += key.size();
    size_t e = head.find('\r', p);
    if (e == std::string::npos) e = head.find('\n', p);
    std::string v = head.substr(p, e == std::string::npos ? std::string::npos : e - p);
    size_t a = v.find_first_not_of(" \t");
    return a == std::string::npos ? "" : v.substr(a);
}

static std::string url_unescape(const std::string& s)
{
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size() && isxdigit((unsigned char)s[i + 1]) && isxdigit((unsigned char)s[i + 2])) {
            o += (char)strtol(s.substr(i + 1, 2).c_str(), NULL, 16);
            i += 2;
        } else o += s[i];
    }
    return o;
}

// Имя файла, безопасное для любой файловой системы (exFAT/FAT32 на флешке)
static std::string safe_name(const std::string& in)
{
    std::string o;
    for (size_t i = 0; i < in.size(); i++) {
        unsigned char c = (unsigned char)in[i];
        if (c < 0x20 || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') o += '_';
        else o += (char)c;
    }
    while (!o.empty() && (o[0] == '.' || o[0] == ' ')) o.erase(0, 1);
    while (!o.empty() && (o[o.size() - 1] == '.' || o[o.size() - 1] == ' ')) o.erase(o.size() - 1);
    if (o.size() > 180) {                                          // обрезаем, не ломая UTF-8 и сохраняя расширение
        std::string ext;
        size_t dot = o.rfind('.');
        if (dot != std::string::npos && o.size() - dot <= 10) ext = o.substr(dot);
        size_t n = 180 - ext.size();
        while (n > 0 && ((unsigned char)o[n] & 0xC0) == 0x80) n--;
        o = o.substr(0, n) + ext;
    }
    if (o.empty()) o = "download.bin";
    return o;
}

static std::string name_from(const std::string& head, const std::string& path)
{
    std::string cd = header_value(head, "content-disposition");
    std::string lcd = lower(cd);
    size_t p = lcd.find("filename*=");
    if (p != std::string::npos) {                                  // filename*=UTF-8''%D0%B8%D0%BC%D1%8F.pkg
        std::string v = cd.substr(p + 10);
        size_t q = v.find("''");
        if (q != std::string::npos) v = v.substr(q + 2);
        size_t e = v.find(';');
        if (e != std::string::npos) v = v.substr(0, e);
        v = url_unescape(v);
        if (!v.empty()) return safe_name(v);
    }
    p = lcd.find("filename=");
    if (p != std::string::npos) {
        std::string v = cd.substr(p + 9);
        if (!v.empty() && v[0] == '"') { size_t e = v.find('"', 1); v = v.substr(1, e == std::string::npos ? std::string::npos : e - 1); }
        else { size_t e = v.find(';'); if (e != std::string::npos) v = v.substr(0, e); }
        if (!v.empty()) return safe_name(v);
    }
    std::string p2 = path;
    size_t q = p2.find('?');
    if (q != std::string::npos) p2 = p2.substr(0, q);
    size_t sl = p2.rfind('/');
    std::string last = sl == std::string::npos ? p2 : p2.substr(sl + 1);
    return safe_name(url_unescape(last));
}

static bool send_all_fd(int s, const std::string& d)
{
    size_t off = 0;
    while (off < d.size()) {
        ssize_t n = send(s, d.data() + off, d.size() - off, 0);
        if (n <= 0) return false;
        off += (size_t)n;
    }
    return true;
}

// Ждёт данных не дольше timeoutMs. 1: есть, 0: тишина, -1: ошибка.
static int wait_readable(int s, int timeoutMs)
{
    fd_set rf;
    FD_ZERO(&rf);
    FD_SET(s, &rf);
    struct timeval tv;
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    int r = select(s + 1, &rf, NULL, NULL, &tv);
    return r < 0 ? (errno == EINTR ? 0 : -1) : (r > 0 ? 1 : 0);
}

struct Resp {
    int sock = -1;
    int code = 0;
    std::string head;          // заголовки ответа
    std::string rest;          // начало тела, пришедшее вместе с заголовками
    std::string url;           // адрес после переадресаций
    std::string path;
};

// GET с Range (from > 0) и переходом по переадресациям (до 5). stop: прервать ожидание (поток загрузки останавливают).
static bool http_open(const std::string& url0, uint64_t from, Resp& r, std::string& err, std::atomic<bool>* stop)
{
    std::string url = url0;
    for (int hop = 0; hop < 6; hop++) {
        Url u;
        if (!parse_url(url, u, err)) return false;
        int s = net_connect(u.host.c_str(), u.port, 10000);
        if (s < 0) { err = "cannot connect to " + u.host + ":" + std::to_string(u.port); return false; }
        int fl = fcntl(s, F_GETFL, 0);
        fcntl(s, F_SETFL, fl & ~O_NONBLOCK);
        std::string hostHdr = u.host + (u.port != 80 ? ":" + std::to_string(u.port) : "");
        std::string req = "GET " + encode_path(u.path) + " HTTP/1.0\r\nHost: " + hostHdr +
                          "\r\nUser-Agent: ps4torrent\r\nAccept: */*\r\nConnection: close\r\n";
        if (from > 0) req += "Range: bytes=" + std::to_string((unsigned long long)from) + "-\r\n";
        req += "\r\n";
        if (!send_all_fd(s, req)) { close(s); err = "send failed"; return false; }

        std::string buf;
        size_t hend = std::string::npos;
        time_t deadline = time(NULL) + IO_TIMEOUT_S;
        char tmp[4096];
        while (hend == std::string::npos) {
            if (stop && stop->load()) { close(s); err = "stopped"; return false; }
            if (time(NULL) > deadline) { close(s); err = "the server did not answer"; return false; }
            int w = wait_readable(s, 500);
            if (w < 0) { close(s); err = "receive failed"; return false; }
            if (w == 0) continue;
            ssize_t n = recv(s, tmp, sizeof(tmp), 0);
            if (n <= 0) { close(s); err = "the server closed the connection"; return false; }
            buf.append(tmp, (size_t)n);
            hend = buf.find("\r\n\r\n");
            if (hend == std::string::npos && buf.size() > 32768) { close(s); err = "bad answer from the server"; return false; }
        }
        r.head = "\n" + buf.substr(0, hend);
        r.rest = buf.substr(hend + 4);
        size_t sp = buf.find(' ');
        r.code = sp == std::string::npos ? 0 : atoi(buf.c_str() + sp + 1);
        if (r.code >= 300 && r.code < 400 && r.code != 304) {
            std::string loc = header_value(r.head, "location");
            close(s);
            if (loc.empty()) { err = "redirect without an address"; return false; }
            if (loc.compare(0, 2, "//") == 0) loc = "http:" + loc;
            else if (loc[0] == '/') loc = "http://" + u.host + (u.port != 80 ? ":" + std::to_string(u.port) : "") + loc;
            else if (lower(loc.substr(0, 7)) != "http://" && lower(loc.substr(0, 8)) != "https://") {
                std::string base = u.path.substr(0, u.path.rfind('/') + 1);
                loc = "http://" + u.host + (u.port != 80 ? ":" + std::to_string(u.port) : "") + base + loc;
            }
            if (lower(loc.substr(0, 8)) == "https://") { err = "the server redirects to https://, which is not supported yet"; return false; }
            url = loc;
            continue;
        }
        r.sock = s;
        r.url = url;
        r.path = u.path;
        return true;
    }
    err = "too many redirects";
    return false;
}

static bool looks_like_torrent_start(const std::string& b)
{
    // .torrent начинается со словаря: d<длина>:<ключ> (обычно d8:announce, d13:announce-list, d4:info, d7:comment)
    if (b.size() < 3 || b[0] != 'd') return false;
    size_t i = 1;
    while (i < b.size() && i < 8 && b[i] >= '0' && b[i] <= '9') i++;
    return i > 1 && i < b.size() && b[i] == ':';
}

bool direct_probe(const std::string& url, size_t maxTorrent, UrlProbe& out)
{
    Resp r;
    if (!http_open(url, 0, r, out.error, NULL)) return false;
    out.finalUrl = r.url;
    if (r.code != 200) {
        close(r.sock);
        out.error = "the server answered " + std::to_string(r.code) + (r.code == 404 ? " (file not found)" : "");
        return false;
    }
    std::string cl = header_value(r.head, "content-length");
    if (!cl.empty()) { out.size = strtoull(cl.c_str(), NULL, 10); out.sizeKnown = true; }
    out.name = name_from(r.head, r.path);
    std::string ct = lower(header_value(r.head, "content-type"));
    bool hint = ct.find("bittorrent") != std::string::npos || (out.name.size() > 8 && lower(out.name.substr(out.name.size() - 8)) == ".torrent");
    // Если по первым байтам похоже на .torrent (или сервер так говорит) и он не слишком большой, читаем целиком.
    std::string body = r.rest;
    time_t deadline = time(NULL) + 20;
    char tmp[4096];
    while (body.size() < 16 && time(NULL) < deadline) {
        if (wait_readable(r.sock, 500) <= 0) continue;
        ssize_t n = recv(r.sock, tmp, sizeof(tmp), 0);
        if (n <= 0) break;
        body.append(tmp, (size_t)n);
    }
    bool maybe = (hint || looks_like_torrent_start(body)) && (!out.sizeKnown || out.size <= maxTorrent);
    if (maybe && looks_like_torrent_start(body)) {
        bool eof = false;
        while (!eof && body.size() <= maxTorrent && (!out.sizeKnown || body.size() < out.size)) {
            if (time(NULL) > deadline) { close(r.sock); out.error = "the server is too slow"; return false; }
            int w = wait_readable(r.sock, 500);
            if (w < 0) break;
            if (w == 0) continue;
            ssize_t n = recv(r.sock, tmp, sizeof(tmp), 0);
            if (n <= 0) { eof = true; break; }
            body.append(tmp, (size_t)n);
        }
        if (body.size() <= maxTorrent && body.find("4:info") != std::string::npos) {
            close(r.sock);
            out.torrent = true;
            out.body.swap(body);
            if (out.name.size() < 9 || lower(out.name.substr(out.name.size() - 8)) != ".torrent") out.name += ".torrent";
            return true;
        }
    }
    close(r.sock);
    if (hint && !out.torrent && ct.find("bittorrent") != std::string::npos) { out.error = "the link points to a damaged .torrent"; return false; }
    return true;
}

// ---------------------------------------------------------------- список загрузок

struct DItem {
    std::string id, url, name, root;
    uint64_t size = 0;
    bool sizeKnown = false;
    bool complete = false, paused = false;
    bool failed = false, permanent = false;   // ошибка; permanent: сама не повторяется (нужна кнопка «продолжить»)
    std::string err;
    time_t retryAt = 0;
    int fails = 0;
    bool offline = false;
    time_t offlineChecked = 0;
    // поток
    bool thRunning = false;
    pthread_t th;
    std::atomic<bool> stop{false};
    std::atomic<int> thState{0};              // 1 соединение, 2 приём, 3 готово, 4 ошибка, 5 остановлен
    std::atomic<uint64_t> done{0};
    std::string thErr;                        // пишет поток до thState = 4 (читаем после)
    bool thPermanent = false;
    uint64_t thSize = 0;
    bool thSizeKnown = false;
    // команды, ждущие остановки потока
    bool delPending = false, delFiles = false;
    // скорость
    uint64_t lastDone = 0;
    uint64_t startDone = 0;                   // сколько было на диске при запуске потока (для счётчика неудач)
    time_t lastT = 0;
    double speed = 0;                         // байт/с, сглаженная
    std::vector<std::pair<time_t, uint64_t> > rate;

    std::string partPath() const { return root + "/downloads/" + name + ".part"; }
    std::string finalPath() const { return root + "/downloads/" + name; }
};

static std::vector<DItem*> g_d;
static pthread_mutex_t g_dmu = PTHREAD_MUTEX_INITIALIZER;    // защищает g_urls (читают HTTP-потоки)
static std::set<std::string> g_urls;                         // ссылки незавершённых загрузок
static bool g_dirty = false;

static void sync_urls()
{
    std::set<std::string> u;
    for (size_t i = 0; i < g_d.size(); i++) if (!g_d[i]->complete) u.insert(g_d[i]->url);
    pthread_mutex_lock(&g_dmu);
    g_urls.swap(u);
    pthread_mutex_unlock(&g_dmu);
}

bool direct_has_url(const std::string& url)
{
    pthread_mutex_lock(&g_dmu);
    bool r = g_urls.count(url) > 0;
    pthread_mutex_unlock(&g_dmu);
    return r;
}

static uint64_t file_size(const std::string& p)
{
    struct stat sb;
    return stat(p.c_str(), &sb) == 0 ? (uint64_t)sb.st_size : 0;
}

static bool exists(const std::string& p) { struct stat sb; return stat(p.c_str(), &sb) == 0; }

static void save_list()
{
    std::string tmp = std::string(DIRECT_FILE) + ".tmp";
    FILE* f = fopen(tmp.c_str(), "w");
    if (!f) { logf_("direct: cannot write %s", tmp.c_str()); return; }
    for (size_t i = 0; i < g_d.size(); i++) {
        const DItem& d = *g_d[i];
        if (d.delPending) continue;
        fprintf(f, "%s\t%s\t%s\t%s\t%llu\t%d\t%d\t%d\n", d.id.c_str(), d.url.c_str(), d.name.c_str(), d.root.c_str(),
                (unsigned long long)d.size, d.sizeKnown ? 1 : 0, d.complete ? 1 : 0, d.paused ? 1 : 0);
    }
    fclose(f);
    rename(tmp.c_str(), DIRECT_FILE);
    g_dirty = false;
}

void direct_load()
{
    FILE* f = fopen(DIRECT_FILE, "r");
    if (!f) return;
    char line[4096];
    while (fgets(line, sizeof(line), f)) {
        std::vector<std::string> p;
        std::string s = line;
        while (!s.empty() && (s[s.size() - 1] == '\n' || s[s.size() - 1] == '\r')) s.erase(s.size() - 1);
        size_t a = 0;
        for (;;) { size_t t = s.find('\t', a); p.push_back(s.substr(a, t == std::string::npos ? std::string::npos : t - a)); if (t == std::string::npos) break; a = t + 1; }
        if (p.size() < 8 || p[0].size() != 40) continue;
        DItem* d = new DItem;
        d->id = p[0]; d->url = p[1]; d->name = p[2]; d->root = p[3];
        d->size = strtoull(p[4].c_str(), NULL, 10); d->sizeKnown = p[5] == "1"; d->complete = p[6] == "1"; d->paused = p[7] == "1";
        d->done = d->complete ? file_size(d->finalPath()) : file_size(d->partPath());
        g_d.push_back(d);
    }
    fclose(f);
    sync_urls();
    logf_("direct: %d link downloads in the list", (int)g_d.size());
}

void direct_add(const std::string& root, const std::string& url, const std::string& name0, uint64_t size, bool sizeKnown, bool start)
{
    if (direct_has_url(url)) { logf_("direct: already in the list: %s", url.c_str()); return; }
    std::string name = safe_name(name0);
    // Имя не должно совпадать с файлом на диске или с другой загрузкой в той же папке
    std::string base = name, ext;
    size_t dot = name.rfind('.');
    if (dot != std::string::npos && dot > 0) { base = name.substr(0, dot); ext = name.substr(dot); }
    for (int n = 2; n < 1000; n++) {
        bool busy = exists(root + "/downloads/" + name) || exists(root + "/downloads/" + name + ".part");
        for (size_t i = 0; i < g_d.size() && !busy; i++) if (g_d[i]->root == root && g_d[i]->name == name) busy = true;
        if (!busy) break;
        name = base + " (" + std::to_string(n) + ")" + ext;
    }
    mkdir((root + "/downloads").c_str(), 0777);
    DItem* d = new DItem;
    unsigned char h[20];
    std::string seed = url + "\n" + root + "\n" + name + "\n" + std::to_string((long long)time(NULL)) + std::to_string((unsigned long)g_d.size());
    sha1((const unsigned char*)seed.data(), seed.size(), h);
    static const char* hx = "0123456789abcdef";
    for (int i = 0; i < 20; i++) { d->id += hx[h[i] >> 4]; d->id += hx[h[i] & 15]; }
    d->url = url; d->name = name; d->root = root; d->size = size; d->sizeKnown = sizeKnown; d->paused = !start;
    g_d.push_back(d);
    sync_urls();
    save_list();
    logf_("direct: added %s -> %s/downloads/%s (%s%llu bytes)%s", url.c_str(), root.c_str(), name.c_str(), sizeKnown ? "" : "size unknown, ",
          (unsigned long long)size, start ? "" : " [paused]");
}

// ---------------------------------------------------------------- поток загрузки

static void* direct_thread(void* arg)
{
    DItem* d = (DItem*)arg;
    std::string part = d->partPath();
    int fd = open(part.c_str(), O_WRONLY | O_CREAT, 0666);
    if (fd < 0) { d->thErr = "cannot create the file on the drive (errno " + std::to_string(errno) + ")"; d->thState = 4; return NULL; }
    uint64_t from = (uint64_t)lseek(fd, 0, SEEK_END);
    d->done = from;
    if (d->sizeKnown && d->size > 0 && from == d->size) { close(fd); d->thSize = d->size; d->thSizeKnown = true; d->thState = 3; return NULL; }
    if (d->sizeKnown && from > d->size) { (void)!ftruncate(fd, 0); from = 0; d->done = 0; }

    d->thState = 1;
    Resp r;
    std::string err;
    if (!http_open(d->url, from, r, err, &d->stop)) {
        close(fd);
        d->thErr = err;
        d->thPermanent = err.find("https") != std::string::npos || err.find("link") != std::string::npos;
        d->thState = d->stop ? 5 : 4;
        return NULL;
    }
    uint64_t total = 0;
    bool totalKnown = false;
    std::string cl = header_value(r.head, "content-length");
    if (r.code == 206) {
        std::string cr = header_value(r.head, "content-range");       // bytes 100-199/200
        size_t sp = cr.find(' '), dash = cr.find('-'), sl = cr.find('/');
        uint64_t a = sp != std::string::npos ? strtoull(cr.c_str() + sp + 1, NULL, 10) : 0;
        if (a != from) {                                                  // сервер прислал не тот кусок: начнём с начала
            close(r.sock); (void)!ftruncate(fd, 0); close(fd);
            d->done = 0;
            d->thErr = "the server sent a wrong part of the file, starting over";
            d->thState = 4;
            return NULL;
        }
        if (sl != std::string::npos && cr[sl + 1] != '*') { total = strtoull(cr.c_str() + sl + 1, NULL, 10); totalKnown = true; }
        else if (!cl.empty()) { total = from + strtoull(cl.c_str(), NULL, 10); totalKnown = true; }
        (void)dash;
    } else if (r.code == 200) {
        if (from > 0) {                                                   // докачку сервер не умеет: начинаем заново
            logf_("direct: %s: the server does not support resuming, starting from the beginning", d->name.c_str());
            (void)!ftruncate(fd, 0);
            lseek(fd, 0, SEEK_SET);
            from = 0;
            d->done = 0;
        }
        if (!cl.empty()) { total = strtoull(cl.c_str(), NULL, 10); totalKnown = true; }
    } else if (r.code == 416 && from > 0 && d->sizeKnown && from >= d->size) {
        close(r.sock); close(fd);
        d->thSize = from; d->thSizeKnown = true; d->thState = 3;
        return NULL;
    } else {
        close(r.sock); close(fd);
        d->thErr = "the server answered " + std::to_string(r.code) + (r.code == 404 ? " (file not found)" : "");
        d->thPermanent = r.code >= 400 && r.code < 500;
        d->thState = 4;
        return NULL;
    }
    d->thSize = total;
    d->thSizeKnown = totalKnown;
    d->thState = 2;

    uint64_t pos = from;
    bool ok = true, eof = false;
    std::string fail;
    char* buf = (char*)malloc(RECV_BUF);
    if (!buf) { close(r.sock); close(fd); d->thErr = "out of memory"; d->thState = 4; return NULL; }
    size_t pre = r.rest.size();
    if (pre > 0) {
        if (totalKnown && pos + pre > total) pre = (size_t)(total - pos);
        if (write(fd, r.rest.data(), pre) != (ssize_t)pre) { ok = false; fail = "write to the drive failed (errno " + std::to_string(errno) + ")"; }
        pos += pre;
        d->done = pos;
    }
    std::string().swap(r.rest);
    time_t lastData = time(NULL);
    while (ok && !eof && !d->stop) {
        if (totalKnown && pos >= total) break;
        int w = wait_readable(r.sock, 1000);
        if (w < 0) { ok = false; fail = "receive failed"; break; }
        if (w == 0) {
            if (time(NULL) - lastData > IO_TIMEOUT_S) { ok = false; fail = "no data from the server for " + std::to_string(IO_TIMEOUT_S) + " s"; }
            continue;
        }
        ssize_t n = recv(r.sock, buf, RECV_BUF, 0);
        if (n < 0) { if (errno == EINTR || errno == EAGAIN) continue; ok = false; fail = "connection lost"; break; }
        if (n == 0) { eof = true; break; }
        lastData = time(NULL);
        size_t len = (size_t)n;
        if (totalKnown && pos + len > total) len = (size_t)(total - pos);
        size_t off = 0;
        while (off < len) {
            ssize_t wr = write(fd, buf + off, len - off);
            if (wr <= 0) { ok = false; fail = "write to the drive failed (errno " + std::to_string(errno) + ")"; break; }
            off += (size_t)wr;
        }
        pos += off;
        d->done = pos;
    }
    free(buf);
    close(r.sock);
    if (close(fd) != 0 && ok) { ok = false; fail = "write to the drive failed (errno " + std::to_string(errno) + ")"; }
    if (d->stop) { d->thState = 5; return NULL; }
    if (ok && totalKnown && pos < total) { ok = false; fail = "the connection closed before the end of the file"; }
    if (!ok) { d->thErr = fail; d->thState = 4; return NULL; }
    if (!totalKnown) { d->thSize = pos; d->thSizeKnown = true; }
    d->thState = 3;
    return NULL;
}

static bool start_thread(DItem* d)
{
    d->stop = false;
    d->thState = 1;
    d->thErr.clear();
    d->thPermanent = false;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 256 * 1024);
    int rc = pthread_create(&d->th, &at, direct_thread, d);
    pthread_attr_destroy(&at);
    if (rc != 0) { d->thState = 0; logf_("direct: cannot start a thread (%d)", rc); return false; }
    d->thRunning = true;
    d->lastDone = d->done;
    d->startDone = d->done;
    d->lastT = time(NULL);
    d->rate.clear();
    logf_("direct: start %s (from %llu bytes)", d->name.c_str(), (unsigned long long)d->done.load());
    return true;
}

// ---------------------------------------------------------------- команды и главный цикл

static DItem* find_id(const std::string& id)
{
    for (size_t i = 0; i < g_d.size(); i++) if (g_d[i]->id == id) return g_d[i];
    return NULL;
}

static void remove_now(DItem* d)
{
    if (d->delFiles) {
        unlink(d->partPath().c_str());
        if (d->complete) unlink(d->finalPath().c_str());
    }
    logf_("direct: removed %s%s", d->name.c_str(), d->delFiles ? " (with the file)" : "");
    for (size_t i = 0; i < g_d.size(); i++) if (g_d[i] == d) { g_d.erase(g_d.begin() + i); break; }
    delete d;
    sync_urls();
    save_list();
}

bool direct_command(const std::string& cmd, const std::string& id)
{
    DItem* d = find_id(id);
    if (!d) return false;
    if (cmd == "pause") {
        if (!d->complete) { d->paused = true; if (d->thRunning) d->stop = true; g_dirty = true; logf_("direct: pause %s", d->name.c_str()); }
    } else if (cmd == "resume") {
        if (!d->complete) { d->paused = false; d->failed = false; d->permanent = false; d->retryAt = 0; d->fails = 0; d->err.clear(); g_dirty = true; logf_("direct: resume %s", d->name.c_str()); }
    } else if (cmd == "delete" || cmd == "delete_files") {
        d->delPending = true;
        d->delFiles = (cmd == "delete_files");
        if (d->thRunning) d->stop = true; else remove_now(d);
    }
    return true;
}

void direct_all(bool pause)
{
    for (size_t i = 0; i < g_d.size(); i++) {
        DItem* d = g_d[i];
        if (d->complete || d->delPending) continue;
        if (pause) { d->paused = true; if (d->thRunning) d->stop = true; }
        else { d->paused = false; d->failed = false; d->permanent = false; d->retryAt = 0; d->fails = 0; d->err.clear(); }
    }
    g_dirty = true;
}

static void finish_thread(DItem* d, time_t now, std::vector<std::string>& finished)
{
    pthread_join(d->th, NULL);
    d->thRunning = false;
    int st = d->thState;
    d->thState = 0;
    d->speed = 0;
    if (d->delPending) { remove_now(d); return; }
    if (st == 3) {
        d->size = d->thSize;
        d->sizeKnown = true;
        if (rename(d->partPath().c_str(), d->finalPath().c_str()) != 0) {
            d->failed = true; d->permanent = true;
            d->err = "cannot rename the finished file (errno " + std::to_string(errno) + ")";
            logf_("direct: %s: %s", d->name.c_str(), d->err.c_str());
        } else {
            d->complete = true;
            d->done = d->size;
            d->failed = false; d->err.clear(); d->fails = 0;
            logf_("direct: finished %s (%llu bytes)", d->name.c_str(), (unsigned long long)d->size);
            finished.push_back(d->name);
        }
        sync_urls();
        save_list();
    } else if (st == 4) {
        if (d->thSizeKnown && d->thSize) { d->size = d->thSize; d->sizeKnown = true; }
        d->failed = true;
        d->permanent = d->thPermanent;
        d->err = d->thErr;
        if (d->done > d->startDone) d->fails = 0;              // что-то успело скачаться: повтор снова через короткую паузу
        d->fails++;
        int delay = DIRECT_RETRY_S << (d->fails > 5 ? 5 : d->fails - 1);
        if (delay > 300) delay = 300;
        d->retryAt = d->permanent ? 0 : now + delay;
        logf_("direct: %s: %s%s", d->name.c_str(), d->err.c_str(), d->permanent ? " (will not retry by itself)" : (" (retry in " + std::to_string(delay) + " s)").c_str());
        g_dirty = true;
    } else {
        if (d->thSizeKnown && d->thSize) { d->size = d->thSize; d->sizeKnown = true; }
        logf_("direct: stopped %s at %llu bytes", d->name.c_str(), (unsigned long long)d->done.load());
        g_dirty = true;
    }
}

void direct_tick(time_t now, std::vector<std::string>& finished)
{
    int running = 0;
    for (size_t i = 0; i < g_d.size(); i++) {
        DItem* d = g_d[i];
        if (d->thRunning) {
            int st = d->thState;
            if (st >= 3) { finish_thread(d, now, finished); i = (size_t)-1; running = 0; continue; }   // список мог измениться: заново
            running++;
        }
    }
    for (size_t i = 0; i < g_d.size(); i++) {
        DItem* d = g_d[i];
        if (d->thRunning) {
            if (d->thState == 2 && d->thSizeKnown && (!d->sizeKnown || d->size != d->thSize)) { d->size = d->thSize; d->sizeKnown = true; g_dirty = true; }
            if (now != d->lastT) {
                uint64_t cur = d->done;
                double inst = (double)(cur - (cur >= d->lastDone ? d->lastDone : cur)) / (double)(now - d->lastT);
                d->speed = d->speed * 0.6 + inst * 0.4;
                d->lastDone = cur;
                d->lastT = now;
                d->rate.push_back(std::make_pair(now, cur));
                while (d->rate.size() > 2 && now - d->rate.front().first > 60) d->rate.erase(d->rate.begin());
            }
            continue;
        }
        if (d->complete || d->paused || d->delPending) continue;
        if (now - d->offlineChecked >= 5 || d->offlineChecked == 0) {
            d->offlineChecked = now;
            bool was = d->offline;
            d->offline = !exists(d->root);
            if (was != d->offline) logf_("direct: %s: drive %s", d->name.c_str(), d->offline ? "gone" : "back");
        }
        if (d->offline) continue;
        if (d->failed && (d->permanent || now < d->retryAt)) continue;
        if (running >= DIRECT_MAX) continue;
        if (start_thread(d)) running++;
    }
    if (g_dirty) save_list();
}

bool direct_running()
{
    for (size_t i = 0; i < g_d.size(); i++) if (g_d[i]->thRunning) return true;
    return false;
}

static std::string jesc(const std::string& s)
{
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
        else if (c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
        else o += (char)c;
    }
    return o;
}

std::string direct_items_json(int& speedKB)
{
    std::string j;
    time_t now = time(NULL);
    for (size_t i = 0; i < g_d.size(); i++) {
        DItem* d = g_d[i];
        if (d->delPending) continue;
        const char* st;
        bool receiving = d->thRunning && d->thState == 2;
        if (d->complete) st = "complete";
        else if (d->thRunning) st = "downloading";
        else if (d->offline) st = "offline";
        else if (d->paused) st = "paused";
        else if (d->failed) st = d->permanent ? "error" : "waiting";
        else st = "queued";
        uint64_t done = d->complete ? d->size : d->done.load();
        uint64_t size = d->sizeKnown ? d->size : 0;
        int pct = size ? (int)(done * 100 / size) : 0;
        if (pct > 100) pct = 100;
        int kb = receiving ? (int)(d->speed / 1024.0 + 0.5) : 0;
        speedKB += kb;
        long long eta = d->complete ? 0 : -1;
        if (receiving && size > done && d->rate.size() >= 2) {
            double dt = (double)(d->rate.back().first - d->rate.front().first);
            double bps = dt >= 10 ? (double)(d->rate.back().second - d->rate.front().second) / dt : 0;
            if (bps >= 1024) { double s = (double)(size - done) / bps; eta = s > 8640000.0 ? 8640000LL : (long long)s; }
        }
        (void)now;
        char buf[400];
        snprintf(buf, sizeof(buf),
                 "{\"hash\":\"%s\",\"kind\":\"http\",\"status\":\"%s\",\"size\":%llu,\"size_known\":%s,\"done\":%llu,\"pct\":%d,"
                 "\"speed_kb\":%d,\"eta_s\":%lld,\"peers\":0,\"paused\":%s,\"mem_paused\":false,\"low_space\":false,\"nfiles\":1,\"nskip\":0,"
                 "\"seeding\":false,\"up_kb\":0,\"root\":\"",
                 d->id.c_str(), st, (unsigned long long)size, d->sizeKnown ? "true" : "false", (unsigned long long)done, pct, kb, eta,
                 d->paused ? "true" : "false");
        if (!j.empty()) j += ",";
        j += buf;
        j += jesc(d->root) + "\",\"title\":\"" + jesc(d->name) + "\",\"url\":\"" + jesc(d->url) + "\",\"error\":\"" + jesc(d->failed ? d->err : "") + "\"}";
    }
    return j;
}

void direct_shutdown()
{
    for (size_t i = 0; i < g_d.size(); i++) if (g_d[i]->thRunning) g_d[i]->stop = true;
    for (int w = 0; w < 60; w++) {                           // поток замечает остановку не позже чем через секунду
        bool any = false;
        for (size_t i = 0; i < g_d.size(); i++) if (g_d[i]->thRunning && g_d[i]->thState < 3) any = true;
        if (!any) break;
        usleep(100000);
    }
    for (size_t i = 0; i < g_d.size(); i++) {
        DItem* d = g_d[i];
        if (!d->thRunning) continue;
        if (d->thState >= 3) { pthread_join(d->th, NULL); d->thRunning = false; }
        else pthread_detach(d->th);                          // завис на диске: не ждём (программа всё равно завершается)
    }
    save_list();
    logf_("direct: stopped");
}
