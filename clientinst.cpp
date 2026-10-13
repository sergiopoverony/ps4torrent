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
#define CLIENT_PKG_NAME   "IV0000-BREW00088_00-PS4TC00000000000.pkg"
#define CLIENT_URL        "https://github.com/" CLIENT_REPO "/releases/latest/download/" CLIENT_PKG_NAME
#define AUTO_MARK_FILE    "/data/pkg/auto_install_tried.txt"

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#ifdef HOST_TEST
// На ПК консольных библиотек нет: модуль выключен.
void clientinst_start(int) {}
std::string clientinst_json() { return "{\"state\":\"off\"}"; }
void clientinst_request_install() {}
#else

extern "C" int sceKernelLoadStartModule(const char* name, size_t argc, const void* argv, unsigned flags, void* opt, int* res);
extern "C" int sceKernelDlsym(int handle, const char* symbol, void** addr);

// ---- состояние ----
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static std::string g_state = "idle";        // idle install done error
static std::string g_msg;                   // последняя ошибка / пояснение (по-английски)
static int g_pct = 0;
static std::atomic<int> g_req(0);
static std::atomic<int> g_started(0);
typedef int (*fn_i_v)(void);

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
    set_state("install");
    std::string v; bool wasThere = installed_info(v);
    mkdir("/data", 0777); mkdir("/data/pkg", 0777);
    logf_("client: asking the console to download and install %s", CLIENT_URL);
    if (!bgft_install(CLIENT_URL, 0)) return;
    // Скачивает и ставит сама консоль (смотри уведомления PS4). Ждём, пока приложение появится.
    for (int i = 0; i < 100; i++) {
        sleep(3);
        std::string v2; bool pres = installed_info(v2);
        if (!wasThere && pres) { logf_("client: installed, version %s", v2.empty() ? "?" : v2.c_str()); set_state("done"); return; }
        if (wasThere && i >= 20) { set_state("done"); return; }      // переустановка: что-то проверить нечем, через минуту убираем «установка»
    }
    set_state("error", "the client did not appear in 5 minutes (see the PS4 notifications)");
}

static void* worker(void*)
{
    sleep(25);                                // сеть к этому времени уже работает
    std::string v;
    bool pres = installed_info(v);
    logf_("client: on the console: %s%s%s", pres ? "installed" : "not installed", v.empty() ? "" : ", version ", v.c_str());
    // Первая установка — один раз за всё время (метка пишется ДО попытки, чтобы не повторять сбой)
    if (!pres && read_small(AUTO_MARK_FILE).empty()) {
        mkdir("/data", 0777); mkdir("/data/pkg", 0777);
        write_small(AUTO_MARK_FILE, "1");
        logf_("client: not installed, trying once to install automatically");
        do_install();
    }
    for (;;) {
        if (g_req.exchange(0) != 0) do_install();
        sleep(1);
    }
    return NULL;
}

void clientinst_start(int)
{
    if (g_started.exchange(1)) return;
    pthread_t th;
    if (pthread_create(&th, NULL, worker, NULL) == 0) pthread_detach(th);
    else logf_("client: cannot start the installer thread");
}

void clientinst_request_install()
{
    pthread_mutex_lock(&g_mu);
    bool busy = g_state == "install";
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
    std::string st, msg;
    pthread_mutex_lock(&g_mu); st = g_state; msg = g_msg; pthread_mutex_unlock(&g_mu);
    std::string v; bool pres = installed_info(v);
    return "{\"state\":\"" + st + "\",\"present\":" + (pres ? "true" : "false") + ",\"installed\":\"" + esc(v) + "\",\"msg\":\"" + esc(msg) + "\"}";
}
#endif
