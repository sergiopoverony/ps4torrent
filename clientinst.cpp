#include "clientinst.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <atomic>
#include <string>

#define CLIENT_TITLE_ID   "BREW00088"
#define CLIENT_REPO       "sergiopoverony/ps4torrent_client"
#define CLIENT_API_URL    "https://api.github.com/repos/" CLIENT_REPO "/releases/latest"
#define PKG_DIR           "/data/pkg"
#define PKG_FILE          PKG_DIR "/ps4torrent_client.pkg"
#define PKG_URL_NAME      "ps4torrent_client.pkg"
#define TAG_FILE          PKG_DIR "/installed_tag.txt"
#define AUTO_MARK_FILE    PKG_DIR "/auto_install_tried.txt"
#define CHECK_EVERY_S     (6 * 3600)

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#ifdef HOST_TEST
// На ПК консольных библиотек нет: модуль выключен.
void clientinst_start(int) {}
std::string clientinst_json() { return "{\"state\":\"off\"}"; }
void clientinst_request_install() {}
int clientinst_serve(int, const std::string&, const std::string&, bool) { return 0; }
#else

extern "C" int sceKernelLoadStartModule(const char* name, size_t argc, const void* argv, unsigned flags, void* opt, int* res);
extern "C" int sceKernelDlsym(int handle, const char* symbol, void** addr);

// ---- состояние ----
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static std::string g_state = "idle";        // idle check download install done error
static std::string g_msg;                   // последняя ошибка / пояснение (по-английски)
static std::string g_latest;                // версия последнего релиза ("" = не знаем)
static std::string g_url;                   // ссылка на .pkg в релизе
static std::string g_tag;                   // тег последнего релиза (например v1.6.2.0)
static int g_pct = 0;
static std::atomic<int> g_req(0);
static std::atomic<int> g_started(0);
static int g_port = 8787;

static void set_state(const char* s, const char* msg = "", int pct = 0)
{
    pthread_mutex_lock(&g_mu);
    g_state = s; g_msg = msg; g_pct = pct;
    pthread_mutex_unlock(&g_mu);
}

// ---- загрузка системных библиотек и поиск функций по имени ----
static int loadmod(const char* path)
{
    int h = sceKernelLoadStartModule(path, 0, NULL, 0, NULL, NULL);
    logf_("client: module %s -> 0x%x", path, (unsigned)h);
    return h;
}
template <class F> static bool sym(int h, const char* name, F& f)
{
    void* a = NULL;
    if (h < 0 || sceKernelDlsym(h, name, &a) != 0 || !a) { logf_("client: function %s not found", name); return false; }
    f = (F)a;
    return true;
}

// ---- HTTPS-запрос через системную библиотеку sceHttp ----
typedef int (*fn_i_v)(void);
typedef int (*fn_pool)(const char*, int, int);
typedef int (*fn_ssl_init)(size_t);
typedef int (*fn_http_init)(int, int, size_t);
typedef int (*fn_tmpl)(int, const char*, int, int);
typedef int (*fn_req_url)(int, int, const char*, unsigned long long);
typedef int (*fn_set2)(int, int);
typedef int (*fn_setu)(int, unsigned);
typedef int (*fn_send)(int, const void*, size_t);
typedef int (*fn_status)(int, int*);
typedef int (*fn_clen)(int, int*, unsigned long long*);
typedef int (*fn_read)(int, void*, size_t);
typedef int (*fn_del)(int);

static struct {
    bool ok;
    int ctx;
    fn_tmpl createTemplate; fn_req_url createReq; fn_set2 autoRedirect; fn_setu connTo, recvTo, sendTo;
    fn_send sendReq; fn_status getStatus; fn_clen getLen; fn_read readData; fn_del delReq, delTmpl;
} H;

static bool http_ready()
{
    if (H.ok) return true;
    int hn = loadmod("/system/common/lib/libSceNet.sprx");
    int hs = loadmod("/system/common/lib/libSceSsl.sprx");
    int hh = loadmod("/system/common/lib/libSceHttp.sprx");
    fn_i_v netInit = NULL; fn_pool poolCreate = NULL; fn_ssl_init sslInit = NULL; fn_http_init httpInit = NULL;
    if (!sym(hn, "sceNetInit", netInit) || !sym(hn, "sceNetPoolCreate", poolCreate) || !sym(hs, "sceSslInit", sslInit)
        || !sym(hh, "sceHttpInit", httpInit) || !sym(hh, "sceHttpCreateTemplate", H.createTemplate)
        || !sym(hh, "sceHttpCreateRequestWithURL", H.createReq) || !sym(hh, "sceHttpSetAutoRedirect", H.autoRedirect)
        || !sym(hh, "sceHttpSetConnectTimeOut", H.connTo) || !sym(hh, "sceHttpSetRecvTimeOut", H.recvTo)
        || !sym(hh, "sceHttpSetSendTimeOut", H.sendTo) || !sym(hh, "sceHttpSendRequest", H.sendReq)
        || !sym(hh, "sceHttpGetStatusCode", H.getStatus) || !sym(hh, "sceHttpGetResponseContentLength", H.getLen)
        || !sym(hh, "sceHttpReadData", H.readData) || !sym(hh, "sceHttpDeleteRequest", H.delReq)
        || !sym(hh, "sceHttpDeleteTemplate", H.delTmpl))
        return false;
    int r = netInit();
    logf_("client: sceNetInit -> 0x%x (an error here is normal, the network is already initialised)", (unsigned)r);
    int mem = poolCreate("ps4torrentd_http", 64 * 1024, 0);
    int ssl = sslInit(256 * 1024);
    int ctx = httpInit(mem, ssl, 256 * 1024);
    logf_("client: net pool 0x%x, ssl 0x%x, http 0x%x", (unsigned)mem, (unsigned)ssl, (unsigned)ctx);
    if (mem < 0 || ssl < 0 || ctx < 0) return false;
    H.ctx = ctx;
    H.ok = true;
    return true;
}

// Скачивает url. В память (если file == NULL, не больше maxMem байт) или в файл. Возвращает HTTP-код (200) или 0 при ошибке.
static int http_get(const std::string& url, std::string* mem, size_t maxMem, const char* file, bool progress)
{
    if (!http_ready()) { set_state("error", "cannot load system HTTP library"); return 0; }
    int tmpl = H.createTemplate(H.ctx, "ps4torrentd", 2, 1);
    if (tmpl < 0) { logf_("client: sceHttpCreateTemplate 0x%x", (unsigned)tmpl); return 0; }
    int req = H.createReq(tmpl, 0, url.c_str(), 0);
    if (req < 0) { logf_("client: sceHttpCreateRequestWithURL 0x%x", (unsigned)req); H.delTmpl(tmpl); return 0; }
    H.autoRedirect(req, 1);
    H.connTo(req, 15u * 1000000u); H.sendTo(req, 15u * 1000000u); H.recvTo(req, 30u * 1000000u);
    int code = 0;
    int r = H.sendReq(req, NULL, 0);
    if (r < 0) {
        logf_("client: request to %s failed: 0x%x (0x80431xxx = TLS/certificate problem, 0x80436xxx = network)", url.c_str(), (unsigned)r);
        char b[48]; snprintf(b, sizeof(b), "request failed 0x%x", (unsigned)r);
        set_state("error", b);
        H.delReq(req); H.delTmpl(tmpl);
        return 0;
    }
    H.getStatus(req, &code);
    unsigned long long total = 0; int lr = 0;
    if (H.getLen(req, &lr, &total) != 0 || lr != 0) total = 0;
    logf_("client: %s -> HTTP %d, %llu bytes announced", url.c_str(), code, total);
    if (code != 200) { char b[48]; snprintf(b, sizeof(b), "server answered HTTP %d", code); set_state("error", b); H.delReq(req); H.delTmpl(tmpl); return 0; }
    int fd = -1;
    if (file) { fd = open(file, O_WRONLY | O_CREAT | O_TRUNC, 0644); if (fd < 0) { set_state("error", "cannot write the file in /data/pkg"); H.delReq(req); H.delTmpl(tmpl); return 0; } }
    char* buf = (char*)malloc(32768);
    unsigned long long got = 0;
    bool ok = buf != NULL;
    while (ok) {
        int n = H.readData(req, buf, 32768);
        if (n < 0) { logf_("client: read error 0x%x", (unsigned)n); ok = false; break; }
        if (n == 0) break;
        got += (unsigned long long)n;
        if (file) { if (write(fd, buf, (size_t)n) != n) { set_state("error", "disk write failed"); ok = false; break; } }
        else if (mem) { if (mem->size() + (size_t)n > maxMem) { ok = false; break; } mem->append(buf, (size_t)n); }
        if (progress && total) {
            pthread_mutex_lock(&g_mu); g_pct = (int)(got * 100 / total); pthread_mutex_unlock(&g_mu);
        }
    }
    free(buf);
    if (fd >= 0) close(fd);
    H.delReq(req); H.delTmpl(tmpl);
    if (ok && total && got != total) ok = false;
    if (!ok) { if (file) unlink(file); pthread_mutex_lock(&g_mu); if (g_state != "error") { g_state = "error"; g_msg = "download interrupted"; } pthread_mutex_unlock(&g_mu); return 0; }
    logf_("client: received %llu bytes", got);
    return code;
}

// ---- разбор ответа GitHub ----
static std::string json_str(const std::string& j, const char* key, size_t from = 0, size_t* endPos = NULL)
{
    std::string k = std::string("\"") + key + "\"";
    size_t p = j.find(k, from);
    if (p == std::string::npos) return "";
    p = j.find(':', p + k.size());
    if (p == std::string::npos) return "";
    p = j.find('"', p);
    if (p == std::string::npos) return "";
    size_t e = j.find('"', p + 1);
    if (e == std::string::npos) return "";
    if (endPos) *endPos = e;
    return j.substr(p + 1, e - p - 1);
}

static void parse_ver(const std::string& s, long v[5])
{
    for (int i = 0; i < 5; i++) v[i] = 0;
    int idx = 0; long cur = 0; bool any = false;
    for (size_t i = 0; i <= s.size() && idx < 5; i++) {
        char ch = i < s.size() ? s[i] : '.';
        if (ch >= '0' && ch <= '9') { cur = cur * 10 + (ch - '0'); any = true; }
        else if (ch == '.') { if (any) v[idx++] = cur; cur = 0; any = false; }
        else if (!any && (ch == 'v' || ch == 'V')) continue;
        else { if (any) v[idx++] = cur; break; }
    }
}
static int cmp_ver(const std::string& a, const std::string& b)
{
    long x[5], y[5]; parse_ver(a, x); parse_ver(b, y);
    for (int i = 0; i < 5; i++) { if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1; }
    return 0;
}
static std::string clean_ver(std::string s)
{
    while (!s.empty() && (s[0] == 'v' || s[0] == 'V')) s.erase(0, 1);
    return s;
}

// Узнаём последнюю версию в релизах GitHub.
static bool check_latest()
{
    std::string j;
    if (http_get(CLIENT_API_URL, &j, 256 * 1024, NULL, false) != 200) return false;
    std::string tag = json_str(j, "tag_name");
    std::string url;
    size_t pos = 0;
    for (int guard = 0; guard < 20; guard++) {
        size_t endp = 0;
        std::string u = json_str(j, "browser_download_url", pos, &endp);
        if (u.empty()) break;
        if (u.size() > 4 && u.compare(u.size() - 4, 4, ".pkg") == 0) { url = u; break; }
        pos = endp + 1;
    }
    if (tag.empty() || url.empty()) { set_state("error", "release has no .pkg file"); logf_("client: release answer without tag or .pkg"); return false; }
    pthread_mutex_lock(&g_mu);
    g_tag = tag; g_latest = clean_ver(tag); g_url = url;
    pthread_mutex_unlock(&g_mu);
    logf_("client: latest release %s, file %s", tag.c_str(), url.c_str());
    return true;
}

// ---- что установлено на консоли ----
static bool dir_exists(const char* p) { struct stat st; return stat(p, &st) == 0 && S_ISDIR(st.st_mode); }

static std::string sfo_version(const char* path)
{
    FILE* f = fopen(path, "rb");
    if (!f) return "";
    std::string d(65536, '\0');
    size_t n = fread(&d[0], 1, d.size(), f);
    fclose(f);
    d.resize(n);
    if (n < 20 || d.compare(0, 4, std::string("\0PSF", 4)) != 0) return "";
    unsigned kt = *(const unsigned*)&d[8], dt = *(const unsigned*)&d[12], cnt = *(const unsigned*)&d[16];
    std::string found;
    for (unsigned i = 0; i < cnt && 20 + (i + 1) * 16 <= n; i++) {
        const unsigned char* e = (const unsigned char*)&d[20 + i * 16];
        unsigned ko = e[0] | (e[1] << 8), len = e[4] | (e[5] << 8) | (e[6] << 16) | ((unsigned)e[7] << 24);
        unsigned off = e[12] | (e[13] << 8) | (e[14] << 16) | ((unsigned)e[15] << 24);
        if (kt + ko >= n || dt + off + len > n) continue;
        std::string key(&d[kt + ko]);
        if (key == "APP_VER" || (key == "VERSION" && found.empty())) found = std::string(&d[dt + off], strnlen(&d[dt + off], len));
    }
    return found;
}

// present = приложение есть на консоли; ver = версия из param.sfo, если удалось прочитать
static bool installed_info(std::string& ver)
{
    static const char* dirs[] = { "/user/app/" CLIENT_TITLE_ID, "/system_data/priv/appmeta/" CLIENT_TITLE_ID, "/user/appmeta/" CLIENT_TITLE_ID, NULL };
    static const char* sfos[] = { "/system_data/priv/appmeta/" CLIENT_TITLE_ID "/param.sfo", "/user/appmeta/" CLIENT_TITLE_ID "/param.sfo",
                                  "/user/app/" CLIENT_TITLE_ID "/sce_sys/param.sfo", NULL };
    bool present = false;
    for (int i = 0; dirs[i]; i++) if (dir_exists(dirs[i])) present = true;
    ver.clear();
    for (int i = 0; sfos[i] && ver.empty(); i++) ver = sfo_version(sfos[i]);
    return present;
}

static std::string read_small(const char* p)
{
    FILE* f = fopen(p, "rb"); if (!f) return "";
    char b[128]; size_t n = fread(b, 1, sizeof(b) - 1, f); fclose(f);
    b[n] = 0; std::string s = b;
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    return s;
}
static void write_small(const char* p, const std::string& s)
{
    FILE* f = fopen(p, "wb"); if (!f) return;
    fwrite(s.data(), 1, s.size(), f); fclose(f);
}

// ---- установка через BGFT ----
struct BgftInit { void* mem; size_t size; };
struct BgftParam {
    int user_id; int entitlement_type;
    const char* id; const char* content_url; const char* content_ex_url; const char* content_name;
    const char* icon_path; const char* sku_id;
    int option;
    const char* playgo_scenario_id; const char* release_date; const char* package_type; const char* package_sub_type;
    unsigned long long package_size;
};
typedef int (*fn_bgft_init)(BgftInit*);
typedef int (*fn_bgft_reg)(BgftParam*, int*);
typedef int (*fn_bgft_start)(int);
typedef int (*fn_user)(int*);

static bool bgft_install(const std::string& url, unsigned long long size)
{
    int ha = loadmod("/system/common/lib/libSceAppInstUtil.sprx");
    int hb = loadmod("/system/common/lib/libSceBgft.sprx");
    int hu = loadmod("/system/common/lib/libSceUserService.sprx");
    fn_i_v appInit = NULL; fn_bgft_init binit = NULL; fn_bgft_reg regDebug = NULL, regPlain = NULL; fn_bgft_start start = NULL; fn_user fg = NULL;
    if (sym(ha, "sceAppInstUtilInitialize", appInit)) logf_("client: sceAppInstUtilInitialize -> 0x%x", (unsigned)appInit());
    if (!sym(hb, "sceBgftServiceIntInit", binit) || !sym(hb, "sceBgftServiceIntDownloadStartTask", start)) {
        set_state("error", "BGFT library not available"); return false;
    }
    bool hasDebug = sym(hb, "sceBgftServiceIntDebugDownloadRegisterPkg", regDebug);
    bool hasPlain = sym(hb, "sceBgftServiceIntDownloadRegisterTask", regPlain);
    if (!hasDebug && !hasPlain) { set_state("error", "BGFT register function not found"); return false; }
    static BgftInit bi; static bool inited = false;
    if (!inited) {
        bi.size = 0x100000; bi.mem = calloc(1, bi.size);
        int r = bi.mem ? binit(&bi) : -1;
        logf_("client: sceBgftServiceIntInit -> 0x%x", (unsigned)r);
        inited = true;      // повторный вызов не нужен, даже если он вернул "уже инициализировано"
    }
    int uid = 0;
    if (sym(hu, "sceUserServiceGetForegroundUser", fg)) { int u = -1; if (fg(&u) == 0 && u > 0) uid = u; }
    BgftParam p; memset(&p, 0, sizeof(p));
    p.user_id = uid; p.entitlement_type = 5;
    p.id = ""; p.content_url = url.c_str(); p.content_ex_url = ""; p.content_name = "ps4torrent client";
    p.icon_path = ""; p.sku_id = ""; p.option = 16;
    p.playgo_scenario_id = "0"; p.release_date = ""; p.package_type = "PS4GD"; p.package_sub_type = "";
    p.package_size = size;
    int task = -1, r = -1;
    if (hasDebug) { r = regDebug(&p, &task); logf_("client: DebugDownloadRegisterPkg -> 0x%x task %d", (unsigned)r, task); }
    if ((r != 0 || task < 0) && hasPlain) { task = -1; r = regPlain(&p, &task); logf_("client: DownloadRegisterTask -> 0x%x task %d", (unsigned)r, task); }
    if (r != 0 || task < 0) { char b[64]; snprintf(b, sizeof(b), "install request refused 0x%x", (unsigned)r); set_state("error", b); return false; }
    r = start(task);
    logf_("client: DownloadStartTask(%d) -> 0x%x", task, (unsigned)r);
    if (r != 0) { char b[64]; snprintf(b, sizeof(b), "install start refused 0x%x", (unsigned)r); set_state("error", b); return false; }
    return true;
}

static void do_install()
{
    set_state("check");
    if (!check_latest()) return;
    std::string url, tag, latest;
    pthread_mutex_lock(&g_mu); url = g_url; tag = g_tag; latest = g_latest; pthread_mutex_unlock(&g_mu);

    mkdir("/data", 0777); mkdir(PKG_DIR, 0777);
    set_state("download", "", 0);
    std::string tmp = std::string(PKG_FILE) + ".part";
    if (http_get(url, NULL, 0, tmp.c_str(), true) != 200) return;
    struct stat st;
    if (stat(tmp.c_str(), &st) != 0 || st.st_size < 4096) { unlink(tmp.c_str()); set_state("error", "downloaded file is too small"); return; }
    FILE* f = fopen(tmp.c_str(), "rb"); char mg[4] = {0};
    if (f) { if (fread(mg, 1, 4, f) != 4) mg[0] = 0; fclose(f); }
    if (memcmp(mg, "\x7f""CNT", 4) != 0) { unlink(tmp.c_str()); set_state("error", "downloaded file is not a PS4 package"); return; }
    unlink(PKG_FILE);
    if (rename(tmp.c_str(), PKG_FILE) != 0) { set_state("error", "cannot save the package"); return; }

    set_state("install");
    char u[128]; snprintf(u, sizeof(u), "http://127.0.0.1:%d/clientpkg/" PKG_URL_NAME, g_port);
    if (!bgft_install(u, (unsigned long long)st.st_size)) return;
    write_small(TAG_FILE, tag);

    for (int i = 0; i < 100; i++) {          // ждём до ~5 минут, пока консоль поставит
        sleep(3);
        std::string v; bool pres = installed_info(v);
        if (pres && (v.empty() || cmp_ver(v, latest) >= 0)) { logf_("client: installed, version %s", v.empty() ? "?" : v.c_str()); set_state("done"); return; }
    }
    set_state("error", "install did not finish in 5 minutes (see the PS4 notifications)");
}

static void* worker(void*)
{
    sleep(25);                                // сеть к этому времени уже работает
    long nextCheck = 0;
    for (;;) {
        bool want = g_req.exchange(0) != 0;
        long now = (long)time(NULL);
        if (want) {
            do_install();
        } else if (now >= nextCheck) {
            nextCheck = now + CHECK_EVERY_S;
            set_state("check");
            if (check_latest()) {
                set_state("idle");
                std::string v;
                bool pres = installed_info(v);
                logf_("client: on the console: %s%s%s", pres ? "installed" : "not installed", v.empty() ? "" : ", version ", v.c_str());
                // Первая установка — один раз за всё время (метка пишется ДО попытки, чтобы не повторять сбой)
                if (!pres && read_small(AUTO_MARK_FILE).empty()) {
                    mkdir("/data", 0777); mkdir(PKG_DIR, 0777);
                    write_small(AUTO_MARK_FILE, "1");
                    logf_("client: not installed, trying once to install automatically");
                    do_install();
                }
            } else {
                nextCheck = now + 600;        // не получилось: повторим через 10 минут
            }
        }
        for (int i = 0; i < 5 && !g_req.load(); i++) sleep(1);
    }
    return NULL;
}

void clientinst_start(int httpPort)
{
    if (g_started.exchange(1)) return;
    g_port = httpPort;
    pthread_t th;
    if (pthread_create(&th, NULL, worker, NULL) == 0) pthread_detach(th);
    else logf_("client: cannot start the installer thread");
}

void clientinst_request_install()
{
    pthread_mutex_lock(&g_mu);
    bool busy = g_state == "check" || g_state == "download" || g_state == "install";
    pthread_mutex_unlock(&g_mu);
    if (!busy) g_req = 1;
}

static std::string esc(const std::string& s)
{
    std::string o;
    for (size_t i = 0; i < s.size(); i++) { char c = s[i]; if (c == '"' || c == '\\') o += '\\'; if ((unsigned char)c >= 32) o += c; }
    return o;
}

std::string clientinst_json()
{
    std::string st, msg, latest, tag; int pct;
    pthread_mutex_lock(&g_mu); st = g_state; msg = g_msg; latest = g_latest; tag = g_tag; pct = g_pct; pthread_mutex_unlock(&g_mu);
    std::string v; bool pres = installed_info(v);
    bool outdated = false;
    if (pres && !latest.empty() && !v.empty() && cmp_ver(v, latest) < 0 && read_small(TAG_FILE) != tag) outdated = true;
    char b[96];
    snprintf(b, sizeof(b), "\",\"pct\":%d,\"present\":%s,\"outdated\":%s,\"installed\":\"", pct, pres ? "true" : "false", outdated ? "true" : "false");
    return "{\"state\":\"" + st + b + esc(v) + "\",\"latest\":\"" + esc(latest) + "\",\"msg\":\"" + esc(msg) + "\"}";
}

// ---- отдача пакета консоли (BGFT качает его с 127.0.0.1) ----
int clientinst_serve(int c, const std::string& head, const std::string& path, bool headOnly)
{
    if (path != "/clientpkg/" PKG_URL_NAME) return 0;
    FILE* f = fopen(PKG_FILE, "rb");
    if (!f) {
        const char* nf = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        send(c, nf, strlen(nf), MSG_NOSIGNAL);
        return 1;
    }
    fseek(f, 0, SEEK_END); long long size = ftell(f);
    long long a = 0, b = size - 1; bool part = false;
    std::string low = head; for (size_t i = 0; i < low.size(); i++) low[i] = (char)tolower((unsigned char)low[i]);
    size_t rp = low.find("\nrange:");
    if (rp != std::string::npos) {
        size_t eq = low.find("bytes=", rp);
        if (eq != std::string::npos && eq < low.find('\n', rp + 1)) {
            const char* s = low.c_str() + eq + 6;
            char* e = NULL;
            long long x = strtoll(s, &e, 10);
            if (e && *e == '-') {
                if (e == s) { long long n = strtoll(e + 1, NULL, 10); a = size - n; if (a < 0) a = 0; }
                else {
                    a = x;
                    if (e[1] >= '0' && e[1] <= '9') b = strtoll(e + 1, NULL, 10);
                }
                if (b >= size) b = size - 1;
                if (a <= b && a < size) part = true; else { a = 0; b = size - 1; }
            }
        }
    }
    char h[320];
    int hl = part
        ? snprintf(h, sizeof(h), "HTTP/1.1 206 Partial Content\r\nContent-Type: application/octet-stream\r\nAccept-Ranges: bytes\r\nContent-Range: bytes %lld-%lld/%lld\r\nContent-Length: %lld\r\nConnection: close\r\n\r\n", a, b, size, b - a + 1)
        : snprintf(h, sizeof(h), "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nAccept-Ranges: bytes\r\nContent-Length: %lld\r\nConnection: close\r\n\r\n", size);
    send(c, h, (size_t)hl, MSG_NOSIGNAL);
    if (!headOnly) {
        char* buf = (char*)malloc(32768);
        if (buf) {
            fseek(f, (long)a, SEEK_SET);
            long long left = b - a + 1;
            while (left > 0) {
                size_t want = left > 32768 ? 32768 : (size_t)left;
                size_t n = fread(buf, 1, want, f);
                if (n == 0) break;
                size_t off = 0;
                while (off < n) { ssize_t w = send(c, buf + off, n - off, MSG_NOSIGNAL); if (w <= 0) { left = 0; break; } off += (size_t)w; }
                left -= (long long)n;
            }
            free(buf);
        }
    }
    fclose(f);
    return 1;
}
#endif
