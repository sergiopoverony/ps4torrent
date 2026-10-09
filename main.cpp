// ps4torrentd: торрент-клиент без экрана для ps4-payload-sdk.
//
// Что делает:
//   * каждые 10 секунд ищет флешки /mnt/usb0 ... /mnt/usb7 с папкой "torrents";
//   * все файлы .torrent из этой папки (только верхний уровень) ставит в очередь;
//   * качает в  <флешка>/torrents/downloads, состояние докачки хранит в <флешка>/torrents/.state;
//   * готовую раздачу переносит .torrent в <флешка>/torrents/complete;
//   * отдаёт статус по HTTP на порту 8787 (страница / и JSON /status).
//
// Один поток занимается загрузками (как в версии для OpenOrbis: один select на все раздачи),
// второй обслуживает HTTP. Общаются через мьютекс: снимок состояния и очередь команд.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <time.h>
#include <dirent.h>
#include <signal.h>
#include <ucontext.h>
#include <fcntl.h>
#include <stdint.h>
#include <exception>
#include <stdexcept>
#include <sys/resource.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <sys/param.h>
#ifdef HOST_TEST
#include <malloc.h>
#endif
#include <sys/mount.h>
#ifndef __linux__
#include <sys/sysctl.h>      // только консоль: узнаём, не в "тюрьме" ли процесс
#endif
#include <sys/time.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <algorithm>
#include <atomic>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "log.h"
#include "net.h"
#include "torrent.h"
#include "storage.h"
#include "download.h"
#include "swarm.h"
#include "resume.h"
#include "incoming.h"
#include "writer.h"
#include "bigbuf.h"
#include "tracker.h"

#ifndef USB_BASE
#define USB_BASE "/mnt/usb"          // /mnt/usb0, /mnt/usb1, ...
#endif
#ifndef EXT_BASE
#define EXT_BASE "/mnt/ext"          // расширенный диск (extended storage): /mnt/ext0, /mnt/ext1
#endif
#ifndef HTTP_PORT
#define HTTP_PORT 8787
#endif
// Версия программы. Правило: при каждой правке и пересборке увеличивать последнюю цифру (1.2.0.0 -> 1.2.0.1 -> ...);
// при крупных изменениях менять и старшие. Версия видна внизу страницы, в /status, в /api/config и в начале лога.
#define APP_VERSION "1.4.1.4"

#ifndef WRITE_FLUSH_BUDGET_MS
#define WRITE_FLUSH_BUDGET_MS 300     // сколько ждать дозаписи проверенных кусков при остановке (остальное допишется в фоне)
#endif
// Картинки, вшитые в ELF (assets.S): значок сайта, логотип страницы, иконка уведомлений.
extern "C" {
extern const unsigned char ps4_favicon_start[], ps4_favicon_end[];
extern const unsigned char ps4_logo_start[], ps4_logo_end[];
extern const unsigned char ps4_notify_start[], ps4_notify_end[];
}
// Завершённые раздачи показываются в списке и после перезапуска (их .torrent лежит в complete/), пока их не удалят. Обычной памяти
// у процесса на консоли ~10 МБ, а раздача в памяти занимает около 1,3 КБ на файл, поэтому показываем не больше этого:
#ifndef MAX_DONE_ITEMS
#define MAX_DONE_ITEMS 100
#endif
#ifndef MAX_DONE_FILES
#define MAX_DONE_FILES 3000
#endif
#ifndef RESUME_FIRST_DELAY_S
#define RESUME_FIRST_DELAY_S 20       // через сколько секунд после запуска возобновляется первая задача, что качалась раньше
#endif
#ifndef RESUME_STAGGER_S
#define RESUME_STAGGER_S 15           // и с каким интервалом возобновляются следующие (чтобы не нагружать консоль разом)
#endif
#ifndef LOG_DIR
#define LOG_DIR "/data/ps4torrent"
#endif
#define SESSION_FILE LOG_DIR "/session.txt"
#ifndef MNT_DIR
#define MNT_DIR "/mnt"                          // где система монтирует накопители (только для диагностики)
#endif
#ifndef DEV_DIR
#define DEV_DIR "/dev"                          // где лежат устройства (только для диагностики)
#endif
#ifndef INTERNAL_ROOT
#define INTERNAL_ROOT "/data/pkg/torrents"       // место хранения во внутренней памяти консоли (системный установщик видит /data/pkg)
#endif
#ifndef LOW_FREE_USB_BYTES
#define LOW_FREE_USB_BYTES (2ull << 30)          // меньше этого на флешке: предупреждаем (2 ГБ)
#endif
#ifndef LOW_FREE_INTERNAL_BYTES
#define LOW_FREE_INTERNAL_BYTES (5ull << 30)     // меньше этого во внутренней памяти: предупреждаем (5 ГБ)
#endif
#ifndef INTERNAL_MIN_FREE_BYTES
#define INTERNAL_MIN_FREE_BYTES (1ull << 30)     // меньше этого во внутренней памяти: загрузки не запускаем (защита системы)
#endif
#ifndef WRITE_RETRY_S
#define WRITE_RETRY_S 20          // через сколько секунд повторить раздачу, остановившуюся из-за ошибки записи
#endif
#ifndef AUTOSTART_DEFAULT
#define AUTOSTART_DEFAULT 0     // 0: появившиеся раздачи ждут на паузе, пока их не запустит пользователь
#endif
#ifndef LISTEN_PORT
#define LISTEN_PORT 6881         // порт для входящих соединений пиров
#endif
#ifndef CONFIG_FILE
#define CONFIG_FILE "/data/ps4torrent/config.txt"
#endif

static const int MAX_USB = 8;
static const int MAX_EXT = 2;                          // /mnt/ext0, /mnt/ext1
static const int SLOT_COUNT = MAX_USB + MAX_EXT;       // номера накопителей: 0..7 = usbN, 8..9 = ext0..ext1
static void slot_mount(int s, char* buf, size_t n)
{
    if (s < MAX_USB) snprintf(buf, n, "%s%d", USB_BASE, s);
    else snprintf(buf, n, "%s%d", EXT_BASE, s - MAX_USB);
}
static bool slot_is_ext(int s) { return s >= MAX_USB; }
// Проверка уже скачанного (чтение файлов и SHA-1) работает чередой "поработали - передохнули", чтобы не занимать
// процессор и USB целиком: иначе вся консоль начинала тормозить. Примерно 40% времени работы, 60% отдыха.
static const int CHECK_WORK_US = 8000;
static const int CHECK_PAUSE_US = 12000;
static const uint64_t MAX_TORRENT_FILE = 2u * 1024 * 1024;     // .torrent больше 2 МБ не разбираем

// ---------------------------------------------------------------- уведомления

// Иконка уведомлений: картинка вшита в программу и отдаётся нашим же веб-сервером (/notify.png); система берёт её по
// адресу http://127.0.0.1:ПОРТ/notify.png (этот способ проверен на консоли). Задаётся при запуске.
static char g_notifyIcon[512] = "";

#ifdef HOST_TEST
static void notify_va(const char* icon, const char* fmt, va_list ap)
{
    printf("[notify] ");
    if (icon && icon[0]) printf("(icon %s) ", icon);
    vprintf(fmt, ap);
    printf("\n");
}
#else
// Расширенная форма запроса уведомления (так её описывают другие программы для PS4): 11 целых полей, флаг "своя
// иконка" в байте 44, затем текст (с байта 45), адрес иконки и запас. Размер и положение текста те же, что у прежней
// упрощённой структуры (45 байт + 3075 байт), поэтому уведомления без иконки не меняются ни на байт.
struct notify_request_t {
    int type, reqId, priority, msgId, targetId, userId, unk1, unk2, appId, errorNum, unk3;
    unsigned char useIconImageUri;
    char message[1024];
    char iconImageUri[1024];
    char unk[1024];
};
static_assert(sizeof(notify_request_t) == 3120, "notification request size");
static_assert(offsetof(notify_request_t, message) == 45, "notification message offset");
extern "C" int sceKernelSendNotificationRequest(int, notify_request_t*, size_t, int);

static void notify_va(const char* icon, const char* fmt, va_list ap)
{
    notify_request_t req;
    memset(&req, 0, sizeof(req));
    vsnprintf(req.message, sizeof(req.message), fmt, ap);
    if (icon && icon[0]) {
        req.useIconImageUri = 1;
        strncpy(req.iconImageUri, icon, sizeof(req.iconImageUri) - 1);
    }
    sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}
#endif

static void notify(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    notify_va(g_notifyIcon, fmt, ap);
    va_end(ap);
}

static void notify_icon(const char* icon, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    notify_va(icon, fmt, ap);
    va_end(ap);
}

// ---------------------------------------------------------------- вспомогательное

static uint64_t now_us()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000;
}

static bool ends_with_ci(const std::string& s, const char* suffix)
{
    size_t a = s.size(), b = strlen(suffix);
    if (a < b) return false;
    for (size_t i = 0; i < b; i++) {
        char c = s[a - b + i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != suffix[i]) return false;
    }
    return true;
}

static bool make_dirs(const std::string& p)
{
    for (size_t i = 1; i < p.size(); i++)
        if (p[i] == '/') mkdir(p.substr(0, i).c_str(), 0777);
    mkdir(p.c_str(), 0777);
    struct stat sb;
    return stat(p.c_str(), &sb) == 0 && S_ISDIR(sb.st_mode);
}

static bool is_dir(const std::string& p)
{
    struct stat sb;
    return stat(p.c_str(), &sb) == 0 && S_ISDIR(sb.st_mode);
}

// Путь из .torrent не должен выходить за пределы папки загрузок.
static bool path_safe(const std::string& p)
{
    if (p.empty() || p[0] == '/') return false;
    size_t start = 0;
    while (start <= p.size()) {
        size_t end = p.find('/', start);
        if (end == std::string::npos) end = p.size();
        std::string comp = p.substr(start, end - start);
        if (comp.empty() || comp == "." || comp == "..") return false;
        if (comp.size() > 255) return false;
        start = end + 1;
    }
    return true;
}

// Обрезает строку до maxBytes, не разрывая символ UTF-8 (для уведомлений).
static std::string cut_utf8(const std::string& s, size_t maxBytes)
{
    if (s.size() <= maxBytes) return s;
    size_t n = maxBytes;
    while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--;
    return s.substr(0, n) + "...";
}

static std::string trim(const std::string& s)
{
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) a++;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) b--;
    return s.substr(a, b - a);
}

// ---------------------------------------------------------------- состояние

struct Item {
    std::string path;        // полный путь к .torrent (сейчас)
    std::string fileName;    // имя файла .torrent
    std::string title;       // имя без ".torrent"
    std::string root;        // /mnt/usbN/torrents
    Torrent t;
    bool paused, offline, complete, failed, running, haveValid;
    uint32_t have;
    int checkPct, speedKB, peers;
    uint64_t sessBytes;      // сколько байт получено за текущий запуск загрузки
    std::vector<std::pair<time_t, uint64_t> > rate;   // (время, sessBytes) за последнюю минуту, для оценки времени
    time_t retryAt, notBefore;          // retryAt: повтор после ошибки; notBefore: не стартовать раньше (очередь возобновления)
    std::vector<bool> haveMap;
    // Выбор файлов (пусто = выбраны все).
    std::vector<bool> fileSel;          // fileSel[k] = файл k выбран
    std::vector<bool> skipPiece;        // куски, которые целиком принадлежат снятым файлам (их не качаем)
    std::vector<bool> inexactPiece;     // куски, затрагивающие снятые файлы: по диску их не проверить
    uint64_t wantBytes;                 // размер нужных кусков (равен размеру раздачи, если выбрано всё)
    uint32_t skipCount;
    bool selPending;                    // смена выбора файлов ждёт, пока остановится загрузка и допишутся записи
    std::string selPendingTxt;

    Item() : paused(false), offline(false), complete(false), failed(false), running(false),
             haveValid(false), have(0), checkPct(0), speedKB(0), peers(0), sessBytes(0), retryAt(0), notBefore(0), wantBytes(0), skipCount(0), selPending(false) {}

    std::string dlDir() const { return root + "/downloads"; }
    std::string doneDir() const { return root + "/complete"; }
    std::string stateDir() const { return root + "/.state"; }
};


// ---------------------------------------------------------------- выбор файлов раздачи

// Список снятых файлов "3,5-9" -> sel[k] (true = выбран). Пустая строка: выбраны все. false: неверный формат.
static bool sel_from_text(const std::string& txt, size_t nFiles, std::vector<bool>& sel)
{
    sel.assign(nFiles, true);
    size_t p = 0;
    while (p < txt.size()) {
        size_t e = txt.find(',', p);
        if (e == std::string::npos) e = txt.size();
        std::string tok = txt.substr(p, e - p);
        p = e + 1;
        if (tok.empty()) return false;
        size_t dash = tok.find('-');
        std::string a = tok.substr(0, dash), b = dash == std::string::npos ? a : tok.substr(dash + 1);
        if (a.empty() || b.empty() || a.size() > 9 || b.size() > 9) return false;
        for (size_t i = 0; i < a.size(); i++) if (a[i] < '0' || a[i] > '9') return false;
        for (size_t i = 0; i < b.size(); i++) if (b[i] < '0' || b[i] > '9') return false;
        unsigned long lo = strtoul(a.c_str(), NULL, 10), hi = strtoul(b.c_str(), NULL, 10);
        if (lo > hi || hi >= nFiles) return false;
        for (unsigned long k = lo; k <= hi; k++) sel[k] = false;
    }
    return true;
}

static std::string sel_to_text(const std::vector<bool>& sel)
{
    std::string out;
    for (size_t k = 0; k < sel.size();) {
        if (sel[k]) { k++; continue; }
        size_t e = k;
        while (e + 1 < sel.size() && !sel[e + 1]) e++;
        out += (out.empty() ? "" : ",") + std::to_string(k) + (e > k ? "-" + std::to_string(e) : "");
        k = e + 1;
    }
    return out;
}

// Выбран ли хоть один непустой файл (иначе качать нечего).
static bool sel_has_data(const Torrent& t, const std::vector<bool>& sel)
{
    for (size_t k = 0; k < t.files.size() && k < sel.size(); k++) if (sel[k] && t.files[k].length > 0) return true;
    return false;
}

// Применяет выбор к раздаче: какие куски нужны, какие нет, сколько байт качать. false: нечего качать.
static bool apply_selection(Item& it, const std::vector<bool>& sel)
{
    const Torrent& t = it.t;
    if (sel.size() != t.files.size() || !sel_has_data(t, sel)) return false;
    bool all = true;
    for (size_t k = 0; k < sel.size(); k++) if (!sel[k]) all = false;
    it.skipPiece.clear();
    it.inexactPiece.clear();
    it.skipCount = 0;
    it.wantBytes = t.totalSize;
    if (all) { it.fileSel.clear(); return true; }
    it.fileSel = sel;
    const uint32_t n = t.numPieces;
    const uint64_t pl = t.pieceLength;
    std::vector<bool> wanted(n, false), touched(n, false);
    uint64_t off = 0;
    for (size_t k = 0; k < t.files.size(); k++) {
        uint64_t len = t.files[k].length;
        if (len > 0 && pl > 0) {
            uint64_t p0 = off / pl, p1 = (off + len - 1) / pl;
            for (uint64_t p = p0; p <= p1 && p < n; p++) { if (sel[k]) wanted[p] = true; else touched[p] = true; }
        }
        off += len;
    }
    it.skipPiece.assign(n, false);
    it.inexactPiece.assign(n, false);
    uint64_t want = 0;
    for (uint32_t i = 0; i < n; i++) {
        it.skipPiece[i] = !wanted[i];
        it.inexactPiece[i] = touched[i];
        if (!wanted[i]) it.skipCount++;
        else want += (i + 1 == n) ? t.totalSize - (uint64_t)(n - 1) * pl : pl;
    }
    it.wantBytes = want;
    return true;
}

static uint64_t item_size_bytes(const Item& it) { return it.wantBytes ? it.wantBytes : it.t.totalSize; }
static uint32_t item_skipped_files(const Item& it)
{
    uint32_t n = 0;
    for (size_t k = 0; k < it.fileSel.size(); k++) if (!it.fileSel[k]) n++;
    return n;
}

static std::vector<Item*> g_items;
static std::map<std::string, time_t> g_bad;     // .torrent, которые не удалось прочитать (путь -> mtime)
static unsigned char g_peerId[20];
static const int MAX_PARALLEL_LIMIT = 25;          // верхний предел одновременных закачек (по умолчанию 3)
static const int CONN_BUDGET = 90;                  // всего исходящих соединений на все закачки вместе
static int g_maxParallel = 3;
static int g_listenPort = LISTEN_PORT;       // 0 = входящие соединения выключены
static std::atomic<bool> g_autostart(AUTOSTART_DEFAULT != 0);   // запускать ли раздачи сразу (по умолчанию нет)
static std::atomic<int> g_saveTo(1);         // куда класть торренты, добавленные без явного места: 0 флешка, 1 память консоли (по умолчанию)
// Что делать с только что добавленным через API торрентом: путь файла -> запускать сразу (true) или оставить на паузе (false).
// Заполняется потоком HTTP, читается главным потоком при сканировании.
static pthread_mutex_t g_addMu = PTHREAD_MUTEX_INITIALIZER;
static std::map<std::string, bool> g_addStart;
static std::map<std::string, std::string> g_addSel;     // путь .torrent -> список снятых файлов, пришедший вместе с добавлением
static std::map<std::string, uint64_t> g_freeMap;   // свободное место по местам хранения (обновляется при сканировании)
static std::atomic<bool> g_resumeSession(true);    // после перезапуска возобновлять то, что качалось
static std::set<std::string> g_sessionWant;          // хэши раздач, которые пользователь запустил и не останавливал
static std::set<std::string> g_sessionSaved;         // что сейчас записано в файл
static time_t g_sessionLastSave = 0, g_resumeNext = 0;
static std::string g_token;                  // пароль на управление (web_token в config.txt); пусто = без пароля
static std::atomic<bool> g_rescan(false);    // просят сразу просканировать флешки (после добавления .torrent)
static std::string g_netIp;                   // последний известный адрес консоли ("" = сети нет)
static int g_netRestarts = 0;                // сколько раз пересоздавались слушающие сокеты
static std::string g_drivesJson;              // кэш списка мест хранения с папкой torrents (обновляется при сканировании, раз в 10 с)
static std::string g_mountsJson;              // кэш всех смонтированных USB-накопителей (с папкой torrents или без)
static std::set<std::string> g_ignored;      // .torrent, которые нельзя снова подхватывать (не удалось убрать)
static std::atomic<bool> g_quit(false);

static void use_state(const Item& it) { resume_set_dir(it.stateDir()); }

static uint64_t item_done_bytes(const Item& it)
{
    if (it.t.numPieces == 0) return 0;
    const uint64_t size = item_size_bytes(it);
    if (it.complete || (it.fileSel.empty() && it.have >= it.t.numPieces)) return size;
    uint64_t d = (uint64_t)it.have * it.t.pieceLength;
    return d > size ? size : d;
}

static void make_peer_id(unsigned char id[20])
{
    memcpy(id, "-PT0002-", 8);
    srand((unsigned)time(NULL) ^ (unsigned)now_us());
    for (int i = 8; i < 20; i++) id[i] = '0' + (rand() % 10);
}

static void load_config()
{
    FILE* f = fopen(CONFIG_FILE, "r");
    if (!f) { logf_("config: no %s, using defaults", CONFIG_FILE); return; }
    char line[300];
    while (fgets(line, sizeof(line), f)) {
        std::string l = trim(line);
        if (l.empty() || l[0] == '#') continue;
        size_t eq = l.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(l.substr(0, eq));
        std::string val = trim(l.substr(eq + 1));
        if (key == "max_parallel") {
            int v = atoi(val.c_str());
            g_maxParallel = v < 1 ? 1 : (v > MAX_PARALLEL_LIMIT ? MAX_PARALLEL_LIMIT : v);
        } else if (key == "listen_port") {
            int v = atoi(val.c_str());
            g_listenPort = (v >= 1024 && v < 65536) ? v : 0;       // 0 или некорректное значение: выключено
        } else if (key == "web_token") {
            g_token = val;
        } else if (key == "autostart") {
            g_autostart = (val == "1" || val == "true" || val == "yes" || val == "on");
        } else if (key == "save_to") {
            g_saveTo = (val == "internal") ? 1 : 0;
        } else if (key == "resume_session") {
            g_resumeSession = !(val == "0" || val == "false" || val == "no" || val == "off");
        }
    }
    fclose(f);
}

// Настройки меняются через API, поэтому файл перезаписывается целиком.
static void save_config()
{
    FILE* f = fopen(CONFIG_FILE, "w");
    if (!f) { logf_("cannot write %s", CONFIG_FILE); return; }
    fprintf(f, "# ps4torrentd settings (rewritten when changed through the web page / API)\n");
    fprintf(f, "max_parallel=%d\nlisten_port=%d\nautostart=%d\nsave_to=%s\nresume_session=%d\n", g_maxParallel, g_listenPort, g_autostart ? 1 : 0, g_saveTo.load() == 1 ? "internal" : "usb", g_resumeSession.load() ? 1 : 0);
    if (!g_token.empty()) fprintf(f, "web_token=%s\n", g_token.c_str());
    fclose(f);
}

static Item* find_hash(const std::string& hex)
{
    for (size_t i = 0; i < g_items.size(); i++)
        if (g_items[i]->t.infoHashHex == hex) return g_items[i];
    return NULL;
}

static bool has_path(const std::string& path)
{
    for (size_t i = 0; i < g_items.size(); i++)
        if (g_items[i]->path == path) return true;
    return false;
}

// ---------------------------------------------------------------- загрузки

struct Job {
    Item* it;
    Storage st;
    DownloadState ds;
    Rechecker* rc;          // пока идёт проверка
    Swarm* sw;              // пока идёт загрузка
    time_t t0;
    uint32_t before;
    time_t lastSave;
    uint32_t savedCount;
    explicit Job(Item* i) : it(i), rc(NULL), sw(NULL), t0(time(NULL)), before(0), lastSave(time(NULL)), savedCount(0) {}
    ~Job() { delete rc; delete sw; }
};

static std::vector<Job*> g_active;
// Раздачи, которые уже остановлены, но поток записи ещё дописывает их проверенные куски на диск (флешка бывает
// очень медленной или зависшей). Сеть у них закрыта; пока запись идёт, Storage освобождать нельзя (с ним работает
// поток записи). Главный цикл по мере готовности завершает куски и дописывает файл прогресса: остановка ничего не ждёт.
struct Zombie {
    Job* j;
    std::string hash, stateDir, title;
    uint32_t numPieces;
    Item* orphan;           // раздачу удалили из списка, пока она дописывала куски: Swarm ещё ссылается на её Torrent, освободим вместе с Job
    Zombie() : j(NULL), numPieces(0), orphan(NULL) {}
};
static std::vector<Zombie> g_zombies;

// Убрать запись о раздаче из памяти. Если эта раздача сейчас "дописывается" потоком записи (зомби), её Torrent ещё нужен
// остановленной загрузке (ссылка в Swarm), поэтому освобождение откладывается до конца записей. Раньше запись освобождалась
// сразу, и при удалении раздачи с недописанными кусками программа падала (обращение к освобождённой памяти).
static void retire_item(Item* it)
{
    for (size_t i = 0; i < g_zombies.size(); i++) {
        if (g_zombies[i].j && g_zombies[i].hash == it->t.infoHashHex && g_zombies[i].orphan == NULL) {
            g_zombies[i].orphan = it;
            return;
        }
    }
    delete it;
}

static Item* find_item_by_hash(const std::string& h)
{
    for (size_t i = 0; i < g_items.size(); i++)
        if (g_items[i]->t.infoHashHex == h) return g_items[i];
    return NULL;
}

static void finish_item(Item& it, bool announce);

// Раздача остановлена в момент записи куска на диск, запись закончилась: дописываем прогресс.
static void zombie_late_save(Zombie& z)
{
    Job* j = z.j;
    Item* it = find_item_by_hash(z.hash);
    if (!it) return;                                      // раздачу уже удалили: прогресс не нужен
    // Файл прогресса мог за это время обновить новый запуск этой же раздачи: поток записи объединяет, а не затирает.
    const std::string dir = z.stateDir, hash = z.hash;
    const uint32_t n = z.numPieces;
    const std::vector<bool> have = j->ds.have;
    const std::vector<uint32_t> recent = j->ds.recent;
    writer_post([dir, hash, n, have, recent]() { resume_merge_save_at(dir, hash, n, have, recent); });

    if (!it->running) {
        std::vector<bool> merged = have;
        if (it->haveValid && it->haveMap.size() == merged.size())
            for (size_t i = 0; i < merged.size(); i++) if (it->haveMap[i]) merged[i] = true;
        uint32_t cnt = 0;
        for (size_t i = 0; i < merged.size(); i++) if (merged[i]) cnt++;
        it->haveMap = merged;
        it->haveValid = true;
        it->have = cnt;
        if (z.j && z.j->ds.needLeft == 0 && z.numPieces > 0 && !it->complete) {
            logf_("  the last pieces of %s were written after the stop: the task is complete", z.title.c_str());
            finish_item(*it, true);
        }
    }
}

// Главный цикл вызывает это на каждом проходе: завершает записанные куски и освобождает раздачи, у которых
// поток записи закончил работу.
static void reap_zombies(time_t now)
{
    (void)now;
    for (size_t i = g_zombies.size(); i > 0; i--) {
        Zombie& z = g_zombies[i - 1];
        if (z.j->sw) z.j->sw->poll_writes();              // уже записанные куски: учесть
        if (z.j->st.pending_writes() > 0) continue;       // поток записи ещё работает с файлами этой раздачи
        if (z.j->sw) z.j->sw->poll_writes();
        zombie_late_save(z);
        int lost = z.j->sw ? z.j->sw->failed_writes() : 0;
        if (lost > 0)
            logf_("  WARNING: %d verified piece(s) of %s could NOT be written to the drive (error or drive removed): they will be downloaded again; progress of the rest is saved",
                  lost, z.title.c_str());
        else
            logf_("  disk writes of %s finished, progress saved", z.title.c_str());
        delete z.j;
        delete z.orphan;
        g_zombies.erase(g_zombies.begin() + (i - 1));
    }
}

static void close_job(Job* j, time_t now, bool userStop);

static void stop_job_for(Item* it, time_t now)
{
    for (size_t i = 0; i < g_active.size(); i++) {
        if (g_active[i]->it == it) { close_job(g_active[i], now, true); return; }
    }
}

// Готовая раздача: .torrent уходит в complete, сохранённый прогресс не нужен.
static void finish_item(Item& it, bool announce)
{
    use_state(it);
    resume_remove(it.t.infoHashHex);
    it.complete = true;
    it.have = it.t.numPieces;
    it.speedKB = 0;
    it.peers = 0;
    it.failed = false;
    logf_("  COMPLETE: %s", it.t.name.c_str());

    if (announce) notify("Torrent done: %s", cut_utf8(it.title, 110).c_str());

    make_dirs(it.doneDir());
    std::string dest = it.doneDir() + "/" + it.fileName;
    struct stat sb;
    if (stat(dest.c_str(), &sb) == 0) {
        char suffix[32];
        snprintf(suffix, sizeof(suffix), ".%lld", (long long)time(NULL));
        dest += suffix;
    }
    if (rename(it.path.c_str(), dest.c_str()) == 0) {
        logf_("  moved .torrent to %s", dest.c_str());
        it.path = dest;
    } else {
        logf_("  WARNING: cannot move .torrent to complete (errno %d), it stays in %s", errno, it.path.c_str());
    }
}

// Быстрая проверка: файлы на месте и не короче, чем должны быть (чтобы не пересчитывать куски).
static bool quick_validate(const Item& it)
{
    const Torrent& t = it.t;
    if (!it.haveValid || t.numPieces == 0 || t.pieceLength == 0 || it.haveMap.size() != t.numPieces) return false;

    uint64_t off = 0;
    for (size_t k = 0; k < t.files.size(); k++) {
        uint64_t len = t.files[k].length;
        uint64_t fileOff = off;
        off += len;
        if (len == 0) continue;
        if (!it.fileSel.empty() && k < it.fileSel.size() && !it.fileSel[k]) continue;      // файл снят с загрузки: его нет на диске, и это нормально

        uint64_t first = fileOff / t.pieceLength;
        uint64_t last = (fileOff + len - 1) / t.pieceLength;
        if (last >= t.numPieces) last = t.numPieces - 1;

        int64_t hp = -1;
        for (uint64_t p = last + 1; p > first; p--) {
            if (it.haveMap[p - 1]) { hp = (int64_t)(p - 1); break; }
        }
        if (hp < 0) continue;

        uint64_t reach = ((uint64_t)hp + 1) * t.pieceLength;
        if (reach <= fileOff) continue;
        uint64_t need = std::min<uint64_t>(len, reach - fileOff);

        if (!path_safe(t.files[k].path)) return false;
        struct stat sb;
        std::string full = it.dlDir() + "/" + t.files[k].path;
        if (stat(full.c_str(), &sb) != 0 || !S_ISREG(sb.st_mode) || (uint64_t)sb.st_size < need) return false;
    }
    return true;
}

static void spot_add(const Item& it, std::vector<uint32_t>& out, uint64_t& bytes, uint32_t idx)
{
    if (idx >= it.t.numPieces || !it.haveMap[idx] || out.size() >= 8) return;
    if (!it.inexactPiece.empty() && it.inexactPiece[idx]) return;       // кусок затрагивает снятый файл: по диску не проверить
    for (size_t i = 0; i < out.size(); i++) if (out[i] == idx) return;
    uint64_t start = (uint64_t)idx * it.t.pieceLength;
    uint64_t sz = std::min<uint64_t>(it.t.pieceLength, it.t.totalSize - start);
    if (!out.empty() && bytes + sz > (32u << 20)) return;
    out.push_back(idx);
    bytes += sz;
}

// Какие куски проверить выборочно после перезапуска: последние скачанные и несколько случайных.
static std::vector<uint32_t> choose_spot(const Item& it, const std::vector<uint32_t>& recent)
{
    std::vector<uint32_t> out;
    uint64_t bytes = 0;
    for (size_t i = recent.size(); i > 0; i--) spot_add(it, out, bytes, recent[i - 1]);
    for (int tries = 0; tries < 64 && out.size() < 8 && it.t.numPieces > 0; tries++)
        spot_add(it, out, bytes, (uint32_t)(rand() % it.t.numPieces));
    return out;
}

// Сохраняет прогресс: готовые куски (файл .have) и недокачанные куски (файл .part, сами блоки при этом
// дописываются в итоговые файлы). Так пауза, остановка и перезапуск ничего не теряют.
// Сохранение прогресса при остановке раздачи. Всё, что касается флешки, уходит в поток записи: главный цикл
// ничего не ждёт (на медленной или зависшей флешке одна запись занимает секунды или дольше). Задачи выполняются
// по порядку, поэтому файл прогресса записывается уже после недокачанных блоков и кусков, поставленных раньше.
static void save_progress(Job& j, time_t now)
{
    Item& it = *j.it;
    j.lastSave = now;
    j.savedCount = j.ds.haveCount;
    const std::string dir = it.stateDir(), hash = it.t.infoHashHex;
    const uint32_t n = it.t.numPieces;

    std::vector<PartialJobInfo> parts;
    if (j.sw) j.sw->persist_partials_async(parts);       // недокачанные блоки: крупными сериями, потоком записи
    const std::vector<bool> have = j.ds.have;
    const std::vector<uint32_t> recent = j.ds.recent;
    writer_post([dir, hash, n, have, recent, parts]() {
        resume_save_at(dir, hash, n, have, recent);
        std::vector<PartialPiece> list;
        for (size_t i = 0; i < parts.size(); i++) {
            const PartialJobInfo& pi = parts[i];
            // Если запись блоков не удалась, в список попадают только те, что лежали на диске раньше.
            const std::vector<unsigned char>& got = (pi.job && pi.job->state.load() != 2) ? pi.before : pi.after;
            PartialPiece pp;
            pp.index = pi.index;
            pp.nblocks = pi.nblocks;
            pp.got = got;
            bool any = false;
            for (size_t b = 0; b < got.size(); b++) if (got[b]) { any = true; break; }
            if (any) list.push_back(pp);
        }
        resume_save_partial_at(dir, hash, n, list);
    });
}

// Проверка закончена (или пропущена): начинаем загрузку. true, если уже всё скачано.
static bool job_after_check(Job& j)
{
    Item& it = *j.it;
    it.haveMap = j.ds.have;
    it.haveValid = true;
    it.have = j.ds.haveCount;
    logf_("  already have %u of %u pieces", j.ds.haveCount, it.t.numPieces);
    j.ds.skip = it.skipPiece;                                    // ненужные куски (снятые файлы)
    ds_recount(j.ds, it.t.numPieces);
    if (it.skipCount) logf_("  selected files only: %u of %u pieces are not needed", it.skipCount, it.t.numPieces);

    if (j.ds.needLeft == 0) return true;

    j.before = j.ds.haveCount;
    j.t0 = time(NULL);
    // Недокачанные куски с прошлого раза (Swarm прочитает их блоки из файлов и очистит список).
    use_state(it);
    resume_load_partial(it.t.infoHashHex, it.t.numPieces, j.ds.partials);
    j.sw = new Swarm(it.t, j.st, j.ds, g_peerId);
    return false;
}

static uint32_t count_have(const std::vector<bool>& v)
{
    uint32_t n = 0;
    for (size_t i = 0; i < v.size(); i++) if (v[i]) n++;
    return n;
}

// ---------------------------------------------------------------- место на диске

static std::string fmt_bytes(uint64_t n)
{
    const char* u[] = {"B", "KB", "MB", "GB", "TB"};
    double v = (double)n;
    int i = 0;
    while (v >= 1024.0 && i < 4) { v /= 1024.0; i++; }
    char b[32];
    snprintf(b, sizeof(b), i ? "%.1f %s" : "%.0f %s", v, u[i]);
    return b;
}

static bool is_internal_root(const std::string& r) { return r == INTERNAL_ROOT; }
static uint64_t low_reserve(const std::string& root) { return is_internal_root(root) ? LOW_FREE_INTERNAL_BYTES : LOW_FREE_USB_BYTES; }

static uint64_t item_remaining(const Item& it)
{
    uint64_t d = item_done_bytes(it);
    return item_size_bytes(it) > d ? item_size_bytes(it) - d : 0;
}

// Не хватит ли места, чтобы докачать раздачу и ещё оставить запас (по последним известным данным о диске).
static bool item_low_space(const Item& it)
{
    if (it.complete || it.offline) return false;
    std::map<std::string, uint64_t>::const_iterator f = g_freeMap.find(it.root);
    if (f == g_freeMap.end()) return false;
    return f->second < item_remaining(it) + low_reserve(it.root);
}

static void start_job(Item& it, time_t now)
{
    // Проверка места. Во внутренней памяти консоли свободного места должно оставаться хоть сколько-нибудь:
    // если диск забить доверху, система начинает работать плохо. Поэтому, когда его почти не осталось, загрузку
    // не запускаем (повтор через минуту). В остальных случаях только предупреждаем.
    {
        std::map<std::string, uint64_t>::const_iterator f = g_freeMap.find(it.root);
        if (f != g_freeMap.end()) {
            static std::map<std::string, time_t> lastTold;
            time_t& told = lastTold[it.t.infoHashHex];
            bool tell = (now - told >= 600);
            if (is_internal_root(it.root) && f->second < INTERNAL_MIN_FREE_BYTES) {
                logf_("not starting %s: only %s free in the console memory (the system needs room to work)", it.title.c_str(), fmt_bytes(f->second).c_str());
                if (tell) { told = now; notify("ps4torrent: NOT starting a download, only %s left in the console memory", fmt_bytes(f->second).c_str()); }
                it.failed = true;
                it.retryAt = now + 60;
                return;
            }
            if (item_low_space(it)) {
                logf_("WARNING: low free space for %s: %s free, about %s still to download (+ %s reserve)", it.title.c_str(),
                      fmt_bytes(f->second).c_str(), fmt_bytes(item_remaining(it)).c_str(), fmt_bytes(low_reserve(it.root)).c_str());
                if (tell) { told = now; notify("ps4torrent: LOW FREE SPACE on %s: %s free, this download still needs about %s",
                                               is_internal_root(it.root) ? "console memory" : it.root.c_str(), fmt_bytes(f->second).c_str(), fmt_bytes(item_remaining(it)).c_str()); }
            }
        }
    }

    logf_("torrent: %s (%llu bytes, %u pieces)", it.title.c_str(),
          (unsigned long long)it.t.totalSize, it.t.numPieces);

    make_dirs(it.dlDir());

    Job* j = new Job(&it);
    std::string err;
    if (!it.fileSel.empty()) {                                      // файлы, снятые с загрузки, не создаются и не пишутся
        std::vector<bool> sk(it.fileSel.size());
        for (size_t k = 0; k < sk.size(); k++) sk[k] = !it.fileSel[k];
        j->st.set_skip_files(sk);
    }
    if (!j->st.open(it.t, it.dlDir(), err)) {
        logf_("  storage FAILED: %s", err.c_str());
        it.haveValid = false;
        it.failed = true;
        it.retryAt = now + 300;
        delete j;
        return;
    }

    it.running = true;
    it.checkPct = 0;
    it.speedKB = 0;
    it.peers = 0;
    g_active.push_back(j);

    if (quick_validate(it)) {
        j->ds.have = it.haveMap;
        j->ds.haveCount = count_have(j->ds.have);
        it.checkPct = 100;
        logf_("  resuming without a full check (%u of %u pieces already verified)", j->ds.haveCount, it.t.numPieces);
        job_after_check(*j);
        return;
    }

    if (it.haveValid) logf_("  files changed while paused, doing a full check");
    it.haveValid = false;

    // Приложение только запустили: смотрим, есть ли сохранённый прогресс на флешке.
    std::vector<bool> hv;
    std::vector<uint32_t> recent;
    bool useSaved = false;
    use_state(it);
    if (resume_load(it.t.infoHashHex, it.t.numPieces, hv, recent)) {
        it.haveMap = hv;
        it.haveValid = true;
        if (quick_validate(it)) useSaved = true;
        else { it.haveValid = false; logf_("  saved progress does not match the files, doing a full check"); }
    }

    if (useSaved) {
        j->ds.have = hv;
        j->ds.recent = recent;
        j->ds.haveCount = count_have(hv);
        std::vector<uint32_t> spot = choose_spot(it, recent);
        if (spot.empty()) {
            it.checkPct = 100;
            logf_("  resuming from saved progress (%u of %u pieces)", j->ds.haveCount, it.t.numPieces);
            job_after_check(*j);
        } else {
            logf_("  saved progress found (%u of %u pieces), verifying %d pieces", j->ds.haveCount,
                  it.t.numPieces, (int)spot.size());
            j->rc = new Rechecker(j->st, it.t, j->ds, spot);
        }
    } else {
        j->rc = new Rechecker(j->st, it.t, j->ds);
    }
}

static void close_job(Job* j, time_t now, bool userStop)
{
    Item& it = *j->it;
    // Проверенные куски, которые ещё не успели записаться на диск (запись идёт порциями), дописываем сейчас,
    // иначе они пропали бы при паузе или остановке.
    if (j->sw) j->sw->flush_writes(it.offline ? 0 : WRITE_FLUSH_BUDGET_MS);       // чуть подождать: чаще всего запись уже заканчивается
    bool complete = (it.t.numPieces > 0 && j->ds.needLeft == 0 && !j->rc);

    if (j->sw || complete) {
        it.haveMap = j->ds.have;
        it.haveValid = true;
        it.have = j->ds.haveCount;
    }

    if (j->sw) {
        long secs = (long)(now - j->t0);
        if (secs < 1) secs = 1;
        unsigned long long bytes = (unsigned long long)(j->ds.haveCount - j->before) * it.t.pieceLength;
        logf_("  result: %s: %u / %u pieces, %llu KB in %ld s (~%llu KB/s)", it.title.c_str(),
              j->ds.haveCount, it.t.numPieces, bytes / 1024, secs, bytes / 1024 / secs);
    }

    if (j->sw && !complete) save_progress(*j, now);
    bool announce = (j->sw != NULL && j->ds.haveCount > j->before);
    const char* why = j->sw ? j->sw->reason() : "";

    it.speedKB = 0;
    it.peers = 0;
    it.running = false;

    // Файлы закрываем до переноса .torrent и до того, как флешку могут вынуть.
    for (size_t i = 0; i < g_active.size(); i++) {
        if (g_active[i] == j) { g_active.erase(g_active.begin() + i); break; }
    }
    std::string whyCopy = why;
    if (j->sw && (j->st.pending_writes() > 0 || j->sw->pending_writes() > 0)) {
        // Поток записи ещё дописывает проверенные куски (медленная или зависшая флешка). Остановка не ждёт: сеть
        // закрываем сразу, а прогресс допишет главный цикл, когда запись закончится (reap_zombies).
        logf_("  %d write job(s) of %s are still being written to the drive: the task is stopped, progress is saved when the writes finish",
              j->st.pending_writes(), it.title.c_str());
        j->sw->stop_network();
        Zombie z;
        z.j = j;
        z.hash = it.t.infoHashHex;
        z.stateDir = it.stateDir();
        z.title = it.title;
        z.numPieces = it.t.numPieces;
        j->it = NULL;
        delete j->rc;
        j->rc = NULL;
        g_zombies.push_back(z);
    } else {
        delete j;
    }

    if (complete) {
        finish_item(it, announce);
    } else if (!userStop && !it.paused && !it.offline) {
        // Ошибка записи чаще всего временная (флешка только что переподключилась): повторяем быстрее.
        int retryIn = (whyCopy == "write failed") ? WRITE_RETRY_S : 120;
        it.failed = true;
        it.retryAt = now + retryIn;
        logf_("  INCOMPLETE (%s), will retry in %d s", whyCopy.c_str(), retryIn);
    }
}

static int pick_next(time_t now)
{
    for (size_t i = 0; i < g_items.size(); i++) {
        const Item* it = g_items[i];
        if (it->complete || it->running || it->paused || it->offline) continue;
        if (it->failed && now < it->retryAt) continue;
        if (now < it->notBefore) continue;            // очередь возобновления после перезапуска
        if (it->selPending) continue;                // идёт смена выбора файлов: не стартуем со старым выбором
        return (int)i;
    }
    return -1;
}

// Входящее соединение пира: отдаём его той раздаче, которая сейчас качается.
static int adopt_incoming(const unsigned char infoHash[20], int fd, const std::string& key, const std::string& initial)
{
    for (size_t i = 0; i < g_active.size(); i++) {
        Job* j = g_active[i];
        if (j->sw && memcmp(j->it->t.infoHash, infoHash, 20) == 0) return j->sw->adopt(fd, key, initial);
    }
    return 0;
}

// Один проход общего цикла загрузок. Внутри один select() на все раздачи (он же служит паузой).
static void run_downloads(time_t now)
{
    // 1. Останавливаем поставленные на паузу и те, чья флешка пропала.
    for (size_t i = g_active.size(); i > 0; i--) {
        Job* j = g_active[i - 1];
        if (j->it->paused || j->it->offline) {
            logf_("  stopped: %s (%s)", j->it->title.c_str(), j->it->offline ? "drive removed" : "paused");
            close_job(j, now, true);
        }
    }

    // 2. Запускаем новые, пока есть свободные места.
    while ((int)g_active.size() < g_maxParallel) {
        int idx = pick_next(now);
        if (idx < 0) break;
        Item& it = *g_items[(size_t)idx];
        it.failed = false;
        start_job(it, now);
        if (!it.running) it.failed = true;      // не удалось открыть файлы: повтор через retryAt
    }
    if (g_active.empty()) return;

    // 3. Соединения делим между раздачами.
    int nDown = 0;
    for (size_t i = 0; i < g_active.size(); i++) if (g_active[i]->sw) nDown++;
    int perConns = nDown > 0 ? std::max(4, std::min(30, CONN_BUDGET / nDown)) : 30;

    fd_set rf, wf;
    FD_ZERO(&rf);
    FD_ZERO(&wf);
    int maxfd = -1;
    bool checking = false;
    for (size_t i = 0; i < g_active.size(); i++) {
        Job* j = g_active[i];
        if (j->sw) {
            j->sw->set_max_conns(perConns);
            int m = j->sw->prepare(now, rf, wf);
            if (m > maxfd) maxfd = m;
        } else {
            checking = true;
        }
    }

    int im = incoming_prepare(rf);
    if (im > maxfd) maxfd = im;

    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = checking ? CHECK_PAUSE_US : 30000;
    int n = select(maxfd + 1, &rf, &wf, NULL, &tv);
    if (n < 0) {
        if (errno != EINTR) logf_("  select failed errno=%d", errno);
        FD_ZERO(&rf);
        FD_ZERO(&wf);
    }
    now = time(NULL);

    incoming_process(now, rf, adopt_incoming);

    // 4. Обработка: загрузки получают данные, проверки делают очередную порцию.
    uint64_t deadline = now_us() + CHECK_WORK_US;
    std::vector<Job*> ending;
    for (size_t i = 0; i < g_active.size(); i++) {
        Job* j = g_active[i];
        Item& it = *j->it;
        if (j->sw) {
            j->sw->process(now, rf, wf);
            SwarmStatus st = j->sw->status();
            it.have = st.haveCount;
            it.peers = st.sending;          // на экране только те пиры, которые реально присылают данные
            it.speedKB = st.speedKB;
            it.sessBytes = st.totalBytes;
            if (j->sw->finished()) ending.push_back(j);
            else if (now - j->lastSave >= 20) {
                // Раз в 20 секунд: файл прогресса (маленький). Он пишется потоком записи: на флешке даже такая запись
                // бывает медленной. Недокачанные блоки во время работы на диск не пишутся (только при остановке):
                // мелкие записи на флешку доказанно убивали отзывчивость программы.
                j->lastSave = now;
                if (st.haveCount != j->savedCount) {
                    const std::string dir = it.stateDir(), hash = it.t.infoHashHex;
                    const uint32_t n = it.t.numPieces;
                    const std::vector<bool> have = j->ds.have;
                    const std::vector<uint32_t> recent = j->ds.recent;
                    writer_post([dir, hash, n, have, recent]() { resume_save_at(dir, hash, n, have, recent); });
                    j->savedCount = st.haveCount;
                }
            }
        } else if (j->rc) {
            bool done = false;
            do { done = j->rc->step(128 * 1024); } while (!done && now_us() < deadline);
            it.checkPct = j->rc->percent();
            if (done) {
                bool wasSubset = j->rc->is_subset();
                bool spotFailed = wasSubset && j->rc->failed();
                delete j->rc;
                j->rc = NULL;
                if (spotFailed) {
                    logf_("  saved progress does not match the disk, doing a full check");
                    it.haveValid = false;
                    it.checkPct = 0;
                    j->ds = DownloadState();
                    j->rc = new Rechecker(j->st, it.t, j->ds);
                } else {
                    if (wasSubset) logf_("  spot check passed, using saved progress");
                    if (job_after_check(*j)) ending.push_back(j);
                }
            }
        } else if (j->ds.needLeft == 0 && !j->sw) {
            ending.push_back(j);
        }
    }
    for (size_t i = 0; i < ending.size(); i++) close_job(ending[i], now, false);
}

// ---------------------------------------------------------------- удаление раздач

// Удаление раздачи. .torrent никогда не стирается сразу, а переносится в <флешка>/torrents/removed.
// Файлы раздачи (по желанию) удаляются понемногу за проход главного цикла. Стираются только файлы
// из списка этого .torrent и только внутри <флешка>/torrents/downloads; файлы, которые нужны другим
// известным раздачам, не трогаются.
struct DelJob {
    std::string title, root;
    std::vector<std::string> files;     // пути из .torrent (относительные)
    std::set<std::string> keep;         // файлы, нужные другим раздачам
    std::vector<std::string> dirs;      // папки (самые глубокие первыми)
    std::atomic<int> stage;             // 0 ждёт, 1 поток работает, 3 готово (пишет поток, читает главный цикл)
    unsigned removed, skipped;
    DelJob() : stage(0), removed(0), skipped(0) {}
};
static std::vector<DelJob*> g_delJobs;

static bool longer_first(const std::string& a, const std::string& b) { return a.size() > b.size(); }

static void request_delete(Item* it, bool withFiles, time_t now)
{
    if (it->offline) { logf_("delete refused, drive is offline: %s", it->title.c_str()); return; }
    logf_("delete%s: %s", withFiles ? " (with files)" : "", it->title.c_str());

    stop_job_for(it, now);
    g_sessionWant.erase(it->t.infoHashHex);
    use_state(*it);
    resume_remove(it->t.infoHashHex);
    {
        const std::string selDir = it->stateDir(), selHash = it->t.infoHashHex;
        writer_post([selDir, selHash]() { resume_save_sel_at(selDir, selHash, ""); });
    }

    // .torrent -> removed/
    make_dirs(it->root + "/removed");
    std::string dest = it->root + "/removed/" + it->fileName;
    struct stat sb;
    if (stat(dest.c_str(), &sb) == 0) {
        char suffix[32];
        snprintf(suffix, sizeof(suffix), ".%lld", (long long)time(NULL));
        dest += suffix;
    }
    if (rename(it->path.c_str(), dest.c_str()) != 0) {
        logf_("  cannot move .torrent to removed (errno %d)", errno);
        if (unlink(it->path.c_str()) != 0 && stat(it->path.c_str(), &sb) == 0) g_ignored.insert(it->path);
    }

    if (withFiles) {
        DelJob* j = new DelJob();
        j->title = it->title;
        j->root = it->root;
        for (size_t k = 0; k < it->t.files.size(); k++) j->files.push_back(it->t.files[k].path);
        for (size_t i = 0; i < g_items.size(); i++) {
            const Item* o = g_items[i];
            if (o == it || o->root != it->root) continue;
            for (size_t k = 0; k < o->t.files.size(); k++) j->keep.insert(o->dlDir() + "/" + o->t.files[k].path);
        }
        g_delJobs.push_back(j);
    }

    for (size_t i = 0; i < g_items.size(); i++)
        if (g_items[i] == it) { g_items.erase(g_items.begin() + i); break; }
    retire_item(it);
}

// Сами unlink/rmdir идут в отдельном потоке: удаление файла в десятки гигабайт на консоли может занять минуты, и если делать
// это в главном цикле, замирает всё (страница не отвечает, закачки стоят, консоль считает программу зависшей).
// Поток работает только с данными своего DelJob; главный цикл лишь запускает его и забирает результат.
static void* delete_thread(void* arg)
{
    DelJob* j = (DelJob*)arg;
    std::set<std::string> dirs;
    for (size_t k = 0; k < j->files.size(); k++) {
        std::string p = j->files[k];
        if (!path_safe(p)) continue;
        size_t pos;
        while ((pos = p.rfind('/')) != std::string::npos) { p.erase(pos); dirs.insert(p); }
    }
    j->dirs.assign(dirs.begin(), dirs.end());
    std::sort(j->dirs.begin(), j->dirs.end(), longer_first);
    for (size_t k = 0; k < j->files.size(); k++) {
        const std::string& rel = j->files[k];
        std::string full = j->root + "/downloads/" + rel;
        if (!path_safe(rel) || j->keep.count(full)) { j->skipped++; continue; }
#ifdef HOST_TEST
        {   // тесты: файл /tmp/ft/unlink_cost = секунд на каждое удаление (медленный диск)
            FILE* cf = fopen("/tmp/ft/unlink_cost", "r");
            int sec = 0;
            if (cf) { if (fscanf(cf, "%d", &sec) != 1) sec = 0; fclose(cf); }
            if (sec > 0) sleep((unsigned)sec);
        }
#endif
        if (unlink(full.c_str()) == 0) j->removed++;           // нет файла: не страшно
    }
    for (size_t k = 0; k < j->dirs.size(); k++) rmdir((j->root + "/downloads/" + j->dirs[k]).c_str());   // не пустая (чужие файлы): так и надо
    j->stage = 3;
    return NULL;
}

static void process_deletions()
{
    if (g_delJobs.empty()) return;
    DelJob* j = g_delJobs[0];

    if (j->stage == 0) {
        j->stage = 1;                                      // поток запущен
        pthread_t th;
        if (pthread_create(&th, NULL, delete_thread, j) == 0) pthread_detach(th);
        else delete_thread(j);                             // потока не вышло: делаем здесь, как раньше
        return;
    }
    if (j->stage == 3) {
        logf_("delete finished: %s (files removed %u, kept %u)", j->title.c_str(), j->removed, j->skipped);
        g_delJobs.erase(g_delJobs.begin());
        delete j;
    }
}

// ---------------------------------------------------------------- флешки

// ---------------------------------------------------------------- память о том, что качалось

static void load_session()
{
    FILE* f = fopen(SESSION_FILE, "r");
    if (!f) return;
    char line[160];
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "want ", 5) != 0) continue;
        std::string h = line + 5;
        while (!h.empty() && (h[h.size() - 1] == '\n' || h[h.size() - 1] == '\r' || h[h.size() - 1] == ' ')) h.erase(h.size() - 1);
        if (h.size() == 40) g_sessionWant.insert(h);
    }
    fclose(f);
    g_sessionSaved = g_sessionWant;
    if (!g_sessionWant.empty()) logf_("session: %d task(s) were running when the program stopped last time", (int)g_sessionWant.size());
}

// Синхронизирует список "что должно качаться" (не на паузе и не завершено) с файлом. Раздачи, которых сейчас нет
// в списке (например, флешка ещё не смонтирована), в списке остаются: их отметка не теряется.
static void session_sync(time_t now, bool force)
{
    if (!g_resumeSession.load()) return;
    for (size_t i = 0; i < g_items.size(); i++) {
        const Item* it = g_items[i];
        if (!it->complete && !it->paused) g_sessionWant.insert(it->t.infoHashHex);
        else g_sessionWant.erase(it->t.infoHashHex);
    }
    if (g_sessionWant == g_sessionSaved) return;
    if (!force && now - g_sessionLastSave < 3) return;
    g_sessionLastSave = now;
    g_sessionSaved = g_sessionWant;
    std::string content;
    for (std::set<std::string>::const_iterator h = g_sessionWant.begin(); h != g_sessionWant.end(); ++h) content += "want " + *h + "\n";
    writer_post([content]() {                         // запись файла уходит в поток записи: главный цикл диск не ждёт
        std::string tmp = std::string(SESSION_FILE) + ".tmp";
        FILE* f = fopen(tmp.c_str(), "w");
        if (!f) return;
        bool ok = fwrite(content.data(), 1, content.size(), f) == content.size();
        ok = (fclose(f) == 0) && ok;
        if (ok) rename(tmp.c_str(), SESSION_FILE); else remove(tmp.c_str());
    });
}

static void add_item(const std::string& root, const std::string& fileName, bool fromComplete = false)
{
    std::string path = root + (fromComplete ? "/complete/" : "/") + fileName;
    if (g_ignored.count(path)) return;
    struct stat sb;
    time_t mtime = (stat(path.c_str(), &sb) == 0) ? sb.st_mtime : 0;

    std::map<std::string, time_t>::iterator b = g_bad.find(path);
    if (b != g_bad.end() && b->second == mtime) return;      // уже знаем, что файл плохой

    // Разбор .torrent строит дерево в памяти, а обычной памяти у процесса на консоли всего ~10 МБ
    // (около 1,3 КБ на файл раздачи). Слишком большие файлы пропускаем, а нехватку памяти ловим:
    // один необычный .torrent не должен останавливать всю программу.
    Torrent t;
    std::string err;
    bool loaded = false;
    if ((uint64_t)sb.st_size > MAX_TORRENT_FILE) {
        err = "file is too large for this console (more than 2 MB)";
    } else {
        try { loaded = load_torrent(path.c_str(), t, err); }
        catch (const std::exception& e) { err = std::string("not enough memory to load it (") + e.what() + "), too many files?"; loaded = false; }
    }
    if (!loaded) {
        logf_("bad torrent %s: %s", path.c_str(), err.c_str());
        g_bad[path] = mtime;
        return;
    }
    g_bad.erase(path);

    Item* old = find_hash(t.infoHashHex);
    if (fromComplete) {
        if (old) return;                                   // эта раздача уже есть в списке (идёт или завершена): старую копию из complete/ не трогаем
        size_t cnt = 0, files = 0;
        for (size_t i = 0; i < g_items.size(); i++) if (g_items[i]->complete) { cnt++; files += g_items[i]->t.files.size(); }
        if (cnt >= MAX_DONE_ITEMS || files + t.files.size() > MAX_DONE_FILES) {
            static bool told = false;
            if (!told) { told = true; logf_("not all finished torrents from complete/ are shown (limit %d torrents / %d files: the console has little memory); the rest stay in the folder", MAX_DONE_ITEMS, MAX_DONE_FILES); }
            g_bad[path] = mtime;                           // не разбирать этот файл заново при каждом сканировании
            return;
        }
    }
    if (old && old->complete) {
        // Тот же .torrent положили заново (например, чтобы перекачать): старую запись забываем.
        logf_("re-added finished torrent, starting over: %s", old->title.c_str());
        resume_save_sel_at(old->stateDir(), old->t.infoHashHex, "");        // прежний выбор файлов не должен действовать на новую загрузку
        for (size_t i = 0; i < g_items.size(); i++)
            if (g_items[i] == old) { g_items.erase(g_items.begin() + i); break; }
        retire_item(old);
        old = NULL;
    }
    if (old) {
        if (old->offline) {
            // та же раздача, флешку воткнули в другой разъём
            logf_("drive moved: %s -> %s", old->root.c_str(), root.c_str());
            old->root = root;
            old->path = path;
            old->fileName = fileName;
            old->offline = false;
            old->failed = false;
            old->retryAt = 0;
        } else {
            logf_("duplicate torrent ignored: %s", path.c_str());
        }
        return;
    }

    Item* it = new Item();
    it->path = path;
    it->fileName = fileName;
    it->title = ends_with_ci(fileName, ".torrent") ? fileName.substr(0, fileName.size() - 8) : fileName;
    it->root = root;
    it->t = t;
    it->wantBytes = t.totalSize;
    {   // Выбор файлов: пришёл при добавлении (API) или сохранён раньше (.sel рядом с прогрессом).
        std::string apiTxt, skipTxt;
        pthread_mutex_lock(&g_addMu);
        std::map<std::string, std::string>::iterator si = g_addSel.find(path);
        if (si != g_addSel.end()) { apiTxt = si->second; g_addSel.erase(si); }
        pthread_mutex_unlock(&g_addMu);
        skipTxt = apiTxt;
        if (skipTxt.empty()) resume_load_sel(it->stateDir(), t.infoHashHex, skipTxt);
        std::vector<bool> sel;
        if (!skipTxt.empty() && sel_from_text(skipTxt, t.files.size(), sel) && apply_selection(*it, sel)) {
            logf_("  selection: %d of %d files will be downloaded (%llu MB)", (int)(t.files.size() - item_skipped_files(*it)), (int)t.files.size(),
                  (unsigned long long)(it->wantBytes >> 20));
            if (!apiTxt.empty()) {
                const std::string dir = it->stateDir(), h = t.infoHashHex;
                writer_post([dir, h, apiTxt]() { resume_save_sel_at(dir, h, apiTxt); });
            }
        }
    }
    if (fromComplete) {                                    // завершённая раньше раздача: просто показываем (до удаления)
        it->complete = true;
        it->have = t.numPieces;
        it->paused = false;
        g_items.push_back(it);
        logf_("item: %s (finished earlier, %llu bytes, %d files) on %s", it->title.c_str(), (unsigned long long)item_size_bytes(*it), (int)t.files.size(), root.c_str());
        return;
    }
    // Явное решение пользователя при добавлении (кнопка Download / параметр start) важнее общей настройки автозапуска.
    bool startNow = g_autostart;
    bool explicitChoice = false;
    pthread_mutex_lock(&g_addMu);
    std::map<std::string, bool>::iterator ai = g_addStart.find(path);
    if (ai != g_addStart.end()) { startNow = ai->second; explicitChoice = true; g_addStart.erase(ai); }
    pthread_mutex_unlock(&g_addMu);
    // Раздача, которая качалась, когда программа остановилась (перезапуск, сон консоли), возобновляется сама, но по
    // очереди: первая через RESUME_FIRST_DELAY_S секунд, остальные с интервалом RESUME_STAGGER_S.
    bool restored = false;
    long resumeIn = 0;
    if (!startNow && g_resumeSession.load() && g_sessionWant.count(t.infoHashHex)) {
        time_t nowT = time(NULL);
        time_t at = (g_resumeNext == 0 || g_resumeNext < nowT) ? nowT + RESUME_FIRST_DELAY_S : g_resumeNext + RESUME_STAGGER_S;
        g_resumeNext = at;
        startNow = true;
        restored = true;
        resumeIn = (long)(at - nowT);
        it->notBefore = at;
    }
    it->paused = !startNow;              // иначе раздача ждёт, пока пользователь не нажмёт «продолжить»
    g_items.push_back(it);
    logf_("item: %s (%llu bytes, %u pieces, %d files) on %s%s", it->title.c_str(),
          (unsigned long long)t.totalSize, t.numPieces, (int)t.files.size(), root.c_str(),
          it->paused ? (explicitChoice ? " [added paused]" : " [waiting: autostart is off]") : (restored ? (" [resuming: it was downloading before the restart, starts in " + std::to_string(resumeIn) + " s]").c_str() : (explicitChoice ? " [starting: requested]" : "")));
}

// ---------------------------------------------------------------- проверка пропавшего хранилища

// Кто мы для системы: если после сна консоли права процесса или его "тюрьма" (jail) изменились, он видит
// файловую систему иначе, чем, например, FTP-сервер.
static std::string process_identity()
{
    char b[120];
    int jailed = -1;
#ifndef __linux__
    int v = 0;
    size_t len = sizeof(v);
    if (sysctlbyname("security.jail.jailed", &v, &len, NULL, 0) == 0) jailed = v;
#endif
    snprintf(b, sizeof(b), "uid=%d euid=%d gid=%d jailed=%d", (int)getuid(), (int)geteuid(), (int)getgid(), jailed);
    return b;
}

// Результат проверки одной точки монтирования. Обращения к файловой системе (stat, opendir, statfs) могут подвиснуть
// на "мёртвом" монтировании, поэтому их делает отдельный поток, а главный цикл только забирает результат.
struct StorageProbe {
    std::atomic<int> done;
    std::string mp, root, text;
    StorageProbe() : done(0) {}
};

static void* probe_thread(void* arg)
{
    std::shared_ptr<StorageProbe>* sp = (std::shared_ptr<StorageProbe>*)arg;
    std::shared_ptr<StorageProbe> p = *sp;
    delete sp;
#ifdef HOST_TEST
    {   // тесты: имитация подвисшего вызова
        FILE* hf = fopen("/tmp/ft/probe_hang", "r");
        if (hf) { fclose(hf); sleep(40); }
    }
#endif
    std::string out = p->mp + ": ";
    struct stat st;
    if (stat(p->mp.c_str(), &st) == 0) {
        char b[120];
        snprintf(b, sizeof(b), "PRESENT (mode %o, uid %d, gid %d)", (unsigned)(st.st_mode & 07777), (int)st.st_uid, (int)st.st_gid);
        out += b;
    } else {
        out += "ABSENT (errno " + std::to_string(errno) + " " + strerror(errno) + ")";
    }
    errno = 0;
    if (access(p->mp.c_str(), R_OK | X_OK) == 0) out += ", we can read it";
    else out += ", we CANNOT read it (errno " + std::to_string(errno) + " " + strerror(errno) + ")";
    DIR* dd = opendir(p->mp.c_str());
    if (!dd) {
        out += ", opendir failed (errno " + std::to_string(errno) + ")";
    } else {
        struct dirent* de;
        int n = 0;
        std::string names;
        while ((de = readdir(dd)) != NULL) {
            std::string nm = de->d_name;
            if (nm == "." || nm == "..") continue;
            if (n < 8) names += (names.empty() ? "" : " ") + nm;
            n++;
        }
        closedir(dd);
        out += ", " + std::to_string(n) + " entries" + (n ? " [" + names + (n > 8 ? " ..." : "") + "]" : "");
    }
    if (stat(p->root.c_str(), &st) == 0) out += "; torrents folder PRESENT";
    else out += "; torrents folder ABSENT (errno " + std::to_string(errno) + " " + strerror(errno) + ")";
#ifndef __linux__
    struct statfs sf;
    if (statfs(p->mp.c_str(), &sf) == 0) out += std::string("; filesystem ") + sf.f_fstypename + " from " + sf.f_mntfromname;
    else out += "; statfs failed (errno " + std::to_string(errno) + ")";
#endif
    p->text = out;
    p->done = 1;
    return NULL;
}

// Имена в каталоге (для списка точек монтирования и устройств).
static std::string dir_names(const char* dir, const char* const* prefixes, int np, int maxn)
{
    DIR* dd = opendir(dir);
    if (!dd) return std::string("cannot read, errno ") + std::to_string(errno);
    std::string out;
    struct dirent* de;
    int n = 0;
    while ((de = readdir(dd)) != NULL && n < maxn) {
        std::string nm = de->d_name;
        if (nm == "." || nm == "..") continue;
        bool take = (np == 0);
        for (int q = 0; q < np && !take; q++) take = nm.compare(0, strlen(prefixes[q]), prefixes[q]) == 0;
        if (!take) continue;
        out += (out.empty() ? "" : " ") + nm;
        n++;
    }
    closedir(dd);
    return out.empty() ? std::string("none") : out;
}

// Смонтирован ли USB-накопитель в этой точке (/mnt/usbN). Пустая точка монтирования без накопителя нам не нужна:
// отдельное монтирование отличается от родительского каталога номером устройства (st_dev); если же номер совпал,
// считаем накопитель смонтированным, только когда в каталоге что-то есть.
static bool usb_mount_available(const std::string& mp)
{
    struct stat st;
    if (stat(mp.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) return false;
#ifdef HOST_TEST
    return access((mp + ".unmounted").c_str(), F_OK) != 0;          // тесты: файл <точка>.unmounted значит "не смонтировано"
#else
    struct stat parent;
    if (stat(MNT_DIR, &parent) == 0 && st.st_dev != parent.st_dev) return true;
    DIR* d = opendir(mp.c_str());
    if (!d) return false;
    bool any = false;
    struct dirent* de;
    while ((de = readdir(d)) != NULL) {
        std::string nm = de->d_name;
        if (nm != "." && nm != "..") { any = true; break; }
    }
    closedir(d);
    return any;
#endif
}

// Можно ли писать на этот накопитель (не смонтирован ли он только для чтения).
static bool usb_mount_writable(const std::string& mp)
{
#ifdef HOST_TEST
    return access((mp + ".readonly").c_str(), F_OK) != 0;               // тесты: файл <точка>.readonly значит "только чтение"
#else
    return access(mp.c_str(), W_OK) == 0;
#endif
}

static bool is_internal_root(const std::string& r);
static uint64_t low_reserve(const std::string& root);

static void scan_usb(time_t now)
{
    std::set<std::string> present;

    // Места хранения: флешки /mnt/usbN/torrents и (если папка есть) внутренняя память консоли.
    std::vector<std::string> roots;
    for (int i = 0; i < SLOT_COUNT; i++) {
        char rootBuf[96];
        slot_mount(i, rootBuf, sizeof(rootBuf));
        strncat(rootBuf, "/torrents", sizeof(rootBuf) - strlen(rootBuf) - 1);
        if (is_dir(rootBuf)) roots.push_back(rootBuf);
    }
    if (is_dir(INTERNAL_ROOT)) roots.push_back(INTERNAL_ROOT);

    for (size_t ri = 0; ri < roots.size(); ri++) {
        const std::string root = roots[ri];
        present.insert(root);

        make_dirs(root + "/downloads");
        make_dirs(root + "/complete");
        make_dirs(root + "/.state");

        DIR* d = opendir(root.c_str());
        if (!d) continue;
        std::vector<std::string> names;
        struct dirent* e;
        while ((e = readdir(d)) != NULL) {
            std::string name = e->d_name;
            if (name.empty() || name[0] == '.' || !ends_with_ci(name, ".torrent")) continue;
            names.push_back(name);
        }
        closedir(d);
        std::sort(names.begin(), names.end());

        for (size_t k = 0; k < names.size(); k++) {
            std::string path = root + "/" + names[k];
            if (has_path(path)) continue;
            struct stat sb;
            if (stat(path.c_str(), &sb) != 0 || !S_ISREG(sb.st_mode)) continue;
            add_item(root, names[k]);
        }

        {   // Завершённые раздачи: .torrent в complete/. Самые свежие первыми, чтобы лимит памяти отсёк самые старые.
            const std::string cdir = root + "/complete";
            DIR* cd = opendir(cdir.c_str());
            if (cd) {
                std::vector<std::pair<time_t, std::string> > done;
                struct dirent* ce;
                while ((ce = readdir(cd)) != NULL) {
                    std::string name = ce->d_name;
                    if (name.empty() || name[0] == '.' || !ends_with_ci(name, ".torrent")) continue;
                    const std::string full = cdir + "/" + name;
                    if (has_path(full)) continue;
                    struct stat sb;
                    if (stat(full.c_str(), &sb) != 0 || !S_ISREG(sb.st_mode)) continue;
                    done.push_back(std::make_pair(sb.st_mtime, name));
                }
                closedir(cd);
                std::sort(done.begin(), done.end(), [](const std::pair<time_t, std::string>& a, const std::pair<time_t, std::string>& b) { return a.first > b.first; });
                for (size_t k = 0; k < done.size(); k++) add_item(root, done[k].second, true);
            }
        }
    }

    // Флешка пропала / вернулась; .torrent убрали руками.
    for (size_t k = g_items.size(); k > 0; k--) {
        Item* it = g_items[k - 1];
        if (it->complete) continue;

        if (present.count(it->root) == 0) {
            if (!it->offline) {
                it->offline = true;
                logf_("drive offline: %s (%s)", it->root.c_str(), it->title.c_str());
                stop_job_for(it, now);
            }
            continue;
        }

        struct stat sb;
        if (stat(it->path.c_str(), &sb) != 0) {
            logf_("torrent file removed, forgetting: %s", it->title.c_str());
            stop_job_for(it, now);
            g_items.erase(g_items.begin() + (k - 1));
            retire_item(it);
            continue;
        }
        if (it->offline) {
            it->offline = false;
            it->failed = false;
            it->retryAt = 0;
            logf_("drive back online: %s (%s)", it->root.c_str(), it->title.c_str());
        }
    }

    // Какая-то раздача ждёт пропавшее хранилище: раз в 30 секунд проверяем его "глазами нашего процесса" и пишем в лог,
    // что именно мы видим (есть ли точка монтирования и устройство, какой ошибкой отвечает система, кто мы такие).
    // Если по FTP флешка видна, а здесь нет, по этим строкам будет понятно, чем отличается наш процесс.
    {
        static time_t lastDiag = 0, offlineSince = 0, probeStarted = 0;
        static int diagN = 0;
        static std::vector<std::shared_ptr<StorageProbe> > pend;
        int waiting = 0;
        std::set<std::string> missing;
        for (size_t k = 0; k < g_items.size(); k++)
            if (!g_items[k]->complete && g_items[k]->offline) { waiting++; missing.insert(g_items[k]->root); }
        if (waiting == 0) {
            offlineSince = 0;
            lastDiag = 0;
            diagN = 0;
            pend.clear();
        } else {
            if (offlineSince == 0) offlineSince = now;
            if (!pend.empty()) {
                bool all = true;
                for (size_t i = 0; i < pend.size(); i++) if (!pend[i]->done.load()) all = false;
                if (all || now - probeStarted >= 8) {
                    std::string probes, which;
                    for (size_t i = 0; i < pend.size(); i++)
                        probes += (probes.empty() ? "" : " ; ") + (pend[i]->done.load() ? pend[i]->text : pend[i]->mp + ": NO ANSWER after " + std::to_string((long)(now - probeStarted)) + " s (a file system call is HUNG on this path)");
                    for (std::set<std::string>::const_iterator m = missing.begin(); m != missing.end(); ++m) which += (which.empty() ? "" : ", ") + *m;
                    static const char* const devPrefixes[] = {"da", "usb", "ugen"};
                    logf_("storage check #%d: %d task(s) wait for %s (offline for %ld s) | %s | %s: [%s] | devices: [%s] | process: %s", ++diagN, waiting,
                          which.c_str(), (long)(now - offlineSince), probes.c_str(), MNT_DIR, dir_names(MNT_DIR, NULL, 0, 24).c_str(),
                          dir_names(DEV_DIR, devPrefixes, 3, 24).c_str(), process_identity().c_str());
                    pend.clear();
                    lastDiag = now;
                }
            } else if (lastDiag == 0 || now - lastDiag >= 30) {
                std::set<std::string> mps;
                for (std::set<std::string>::const_iterator m = missing.begin(); m != missing.end(); ++m) {
                    size_t sl = m->rfind('/');
                    mps.insert(sl == std::string::npos ? *m : m->substr(0, sl));            // /mnt/usb0/torrents -> /mnt/usb0
                }
                for (std::set<std::string>::const_iterator m = mps.begin(); m != mps.end(); ++m) {
                    std::shared_ptr<StorageProbe> sp(new StorageProbe());
                    sp->mp = *m;
                    sp->root = *m + "/torrents";
                    std::shared_ptr<StorageProbe>* arg = new std::shared_ptr<StorageProbe>(sp);
                    pthread_t th;
                    if (pthread_create(&th, NULL, probe_thread, arg) == 0) { pthread_detach(th); pend.push_back(sp); }
                    else delete arg;
                }
                probeStarted = now;
            }
        }
    }

    // Список мест хранения и свободное место: узнаём здесь (раз в 10 секунд), а не при каждом обновлении состояния.
    std::string dj;
    g_freeMap.clear();
    std::vector<std::string> order;
    for (int d = 0; d < SLOT_COUNT; d++) {
        char rootBuf[96];
        slot_mount(d, rootBuf, sizeof(rootBuf));
        strncat(rootBuf, "/torrents", sizeof(rootBuf) - strlen(rootBuf) - 1);
        if (present.count(rootBuf)) order.push_back(rootBuf);
    }
    if (present.count(INTERNAL_ROOT)) order.push_back(INTERNAL_ROOT);
    for (size_t k = 0; k < order.size(); k++) {
        const std::string& rootBuf = order[k];
        struct statvfs sv;
        unsigned long long freeB = 0, totalB = 0;
        if (statvfs(rootBuf.c_str(), &sv) == 0) {
            freeB = (unsigned long long)sv.f_bavail * sv.f_frsize;
            totalB = (unsigned long long)sv.f_blocks * sv.f_frsize;
        }
#ifdef HOST_TEST
        {   // тесты: свободное место из файла (в байтах)
            FILE* ff = fopen("/tmp/ft/free_mock", "r");
            unsigned long long m = 0;
            if (ff) { if (fscanf(ff, "%llu", &m) == 1) freeB = m; fclose(ff); }
        }
#endif
        g_freeMap[rootBuf] = freeB;
        char buf[360];
        snprintf(buf, sizeof(buf), "%s{\"root\":\"%s\",\"free\":%llu,\"total\":%llu,\"kind\":\"%s\",\"low\":%s}", dj.empty() ? "" : ",",
                 rootBuf.c_str(), freeB, totalB, is_internal_root(rootBuf) ? "internal" : (rootBuf.compare(0, strlen(EXT_BASE), EXT_BASE) == 0 ? "ext" : "usb"), freeB < low_reserve(rootBuf) ? "true" : "false");
        dj += buf;
    }
    g_drivesJson = dj;

    // Все смонтированные USB-накопители, в том числе без папки torrents (она создаётся при добавлении торрента на такой
    // накопитель). Нужны странице для выбора места.
    std::string mj;
    for (int d = 0; d < SLOT_COUNT; d++) {
        char mpb[96];
        slot_mount(d, mpb, sizeof(mpb));
        if (!usb_mount_available(mpb)) continue;
        struct statvfs sv;
        unsigned long long freeB = 0, totalB = 0;
        if (statvfs(mpb, &sv) == 0) {
            freeB = (unsigned long long)sv.f_bavail * sv.f_frsize;
            totalB = (unsigned long long)sv.f_blocks * sv.f_frsize;
        }
#ifdef HOST_TEST
        {
            FILE* ff = fopen("/tmp/ft/free_mock", "r");
            unsigned long long m = 0;
            if (ff) { if (fscanf(ff, "%llu", &m) == 1) freeB = m; fclose(ff); }
        }
#endif
        char buf[300];
        snprintf(buf, sizeof(buf), "%s{\"n\":%d,\"kind\":\"%s\",\"mount\":\"%s\",\"has_torrents\":%s,\"writable\":%s,\"free\":%llu,\"total\":%llu,\"low\":%s}", mj.empty() ? "" : ",", d, slot_is_ext(d) ? "ext" : "usb", mpb,
                 is_dir(std::string(mpb) + "/torrents") ? "true" : "false", usb_mount_writable(mpb) ? "true" : "false", freeB, totalB, freeB < LOW_FREE_USB_BYTES ? "true" : "false");
        mj += buf;
    }
    g_mountsJson = mj;
}

// ---------------------------------------------------------------- HTTP

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static std::string g_json = "{\"items\":[],\"drives\":[]}";
static std::string g_filesReply;                 // ответ на /api/files (готовит главный поток, под g_mu)
static std::string g_cfg = "{}";
static unsigned g_cmdSeq = 0;                      // сколько команд поставлено в очередь (HTTP-потоки)
static unsigned g_cmdTaken = 0;                    // сколько команд забрал главный цикл (только главный поток)
static unsigned g_cmdDone = 0;                     // до какой команды состояние уже обновлено и опубликовано
static std::vector<std::pair<std::string, std::string> > g_cmds;     // (команда, хэш)
static std::atomic<int> g_srv(-1);                // слушающий сокет веб-сервера (пересоздаётся при смене сети)
static std::atomic<unsigned> g_httpConns(0);       // сколько соединений принято (для лога)
static std::atomic<int> g_rebindReq(0);            // 1: главный поток просит поток сервера пересоздать слушающий сокет
static std::atomic<int> g_rebindDone(0);           // ответ потока сервера: 1 получилось, -1 нет
static std::atomic<int> g_rebindErr(0);
static std::atomic<int> g_httpHeals(0);           // сколько раз поток сервера сам пересоздал сломанный сокет
static pthread_t g_srvThread;
static bool g_srvStarted = false;

static std::string json_escape(const std::string& s)
{
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"') o += "\\\"";
        else if (c == '\\') o += "\\\\";
        else if (c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
        else o += (char)c;
    }
    return o;
}

static const char* status_name(const Item& it)
{
    if (it.complete) return "complete";
    if (it.offline) return "offline";
    if (it.running) {
        // идёт проверка, если процент проверки не 100 и ещё нет соединений
        return it.checkPct < 100 && it.speedKB == 0 && it.peers == 0 ? "checking" : "downloading";
    }
    if (it.paused) return "paused";
    if (it.failed) return "waiting";
    return "queued";
}

static void build_snapshot()
{
    std::string j = "{\"items\":[";
    int totalSpeed = 0;
    for (size_t i = 0; i < g_items.size(); i++) {
        Item& it = *g_items[i];
        totalSpeed += it.speedKB;
        uint64_t done = item_done_bytes(it);
        int pct = item_size_bytes(it) ? (int)(done * 100 / item_size_bytes(it)) : 0;

        // Оценка оставшегося времени: остаток делим на реальную среднюю скорость за последние до 60 секунд
        // (мгновенная скорость прыгает, а среднее с нуля медленно разгоняется). Снимок строится раз в секунду.
        // Раньше 10 секунд измерений время не показываем.
        time_t nowT = time(NULL);
        if (it.running) {
            if (!it.rate.empty() && it.sessBytes < it.rate.back().second) it.rate.clear();   // загрузка началась заново
            if (it.rate.empty() || it.rate.back().first != nowT) it.rate.push_back(std::make_pair(nowT, it.sessBytes));
            while (it.rate.size() > 2 && nowT - it.rate.front().first > 60) it.rate.erase(it.rate.begin());
        } else {
            it.rate.clear();
        }
        double avgKB = 0;
        if (it.rate.size() >= 2) {
            double dt = (double)(it.rate.back().first - it.rate.front().first);
            if (dt >= 10.0) avgKB = (double)(it.rate.back().second - it.rate.front().second) / 1024.0 / dt;
        }
        long long eta = -1;                                   // -1: неизвестно
        if (it.complete) {
            eta = 0;
        } else if (it.running && avgKB >= 1.0 && item_size_bytes(it) > done) {
            double secs = (double)(item_size_bytes(it) - done) / (avgKB * 1024.0);
            eta = secs > 8640000.0 ? 8640000LL : (long long)secs;   // не больше 100 суток
        }

        char buf[480];
        snprintf(buf, sizeof(buf),
                 "%s{\"hash\":\"%s\",\"status\":\"%s\",\"size\":%llu,\"done\":%llu,\"pct\":%d,"
                 "\"speed_kb\":%d,\"eta_s\":%lld,\"peers\":%d,\"paused\":%s,\"low_space\":%s,\"nfiles\":%u,\"nskip\":%u,\"root\":\"",
                 i ? "," : "", it.t.infoHashHex.c_str(), status_name(it),
                 (unsigned long long)item_size_bytes(it), (unsigned long long)done, pct,
                 it.speedKB, eta, it.peers, it.paused ? "true" : "false", item_low_space(it) ? "true" : "false", (unsigned)it.t.files.size(), item_skipped_files(it));
        j += buf;
        j += json_escape(it.root) + "\",\"title\":\"" + json_escape(it.title) + "\"}";
    }
    j += "],\"drives\":[";

    j += g_drivesJson;
    j += "],\"mounts\":[";
    j += g_mountsJson;
    char tail[320];
    snprintf(tail, sizeof(tail), "],\"speed_kb\":%d,\"max_parallel\":%d,\"listen_port\":%d,\"incoming\":%u,"
             "\"deleting\":%d,\"net_ip\":\"%s\",\"net_restarts\":%d,\"net_heals\":%d,\"buf_kb\":%llu,\"version\":\"%s\"}", totalSpeed, g_maxParallel,
             incoming_port(), incoming_accepted(), (int)g_delJobs.size(), g_netIp.c_str(), g_netRestarts, g_httpHeals.load(), (unsigned long long)(bigbuf_total().load() >> 10), APP_VERSION);
    j += tail;
#ifdef HOST_TEST
    {   // тесты: сколько памяти занято в обычной куче (на консоли куча ~10 МБ)
        struct mallinfo2 mi = mallinfo2();
        if (!j.empty() && j[j.size() - 1] == '}') { j.erase(j.size() - 1); j += ",\"heap_kb\":" + std::to_string((unsigned long long)((mi.uordblks + mi.hblkhd) >> 10)) + "}"; }
    }
#endif

    char cfgBuf[300];
    snprintf(cfgBuf, sizeof(cfgBuf), "{\"max_parallel\":%d,\"listen_port\":%d,\"listening\":%s,\"token_set\":%s,\"autostart\":%s,\"save_to\":\"%s\",\"resume_session\":%s,\"http_port\":%d,\"version\":\"%s\"}",
             g_maxParallel, g_listenPort, incoming_port() ? "true" : "false", g_token.empty() ? "false" : "true",
             g_autostart ? "true" : "false", g_saveTo.load() == 1 ? "internal" : "usb", g_resumeSession.load() ? "true" : "false", HTTP_PORT, APP_VERSION);
    std::string cfg = cfgBuf;

    pthread_mutex_lock(&g_mu);
    g_json = j;
    g_cfg = cfg;
    pthread_mutex_unlock(&g_mu);
}

static const char* PAGE = R"HTMLPAGE(<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<meta name="color-scheme" content="dark"><meta name="theme-color" content="#10151c">
<title>ps4torrent</title><link rel="icon" href="/favicon.ico"><style>
:root{color-scheme:dark;--bg:#10151c;--fg:#e6edf3;--mut:#8b98a8;--card:#18202a;--ac:#4aa3ff;--ok:#3fb950;--wr:#d29922;--er:#f85149}
body{margin:0;background:var(--bg);color:var(--fg);font:15px system-ui,sans-serif}
header{padding:14px 18px;display:flex;gap:12px;flex-wrap:wrap;align-items:center}
h1{font-size:18px;margin:0}.mut{color:var(--mut)}
main{padding:0 14px 24px;display:grid;gap:10px}
.c{background:var(--card);border-radius:10px;padding:12px 14px}
.t{font-weight:600;word-break:break-word}.r{display:flex;gap:10px;align-items:center;margin-top:6px;flex-wrap:wrap}
.bar{flex:1;min-width:140px;height:8px;background:rgba(127,127,127,.25);border-radius:4px;overflow:hidden}
.bar i{display:block;height:100%;background:var(--ac)}.complete .bar i{background:var(--ok)}
button,.btn{background:transparent;color:var(--ac);border:1px solid var(--ac);border-radius:6px;padding:3px 10px;cursor:pointer;font:inherit;font-size:13px}
.dng{color:var(--er);border-color:var(--er)}
.s{font-size:12px;padding:1px 7px;border-radius:9px;border:1px solid var(--mut);color:var(--mut)}
.offline .s,.waiting .s{color:var(--wr);border-color:var(--wr)}
.warn{margin:0 14px 10px;padding:8px 12px;border-radius:8px;background:rgba(248,81,73,.14);color:var(--er);display:none;font-size:14px}
.ov{position:fixed;left:0;top:0;right:0;bottom:0;background:rgba(0,0,0,.6);display:none;align-items:center;justify-content:center;z-index:10}
.dl{max-width:540px;width:92%;max-height:86vh;overflow:auto;box-sizing:border-box}
.dl h2{margin:0 0 10px;font-size:17px}
select,select option{background:var(--card);color:var(--fg)}
select{max-width:100%;border:1px solid var(--mut);border-radius:6px;padding:4px 6px;font:inherit}
option:checked,option:hover{background:#25344a;color:var(--fg)}
.dl .f{padding:3px 0;word-break:break-word}.dl .row{margin-top:12px;display:flex;gap:10px;flex-wrap:wrap}
.logo{display:inline-flex;align-items:center;background:#10151c;border-radius:10px;padding:6px 14px;text-decoration:none}
.logo img{height:36px;width:auto;display:block}
.fl{max-height:48vh;overflow:auto;margin:10px 0;border:1px solid #25344a;border-radius:8px;padding:4px 8px}
.fl .h{padding:8px 0 2px;border-top:1px solid #25344a}.fl .h:first-child{border-top:0}
.fl label{display:flex;gap:8px;align-items:flex-start;padding:3px 0;word-break:break-word}
.fl input{width:auto;margin-top:3px}.fl .sz{margin-left:auto;white-space:nowrap;padding-left:10px}
footer{padding:16px 14px 22px;text-align:center;color:var(--mut);font-size:12px}
button:disabled{opacity:.55;cursor:default}
#msg{margin:0 14px 10px;padding:8px 12px;border-radius:8px;background:var(--card);display:none}
#cfg{margin:0 14px 10px;display:none}
#cfg label{display:inline-flex;gap:6px;align-items:center;margin-right:14px}
#cfg input{width:70px;background:transparent;color:var(--fg);border:1px solid var(--mut);border-radius:6px;padding:3px 6px}
</style></head><body>
<header><a class="logo" href="/"><img src="/logo.png" alt="ps4torrent"></a><span class="mut" id="sum"></span>
<label class="btn">+ .torrent<input id="up" type="file" accept=".torrent" multiple hidden></label>
<button id="allgo">start all</button><button id="allstop">pause all</button>
<button id="cfgb">settings</button></header>
<div id="warn" class="warn"></div>
<div id="msg"></div>
<div id="cfg" class="c">
<label>parallel downloads <input id="mp" type="number" min="1" max="25"></label>
<label>listen port (0 = off) <input id="lp" type="number" min="0" max="65535"></label>
<label><input id="as" type="checkbox" style="width:auto"> start tasks automatically</label>
<label><input id="rs" type="checkbox" style="width:auto"> resume interrupted downloads after a restart</label>
<button id="cfgs">save</button> <span class="mut" id="cfgi"></span></div>
<div id="dlg" class="ov"><div class="dl c">
<h2>Add torrent</h2>
<div id="dfiles"></div>
<div style="margin-top:12px"><label>Save to <select id="dsel"></select></label></div>
<div id="dinfo" class="mut" style="margin-top:6px"></div>
<div id="dwarn" class="warn" style="margin:8px 0 0"></div>
<div style="margin-top:10px"><label><input id="dstart" type="checkbox" checked> Start downloading right away</label></div>
<div class="row"><button id="dcancel">Cancel</button><button id="dgo">Download</button></div>
</div></div>
<div id="dlg2" class="ov"><div class="dl c">
<h2 id="f2title">Select files to download</h2>
<div id="f2info" class="mut"></div>
<div id="f2warn" class="warn" style="margin:8px 0 0"></div>
<div id="f2list" class="fl"></div>
<div class="row"><button id="f2all">Select all</button><button id="f2none">Deselect all</button><button id="f2go">Start download</button><button id="f2cancel">Cancel</button></div>
</div></div>
<main id="m"><div class="c">loading...</div></main>
<footer id="ft">ps4torrent v1.0 Created by SergioPoverony and Mr.Claude</footer>
<script>
var TK = '';
try { TK = sessionStorage.getItem('tk') || ''; } catch (e) {}
var hm = location.hash.match(/token=([^&]*)/);
if (hm) { TK = decodeURIComponent(hm[1]); try { sessionStorage.setItem('tk', TK); } catch (e) {} }

function sz(n){var u=['B','KB','MB','GB','TB'],i=0;while(n>=1024&&i<4){n/=1024;i++}return n.toFixed(i?1:0)+' '+u[i]}
function tm(s){if(s<0)return'-';if(s<60)return'<1 min';var m=Math.round(s/60);if(m<60)return m+' min';var h=Math.floor(m/60);m=m%60;if(h<48)return h+' h '+m+' min';return Math.floor(h/24)+' d '+(h%24)+' h'}
function E(s){return String(s).replace(/[&<>"]/g,function(c){return{'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]})}
function $(id){return document.getElementById(id)}

var msgTimer = 0;
function say(text, bad){
  var m = $('msg'); m.textContent = text; m.style.display = 'block'; m.style.color = bad ? 'var(--er)' : 'var(--fg)';
  clearTimeout(msgTimer); msgTimer = setTimeout(function(){ m.style.display = 'none'; }, 7000);
}

// Все запросы к API идут через api(): добавляет пароль, при ответе 401 один раз спрашивает его.
function api(path, opts, retried){
  var url = path + (path.indexOf('?') >= 0 ? '&' : '?') + 'token=' + encodeURIComponent(TK);
  return fetch(url, opts || {}).then(function(r){
    if (r.status === 401 && !retried) {
      var t = prompt('Access token:');
      if (t === null) return r;
      TK = t; try { sessionStorage.setItem('tk', TK); } catch (e) {}
      return api(path, opts, true);
    }
    return r;
  });
}

// ---- Кнопки. Список перерисовывается целиком (innerHTML), и кнопка, на которую нажали, может быть заменена новой.
// Поэтому состояние "действие выполняется" хранится не на кнопке, а здесь, и учитывается при каждой отрисовке.
var pending = {};        // выполняющиеся действия: ключ "команда:хэш"
var busy = 0;            // сколько действий в работе; пока они есть, список не перерисовывается сам
var holdUntil = 0;       // до этого момента (мс) список не перерисовывается: палец или мышь на кнопке
var lastHtml = '';       // что сейчас показано в списке (если не изменилось, DOM не трогаем)
var lastData = null;     // последнее состояние, полученное от консоли
var EMPTY = '<div class="c">No torrents. Put .torrent files into /mnt/usbN/torrents/ or use "+ .torrent"</div>';

function btn(c, h, label, cls, title){
  var p = pending[c + ':' + h];
  return '<button' + (cls ? ' class="' + cls + '"' : '') + ' data-c="' + c + '" data-h="' + h + '"' +
    (title !== undefined ? ' data-t="' + E(title) + '"' : '') + (p ? ' disabled' : '') + '>' + (p ? '...' : label) + '</button>';
}

function row(i){
  var b = '';
  if (i.status === 'paused') b += btn('resume', i.hash, 'resume');
  else if (i.status !== 'complete' && i.status !== 'offline') b += btn('pause', i.hash, 'pause');
  if (i.nfiles > 1 && i.status !== 'complete') b += btn('files', i.hash, 'files');
  b += btn('delete', i.hash, 'remove', 'dng', i.title);
  b += btn('delete_files', i.hash, 'remove + files', 'dng', i.title);
  var eta = i.status === 'downloading' ? ', ETA ' + tm(i.eta_s) : '';
  var fsel = i.nskip ? '<span class="s">' + (i.nfiles - i.nskip) + '/' + i.nfiles + ' files</span>' : '';
  var low = i.low_space ? '<span class="s" style="color:var(--er);border-color:var(--er)">low space</span>' : '';
  return '<div class="c ' + i.status + '"><div class="t">' + E(i.title) + '</div><div class="r">'
    + '<span class="s">' + i.status + '</span>' + fsel + low + '<div class="bar"><i style="width:' + i.pct + '%"></i></div>'
    + '<span>' + i.pct + '% of ' + sz(i.size) + '</span>'
    + '<span class="mut">' + i.speed_kb + ' KB/s, ' + i.peers + ' peers' + eta + '</span>' + b + '</div></div>';
}

function paint(){
  if (!lastData) return;
  var html = lastData.items.map(row).join('') || EMPTY;
  if (html !== lastHtml) { $('m').innerHTML = html; lastHtml = html; }
}

function driveName(x){ return x.kind === 'internal' ? 'console memory' : x.root.replace('/torrents', ''); }

function warnings(d){
  var w = [], off = {}, cnt = 0;
  d.items.forEach(function(i){ if (i.status === 'offline') { off[i.root] = (off[i.root] || 0) + 1; cnt++; } });
  Object.keys(off).forEach(function(r){
    w.push('Storage not found: ' + (r.indexOf('/data/') === 0 ? 'console memory' : r.replace('/torrents', '')) + ' (' + off[r] + ' task(s) waiting). They continue by themselves when it returns.');
  });
  d.drives.forEach(function(x){ if (x.low) w.push('Low free space on ' + driveName(x) + ': only ' + sz(x.free) + ' left'); });
  d.items.forEach(function(i){
    if (!i.low_space) return;
    var left = Math.max(0, i.size - i.done), dr = d.drives.filter(function(x){ return x.root === i.root; })[0];
    w.push('"' + i.title + '" still needs about ' + sz(left) + (dr ? ', but only ' + sz(dr.free) + ' is free' : ''));
  });
  var off = d.items.filter(function(i){ return i.status === 'offline'; }).length;
  if (off) w.push(off + ' task(s) wait for the drive. After console sleep it can take a few minutes to come back; if it does not, re-plug it. The tasks continue by themselves');
  return w;
}

// Скорость приходит от консоли уже усреднённой, но данные от пиров идут пачками, и цифра всё равно скачет. Здесь добавляется
// ещё одно сглаживание (по 25% новых данных на каждое обновление раз в 2 секунды). Время и остаток считает консоль, их это не касается.
var sm = {};
function smooth(key, v, running){
  if (!running) { delete sm[key]; return v; }
  var s = (sm[key] === undefined) ? v : sm[key] * 0.75 + v * 0.25;
  sm[key] = s;
  return Math.round(s);
}

function load(force){
  if (!force && (busy > 0 || Date.now() < holdUntil)) return;      // идёт действие или на кнопке палец: не мешаем
  api('/status').then(function(r){ return r.json(); })
  .then(function(d){
    lastData = d;
    d.items.forEach(function(i){ i.speed_kb = smooth(i.hash, i.speed_kb, i.status === 'downloading'); });
    d.speed_kb = smooth('_all', d.speed_kb, d.speed_kb > 0);
    var dr = d.drives.map(function(x){ return driveName(x) + ' free ' + sz(x.free); }).join(' | ')
      || (d.items.length ? 'USB DRIVE NOT FOUND: tasks continue by themselves when it returns (check or re-plug the drive)' : 'no storage with a torrents folder');
    $('sum').textContent = d.speed_kb + ' KB/s | ' + dr + ' | incoming: ' + (d.listen_port ? d.incoming + ' peers (port ' + d.listen_port + ')' : 'off')
      + (d.deleting ? ' | deleting files...' : '');
    $('ft').textContent = 'ps4torrent v' + d.version + ' Created by SergioPoverony and Mr.Claude';
    var w = warnings(d), wb = $('warn');
    wb.style.display = w.length ? 'block' : 'none';
    wb.textContent = w.join(' | ');
    if (busy > 0 || (!force && Date.now() < holdUntil)) return;     // пока ждали ответ, началось действие: список не трогаем
    paint();
  }).catch(function(){ $('sum').textContent = 'no connection'; });
}

function withTimeout(p, ms){
  return new Promise(function(res, rej){
    var t = setTimeout(function(){ rej(new Error('timeout')); }, ms);
    p.then(function(v){ clearTimeout(t); res(v); }, function(e){ clearTimeout(t); rej(e); });
  });
}

function act(c, h, t){
  if (c === 'files') { openTaskFiles(h); return; }
  var k = c + ':' + h, q;
  if (pending[k]) return;                                           // уже выполняется: повторное нажатие игнорируем
  if (c === 'delete') {
    if (!confirm('Remove from the list? Downloaded files stay on the drive.\n\n' + t)) return;
    q = '/api/delete?hash=' + h;
  } else if (c === 'delete_files') {
    if (!confirm('Remove AND delete the downloaded files from the drive?\n\n' + t)) return;
    q = '/api/delete?files=1&hash=' + h;
  } else q = '/api/' + c + '?hash=' + h;

  pending[k] = true; busy++;
  paint();                                                          // кнопка сразу показывает "..."
  var finish = function(){ delete pending[k]; busy--; load(true); };  // один раз перерисовываем актуальное состояние
  withTimeout(api(q).then(function(r){ return r.json(); }), 10000)
    .then(function(d){ if (!d.ok) say(d.error || 'failed', true); finish(); })
    .catch(function(){ say('No answer from the console', true); finish(); });
}

$('m').onclick = function(e){
  var b = e.target;
  if (!b || !b.dataset || !b.dataset.c || b.disabled) return;
  act(b.dataset.c, b.dataset.h, b.dataset.t);
};
// Нажатие уже началось (но ещё не завершилось кликом): не заменяем кнопки под пальцем.
$('m').onmousedown = $('m').ontouchstart = function(){ holdUntil = Date.now() + 700; };

// ---- Добавление торрента: выбор файла -> диалог (место сохранения) -> отправка.
var dlgFiles = [];

// Минимальный разбор .torrent (bencode): достаём имя и общий размер, чтобы заранее проверить свободное место.
function latin(u8){ var s = ''; for (var i = 0; i < u8.length; i++) s += String.fromCharCode(u8[i]); return s; }
function bdecode(b){
  var p = 0;
  function num(end){ var s = ''; while (b[p] !== end) { if (p >= b.length) throw new Error('eof'); s += String.fromCharCode(b[p++]); } p++; return s; }
  function val(depth){
    if (depth > 24 || p >= b.length) throw new Error('bad');
    var c = b[p];
    if (c === 105) { p++; return parseInt(num(101), 10); }
    if (c === 108) { p++; var l = []; while (b[p] !== 101) { if (p >= b.length) throw new Error('eof'); l.push(val(depth + 1)); } p++; return l; }
    if (c === 100) { p++; var d = {}; while (b[p] !== 101) { if (p >= b.length) throw new Error('eof'); var k = val(depth + 1); d[latin(k.b)] = val(depth + 1); } p++; return d; }
    if (c >= 48 && c <= 57) { var n = parseInt(num(58), 10); if (p + n > b.length) throw new Error('eof'); var s = b.subarray(p, p + n); p += n; return { b: s }; }
    throw new Error('bad');
  }
  return val(0);
}
function torrentInfo(buf){
  try {
    var t = bdecode(new Uint8Array(buf)), info = t.info;
    if (!info || !info.name || !info.pieces) return null;
    var dec = function(u){ return (typeof TextDecoder !== 'undefined') ? new TextDecoder('utf-8').decode(u) : latin(u); };
    var name = dec(info.name.b), files = [], size = 0;
    if (typeof info.length === 'number') { size = info.length; files.push({ path: name, size: info.length }); }
    else if (info.files && info.files.length) info.files.forEach(function(f){
      var len = (typeof f.length === 'number') ? f.length : 0;
      files.push({ path: (f.path || []).map(function(p){ return dec(p.b); }).join('/'), size: len });
      size += len;
    });
    else return null;
    return { name: name, size: size, files: files };
  } catch (e) { return null; }
}

function destOptions(){
  var opts = [{ v: 'internal', label: 'Console memory (system disk)', free: null, reserve: 5 * 1073741824 }];
  var d = lastData || {};
  (d.drives || []).forEach(function(x){ if (x.kind === 'internal') opts[0].free = x.free; });
  if (d.mounts) {
    d.mounts.forEach(function(m){
      opts.push({ v: String(m.n), label: (m.kind === 'ext' ? 'Extended disk ' : 'USB drive ') + m.mount + (m.writable === false ? ' (read-only: cannot be used)' : ''), free: m.free, reserve: 2 * 1073741824 });
    });
  } else {
    (d.drives || []).forEach(function(x){
      var m = x.root.match(/usb(\d+)\/torrents$/);
      if (x.kind !== 'internal' && m) opts.push({ v: m[1], label: 'USB drive ' + x.root.replace('/torrents', ''), free: x.free, reserve: 2 * 1073741824 });
      var e = x.root.match(/ext(\d+)\/torrents$/);
      if (x.kind === 'ext' && e) opts.push({ v: String(8 + parseInt(e[1], 10)), label: 'Extended disk ' + x.root.replace('/torrents', ''), free: x.free, reserve: 2 * 1073741824 });
    });
  }
  opts.forEach(function(o){ if (o.free !== null) o.label += ' - ' + sz(o.free) + ' free'; });
  return opts;
}

// Предупреждение о месте: нужно total байт на выбранном месте (o).
function spaceWarning(o, total){
  if (!o || o.free === null || !total) return '';
  if (total > o.free) return 'Not enough free space: needs about ' + sz(total) + ', only ' + sz(o.free) + ' is free here.';
  if (o.free - total < o.reserve) return 'Low free space: only ' + sz(o.free - total) + ' will be left after the download.';
  return '';
}
function destByValue(v){ var opts = destOptions(); return opts.filter(function(x){ return x.v === v; })[0] || opts[0]; }

function anyMulti(){ return dlgFiles.some(function(f){ return f.info && f.info.files.length > 1; }); }

function updateDlg(){
  var o = destByValue($('dsel').value), total = 0, good = 0;
  dlgFiles.forEach(function(f){ if (f.info) { total += f.info.size; good++; } });
  $('dinfo').textContent = good ? 'Total size: ' + sz(total) + (o.free !== null ? ', free: ' + sz(o.free) : '') : '';
  var w = $('dwarn'), msg = good ? spaceWarning(o, total) : '';
  w.style.display = msg ? 'block' : 'none';
  w.textContent = msg;
  $('dgo').disabled = good === 0;
  $('dgo').textContent = anyMulti() ? 'Next' : 'Download';
}

function openDialog(files){
  dlgFiles = files;
  $('dfiles').innerHTML = files.map(function(f){
    return '<div class="f">' + (f.info ? E(f.info.name) + ' <span class="mut">(' + sz(f.info.size) + (f.info.files.length > 1 ? ', ' + f.info.files.length + ' files' : '') + ')</span>'
      : E(f.name) + ' <span style="color:var(--er)">- not a valid .torrent</span>') + '</div>';
  }).join('');
  $('dsel').innerHTML = destOptions().map(function(o){ return '<option value="' + o.v + '">' + E(o.label) + '</option>'; }).join('');
  $('dsel').value = 'internal';                                    // по умолчанию: система консоли
  $('dstart').checked = true;
  updateDlg();
  $('dlg').style.display = 'flex';
}
function closeDialog(){ $('dlg').style.display = 'none'; $('dlg2').style.display = 'none'; dlgFiles = []; f2 = null; }

// ---- Окно 2: выбор файлов (при добавлении нескольких файлов и для уже добавленной задачи)
var f2 = null;           // { mode: 'add' | 'edit', hash, groups: [{ title, files: [{path,size}], chk: [bool], src }], drive, start, label }
function skipText(chk){  // номера снятых файлов: "3,5-9"
  var out = [], k = 0;
  while (k < chk.length) {
    if (chk[k]) { k++; continue; }
    var e = k;
    while (e + 1 < chk.length && !chk[e + 1]) e++;
    out.push(e > k ? k + '-' + e : String(k));
    k = e + 1;
  }
  return out.join(',');
}
function f2Totals(){
  var t = { size: 0, cnt: 0, all: 0, bad: 0 };
  f2.groups.forEach(function(g){
    var n = 0;
    g.files.forEach(function(x, i){ t.all++; if (g.chk[i]) { t.size += x.size; t.cnt++; n++; } });
    if (!n) t.bad++;
  });
  return t;
}
function refresh2(){
  var t = f2Totals(), msg = '';
  $('f2info').textContent = 'Selected: ' + t.cnt + ' of ' + t.all + ' files, ' + sz(t.size);
  if (t.bad) msg = f2.groups.length > 1 ? 'Select at least one file in every torrent.' : 'Select at least one file.';
  else if (f2.mode === 'add') msg = spaceWarning(destByValue(f2.drive), t.size);
  $('f2warn').style.display = msg ? 'block' : 'none';
  $('f2warn').textContent = msg;
  $('f2go').disabled = t.bad > 0 || t.cnt === 0;
}
function renderFiles2(){
  var html = '';
  f2.groups.forEach(function(g, gi){
    if (f2.groups.length > 1) {
      var n = 0, tot = 0;
      g.files.forEach(function(x, i){ if (g.chk[i]) n++; tot += x.size; });
      html += '<div class="h"><label><input type="checkbox" data-ga="' + gi + '"' + (n === g.files.length ? ' checked' : '') + '><b>' + E(g.title) + '</b> <span class="mut">(' + g.files.length + ' files, ' + sz(tot) + ')</span></label></div>';
    }
    g.files.forEach(function(x, i){
      html += '<label><input type="checkbox" data-g="' + gi + '" data-f="' + i + '"' + (g.chk[i] ? ' checked' : '') + '><span>' + E(x.path) + '</span><span class="sz mut">' + sz(x.size) + '</span></label>';
    });
  });
  $('f2list').innerHTML = html;
  refresh2();
}
function setAll2(v){ f2.groups.forEach(function(g){ g.chk = g.chk.map(function(){ return v; }); }); renderFiles2(); }
$('f2list').onchange = function(e){
  var t = e.target;
  if (!t || !t.dataset || !f2) return;
  if (t.dataset.ga !== undefined) { var g = f2.groups[+t.dataset.ga]; g.chk = g.chk.map(function(){ return !!t.checked; }); renderFiles2(); }
  else if (t.dataset.g !== undefined) { f2.groups[+t.dataset.g].chk[+t.dataset.f] = !!t.checked; refresh2(); }
};
$('f2all').onclick = function(){ if (f2) setAll2(true); };
$('f2none').onclick = function(){ if (f2) setAll2(false); };
$('f2cancel').onclick = closeDialog;

function openFiles2(groups, mode, extra){
  f2 = { mode: mode, groups: groups, hash: extra.hash || '', drive: extra.drive || '', start: extra.start, label: extra.label || '' };
  $('dlg').style.display = 'none';
  $('f2title').textContent = mode === 'add' ? 'Select files to download' : 'Files: ' + (extra.title || '');
  $('f2go').textContent = mode === 'add' ? 'Start download' : 'Apply';
  renderFiles2();
  $('dlg2').style.display = 'flex';
}

// Файлы уже добавленной задачи (кнопка Files): список приходит от консоли
function openTaskFiles(hash){
  api('/api/files?hash=' + hash).then(function(r){ return r.json(); }).then(function(d){
    if (!d.ok) { say(d.error || 'failed', true); return; }
    var paths = d.files.map(function(x){ return x.path; }), root = paths[0].indexOf('/') > 0 ? paths[0].split('/')[0] : '';
    var strip = root && paths.every(function(p){ return p.indexOf(root + '/') === 0; });
    openFiles2([{ title: d.title, files: d.files.map(function(x){ return { path: strip ? x.path.substr(root.length + 1) : x.path, size: x.size }; }), chk: d.files.map(function(x){ return !!x.sel; }) }],
               'edit', { hash: d.hash, title: d.title });
  }).catch(function(){ say('No answer from the console', true); });
}

$('f2go').onclick = function(){
  if (!f2) return;
  var m = f2;
  if (m.mode === 'edit') {
    var txt = skipText(m.groups[0].chk);
    closeDialog();
    api('/api/select?hash=' + m.hash + '&skip=' + encodeURIComponent(txt)).then(function(r){ return r.json(); })
      .then(function(d){ say(d.ok ? 'Files updated' : (d.error || 'failed'), !d.ok); setTimeout(function(){ load(true); }, 1500); })
      .catch(function(){ say('No answer from the console', true); });
    return;
  }
  var files = m.groups.map(function(g){ return { name: g.src.name, data: g.src.data, skip: skipText(g.chk) }; });
  closeDialog();
  doUpload(files, m.drive, m.start, m.label);
};

$('up').onchange = function(){
  var fs = Array.prototype.slice.call(this.files);
  this.value = '';
  if (!fs.length) return;
  var out = [], left = fs.length;
  var done = function(){ if (--left === 0) { api('/status').then(function(r){ return r.json(); }).then(function(d){ lastData = d; }).catch(function(){}).then(function(){ openDialog(out); }); } };
  fs.forEach(function(f, i){
    var rd = new FileReader();
    rd.onload = function(){ out[i] = { name: f.name, data: rd.result, info: torrentInfo(rd.result) }; done(); };
    rd.onerror = function(){ out[i] = { name: f.name, data: null, info: null }; done(); };
    rd.readAsArrayBuffer(f);
  });
};
$('dsel').onchange = updateDlg;
$('dcancel').onclick = closeDialog;
$('dlg').onclick = function(e){ if (e.target === $('dlg')) closeDialog(); };

function doUpload(files, drive, start, label){
  if (!files.length) return;
  say('Adding ' + files.length + ' torrent(s) to ' + label + '...', false);
  var okN = 0, errs = [], left = files.length;
  var fin = function(){
    if (--left > 0) return;
    if (errs.length) say('Added ' + okN + ', failed ' + errs.length + ': ' + errs.join('; '), true);
    else say('Added ' + okN + ' to ' + label + (start ? ' and started' : ' (not started: press resume)'), false);
    load(true);
  };
  files.forEach(function(f){
    api('/api/add?name=' + encodeURIComponent(f.name) + '&drive=' + encodeURIComponent(drive) + '&start=' + start + (f.skip ? '&skip=' + encodeURIComponent(f.skip) : ''), { method: 'POST', body: f.data })
      .then(function(r){ return r.json(); })
      .then(function(d){ if (d.ok) okN++; else errs.push(f.name + ': ' + d.error); fin(); })
      .catch(function(){ errs.push(f.name + ': upload failed'); fin(); });
  });
}

$('dgo').onclick = function(){
  var valid = dlgFiles.filter(function(f){ return f.info && f.data; });
  if (!valid.length) return;
  var sel = $('dsel'), drive = sel.value, start = $('dstart').checked ? 1 : 0;
  var label = sel.options[sel.selectedIndex] ? sel.options[sel.selectedIndex].text.split(' - ')[0] : drive;
  if (anyMulti()) {                                                 // есть раздача из нескольких файлов: окно выбора файлов
    openFiles2(valid.map(function(f){ return { title: f.info.name, files: f.info.files, chk: f.info.files.map(function(){ return true; }), src: f }; }),
               'add', { drive: drive, start: start, label: label });
    return;
  }
  closeDialog();
  doUpload(valid.map(function(f){ return { name: f.name, data: f.data, skip: '' }; }), drive, start, label);
};

// Все задачи сразу
$('allgo').onclick = function(){ api('/api/resume_all').then(function(){ load(); }); };
$('allstop').onclick = function(){ api('/api/pause_all').then(function(){ load(); }); };

// Настройки
$('cfgb').onclick = function(){
  var p = $('cfg');
  if (p.style.display === 'block') { p.style.display = 'none'; return; }
  api('/api/config').then(function(r){ return r.json(); }).then(function(c){
    $('mp').value = c.max_parallel; $('lp').value = c.listen_port; $('as').checked = !!c.autostart; $('rs').checked = c.resume_session !== false;
    $('cfgi').textContent = 'version ' + c.version;
    p.style.display = 'block';
  });
};
$('cfgs').onclick = function(){
  api('/api/set?max_parallel=' + encodeURIComponent($('mp').value) + '&listen_port=' + encodeURIComponent($('lp').value) + '&autostart=' + ($('as').checked ? 1 : 0) + '&resume_session=' + ($('rs').checked ? 1 : 0))
    .then(function(r){ return r.json(); }).then(function(d){ say(d.ok ? 'Saved' : (d.error || 'failed'), !d.ok); setTimeout(load, 1500); });
};

load(); setInterval(function(){ load(); }, 2000);
</script></body></html>
)HTMLPAGE";

static bool send_all(int c, const char* data, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(c, data + sent, len - sent, 0);
        if (n <= 0) return false;
        sent += (size_t)n;
    }
    return true;
}

static void respond_status(int c, int code, const char* text, const char* type, const std::string& body)
{
    char head[240];
    int hl = snprintf(head, sizeof(head),
                      "HTTP/1.0 %d %s\r\nContent-Type: %s\r\nCache-Control: no-store\r\n"
                      "Content-Length: %d\r\n\r\n", code, text, type, (int)body.size());
    send_all(c, head, (size_t)hl);
    send_all(c, body.data(), body.size());
}

static void respond(int c, const char* type, const std::string& body) { respond_status(c, 200, "OK", type, body); }

// Вшитая картинка: браузер может держать её в кэше (значок и логотип не меняются).
static void respond_bytes(int c, const char* type, const unsigned char* data, size_t len)
{
    char head[240];
    int hl = snprintf(head, sizeof(head), "HTTP/1.0 200 OK\r\nContent-Type: %s\r\nCache-Control: max-age=3600\r\nContent-Length: %d\r\n\r\n", type, (int)len);
    send_all(c, head, (size_t)hl);
    send_all(c, (const char*)data, len);
}

static void respond_json_error(int c, int code, const char* text, const std::string& message)
{
    respond_status(c, code, text, "application/json", "{\"ok\":false,\"error\":\"" + json_escape(message) + "\"}");
}

static bool hex_ok(const std::string& h)
{
    if (h.size() < 8 || h.size() > 40) return false;
    for (size_t i = 0; i < h.size(); i++) {
        char ch = h[i];
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F'))) return false;
    }
    return true;
}

static std::string url_decode(const std::string& s)
{
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '+') { o += ' '; continue; }
        if (s[i] == '%' && i + 2 < s.size() && isxdigit((unsigned char)s[i + 1]) && isxdigit((unsigned char)s[i + 2])) {
            char hx[3] = {s[i + 1], s[i + 2], 0};
            o += (char)strtol(hx, NULL, 16);
            i += 2;
            continue;
        }
        o += s[i];
    }
    return o;
}

// Значение параметра key из строки запроса "a=1&b=2" (с раскодированием %XX).
static std::string qget(const std::string& q, const char* key)
{
    std::string k = std::string(key) + "=";
    size_t pos = 0;
    while (pos <= q.size()) {
        size_t amp = q.find('&', pos);
        if (amp == std::string::npos) amp = q.size();
        if (q.compare(pos, k.size(), k) == 0) return url_decode(q.substr(pos + k.size(), amp - pos - k.size()));
        pos = amp + 1;
    }
    return "";
}

// Имя файла из запроса делаем безопасным: без путей, управляющих символов и знаков, запрещённых на exFAT.
static std::string safe_torrent_name(const std::string& in)
{
    std::string o;
    for (size_t i = 0; i < in.size(); i++) {
        unsigned char c = (unsigned char)in[i];
        if (c < 0x20 || c == 0x7f || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') continue;
        o += (char)c;
    }
    while (!o.empty() && (o[0] == '.' || o[0] == ' ')) o.erase(0, 1);
    if (ends_with_ci(o, ".torrent")) o.erase(o.size() - 8);
    while (!o.empty() && (o[o.size() - 1] == '.' || o[o.size() - 1] == ' ')) o.erase(o.size() - 1);
    if (o.size() > 180) { size_t n = 180; while (n > 0 && ((unsigned char)o[n] & 0xC0) == 0x80) n--; o.erase(n); }
    if (o.empty()) o = "upload";
    return o + ".torrent";
}

static bool read_more(int c, std::string& buf, int timeoutSec)
{
    fd_set rf;
    struct timeval tv = {timeoutSec, 0};
    FD_ZERO(&rf);
    FD_SET(c, &rf);
    if (select(c + 1, &rf, NULL, NULL, &tv) <= 0) return false;
    char tmp[4096];
    ssize_t n = recv(c, tmp, sizeof(tmp), 0);
    if (n <= 0) return false;
    buf.append(tmp, (size_t)n);
    return true;
}

static const size_t MAX_UPLOAD = 1024 * 1024;       // .torrent больше 1 МБ не принимаем

// Добавление .torrent с другого устройства: проверяем и кладём на флешку, дальше его подхватит обычное сканирование.
static void handle_add(int c, const std::string& query, const std::string& body)
{
    std::string name = safe_torrent_name(qget(query, "name"));

    // Куда класть: drive=internal (память консоли), drive=N (флешка N) или, если не указано, как выбрано в настройках
    // (save_to): память консоли либо флешка с наибольшим свободным местом.
    std::string root;
    std::string want = qget(query, "drive");
    bool useInternal = (want == "internal") || (want.empty() && g_saveTo.load() == 1);
    if (useInternal) {
        make_dirs(INTERNAL_ROOT);                      // папка создаётся при первом использовании
        if (!is_dir(INTERNAL_ROOT)) { respond_json_error(c, 500, "Error", "cannot create the folder in the console memory"); return; }
        root = INTERNAL_ROOT;
    } else {
        // Накопитель выбран явно (drive=N) или берём тот, где больше свободного места: сначала среди тех, где уже есть папка
        // torrents, а если таких нет, среди любых смонтированных. Если папки torrents на выбранном накопителе нет, создаём её.
        std::string mp;
        if (!want.empty()) {
            bool digits = true;
            for (size_t i = 0; i < want.size(); i++) if (want[i] < '0' || want[i] > '9') digits = false;
            int n = digits ? atoi(want.c_str()) : -1;
            if (n < 0 || n >= SLOT_COUNT) { respond_json_error(c, 400, "Bad Request", "drive must be internal or a drive number"); return; }
            char mpb[96];
            slot_mount(n, mpb, sizeof(mpb));
            if (!usb_mount_available(mpb)) { respond_json_error(c, 409, "Conflict", std::string("drive ") + mpb + " is not mounted"); return; }
            mp = mpb;
        } else {
            unsigned long long bestFree = 0;
            bool bestHas = false;
            for (int d = 0; d < SLOT_COUNT; d++) {
                char mpb[96];
                slot_mount(d, mpb, sizeof(mpb));
                if (!usb_mount_available(mpb) || !usb_mount_writable(mpb)) continue;      // только доступные для записи
                bool has = is_dir(std::string(mpb) + "/torrents");
                struct statvfs sv;
                unsigned long long fr = (statvfs(mpb, &sv) == 0) ? (unsigned long long)sv.f_bavail * sv.f_frsize : 0;
                if (mp.empty() || (has && !bestHas) || (has == bestHas && fr > bestFree)) { mp = mpb; bestFree = fr; bestHas = has; }
            }
            if (mp.empty()) { respond_json_error(c, 409, "Conflict", "no writable USB or extended drive is mounted (you can save to the console memory instead)"); return; }
        }
        root = mp + "/torrents";
        if (!is_dir(root)) {
            int err = 0;
#ifdef HOST_TEST
            if (access((mp + ".readonly").c_str(), F_OK) == 0) err = EROFS;        // тесты: накопитель только для чтения
            else
#endif
            if (mkdir(root.c_str(), 0777) != 0 && errno != EEXIST) err = errno;
            if (err != 0 || !is_dir(root)) {
                respond_json_error(c, 500, "Error", "cannot create the 'torrents' folder on " + mp + " (errno " + std::to_string(err) + ": " + strerror(err) + "); is the drive read-only?");
                return;
            }
            logf_("created the 'torrents' folder on %s", mp.c_str());
        }
    }

    // Сначала во временный файл и проверка, что это настоящий .torrent.
    static volatile unsigned addSeq = 0;
    std::string tmp = root + "/.upload." + std::to_string(__sync_add_and_fetch(&addSeq, 1)) + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) { respond_json_error(c, 500, "Error", "cannot write to the drive"); return; }
    bool wrote = fwrite(body.data(), 1, body.size(), f) == body.size();
    wrote = (fclose(f) == 0) && wrote;
    if (!wrote) { unlink(tmp.c_str()); respond_json_error(c, 500, "Error", "write to the drive failed"); return; }

    Torrent t;
    std::string err;
    bool loaded = false;
    try { loaded = load_torrent(tmp.c_str(), t, err); }
    catch (const std::exception& e) {
        unlink(tmp.c_str());
        respond_json_error(c, 507, "Insufficient Storage", "not enough memory on the console to load this torrent (too many files?)");
        return;
    }
    if (!loaded) {
        unlink(tmp.c_str());
        respond_json_error(c, 400, "Bad Request", "not a valid .torrent: " + err);
        return;
    }

    // skip=3,5-9: номера файлов раздачи, которые качать не надо. Проверяем до сохранения: плохой список или "сняты все" не принимаем.
    {
        std::string sp = qget(query, "skip");
        if (!sp.empty()) {
            std::vector<bool> selChk;
            if (!sel_from_text(sp, t.files.size(), selChk)) { unlink(tmp.c_str()); respond_json_error(c, 400, "Bad Request", "skip: bad list of file numbers"); return; }
            if (!sel_has_data(t, selChk)) { unlink(tmp.c_str()); respond_json_error(c, 400, "Bad Request", "select at least one file"); return; }
        }
    }

    // Имя не должно затирать существующий файл.
    std::string base = name.substr(0, name.size() - 8), finalName = name;
    for (int n = 2; n < 100; n++) {
        struct stat sb;
        if (stat((root + "/" + finalName).c_str(), &sb) != 0) break;
        char suffix[24];
        snprintf(suffix, sizeof(suffix), " (%d)", n);
        finalName = base + suffix + ".torrent";
    }
    if (rename(tmp.c_str(), (root + "/" + finalName).c_str()) != 0) {
        unlink(tmp.c_str());
        respond_json_error(c, 500, "Error", "cannot save the file");
        return;
    }
    std::string skipParam = qget(query, "skip");              // проверен выше, до сохранения файла
    // start=1: запустить сразу; start=0: оставить на паузе; без параметра: по настройке автозапуска.
    std::string startParam = qget(query, "start");
    bool started = g_autostart;
    if (startParam == "1" || startParam == "0") {
        started = (startParam == "1");
        pthread_mutex_lock(&g_addMu);
        g_addStart[root + "/" + finalName] = started;
        pthread_mutex_unlock(&g_addMu);
    }
    if (!skipParam.empty()) {
        pthread_mutex_lock(&g_addMu);
        g_addSel[root + "/" + finalName] = skipParam;
        pthread_mutex_unlock(&g_addMu);
    }
    logf_("added via web: %s (%llu bytes, %u pieces) on %s%s", finalName.c_str(), (unsigned long long)t.totalSize, t.numPieces, root.c_str(),
          startParam.empty() ? "" : (started ? " [start requested]" : " [paused requested]"));
    g_rescan = true;
    respond(c, "application/json", "{\"ok\":true,\"file\":\"" + json_escape(finalName) + "\",\"root\":\"" + json_escape(root) + "\",\"autostart\":" + (g_autostart ? "true" : "false") + ",\"started\":" + (started ? "true" : "false") + "}");
}

// Ставит команду в очередь и ждёт (до 3 секунд), пока главный цикл её применит и обновит состояние:
// тогда страница, запросив состояние сразу после ответа, увидит уже новое, а не старое.
static void queue_cmd(const std::string& cmd, const std::string& arg)
{
    pthread_mutex_lock(&g_mu);
    g_cmds.push_back(std::make_pair(cmd, arg));
    unsigned seq = ++g_cmdSeq;
    pthread_mutex_unlock(&g_mu);

    for (int waited = 0; waited < 3000; waited += 5) {
        pthread_mutex_lock(&g_mu);
        bool done = (int)(g_cmdDone - seq) >= 0;
        pthread_mutex_unlock(&g_mu);
        if (done) return;
        usleep(5000);
    }
    logf_("command %s was not applied within 3 s (main loop busy?)", cmd.c_str());
}

static size_t header_content_length(const std::string& head)
{
    std::string low = head;
    for (size_t i = 0; i < low.size(); i++) low[i] = (char)tolower((unsigned char)low[i]);
    size_t p = low.find("\ncontent-length:");
    if (p == std::string::npos) return 0;
    return (size_t)strtoull(low.c_str() + p + 16, NULL, 10);
}

static void handle_client(int c)
{
    std::string req;
    size_t hend = std::string::npos;
    for (int round = 0; round < 12; round++) {
        hend = req.find("\r\n\r\n");
        if (hend != std::string::npos) break;
        if (req.compare(0, 4, "GET ") == 0 && req.find('\n') != std::string::npos) break;     // как раньше: GET без пустой строки
        if (req.size() > 16384) { respond_json_error(c, 431, "Header Too Large", "request too large"); return; }
        if (!read_more(c, req, 2)) break;
    }
    if (req.empty()) return;
    if (hend != std::string::npos && hend > 16384) { respond_json_error(c, 431, "Header Too Large", "request too large"); return; }

    std::string head = hend != std::string::npos ? req.substr(0, hend) : req;
    std::string body = hend != std::string::npos ? req.substr(hend + 4) : std::string();

    std::string line = head.substr(0, head.find_first_of("\r\n"));
    size_t sp1 = line.find(' ');
    if (sp1 == std::string::npos) { respond(c, "text/plain", "bad request\n"); return; }
    std::string method = line.substr(0, sp1);
    size_t sp2 = line.find(' ', sp1 + 1);
    std::string url = line.substr(sp1 + 1, sp2 == std::string::npos ? std::string::npos : sp2 - sp1 - 1);

    std::string path = url, query;
    size_t q = url.find('?');
    if (q != std::string::npos) { path = url.substr(0, q); query = url.substr(q + 1); }

    if (method != "GET" && method != "POST") { respond_status(c, 405, "Method Not Allowed", "text/plain", "GET or POST only\n"); return; }

    // Страница отдаётся всегда (в ней нет данных); всё остальное требует пароль, если он задан.
    if (path == "/" || path == "/index.html") { respond(c, "text/html; charset=utf-8", PAGE); return; }
    // Картинки отдаём без пароля (как и саму страницу): значок сайта, логотип и иконка уведомлений.
    if (path == "/favicon.ico") { respond_bytes(c, "image/x-icon", ps4_favicon_start, (size_t)(ps4_favicon_end - ps4_favicon_start)); return; }
    if (path == "/logo.png") { respond_bytes(c, "image/png", ps4_logo_start, (size_t)(ps4_logo_end - ps4_logo_start)); return; }
    if (path == "/notify.png") { respond_bytes(c, "image/png", ps4_notify_start, (size_t)(ps4_notify_end - ps4_notify_start)); return; }
    if (!g_token.empty() && qget(query, "token") != g_token) {
        respond_json_error(c, 401, "Unauthorized", "access token required");
        return;
    }

    if (path == "/status") {
        pthread_mutex_lock(&g_mu);
        std::string j = g_json;
        pthread_mutex_unlock(&g_mu);
        respond(c, "application/json", j);
    } else if (path == "/api/config") {
        pthread_mutex_lock(&g_mu);
        std::string j = g_cfg;
        pthread_mutex_unlock(&g_mu);
        respond(c, "application/json", j);
    } else if (path == "/api/pause" || path == "/api/resume") {
        std::string hash = qget(query, "hash");
        if (!hex_ok(hash)) { respond_json_error(c, 400, "Bad Request", "bad hash"); return; }
        queue_cmd(path == "/api/pause" ? "pause" : "resume", hash);
        respond(c, "application/json", "{\"ok\":true}");
    } else if (path == "/api/files") {
        std::string hash = qget(query, "hash");
        if (hash.size() < 8 || !hex_ok(hash)) { respond_json_error(c, 400, "Bad Request", "bad hash"); return; }
        for (size_t k = 0; k < hash.size(); k++) if (hash[k] >= 'A' && hash[k] <= 'F') hash[k] = (char)(hash[k] - 'A' + 'a');
        queue_cmd("files", hash);
        std::string reply;
        pthread_mutex_lock(&g_mu);
        reply = g_filesReply;
        pthread_mutex_unlock(&g_mu);
        if (reply.empty() || (reply.find("\"hash\":\"" + hash) == std::string::npos && reply.find("\"ok\":false") == std::string::npos)) {
            respond_json_error(c, 409, "Conflict", "busy, try again");
            return;
        }
        respond(c, "application/json", reply);
    } else if (path == "/api/select") {
        std::string hash = qget(query, "hash"), skip = qget(query, "skip");
        if (hash.size() < 8 || !hex_ok(hash)) { respond_json_error(c, 400, "Bad Request", "bad hash"); return; }
        if (skip.size() > 6000) { respond_json_error(c, 400, "Bad Request", "skip list too long"); return; }
        for (size_t k = 0; k < skip.size(); k++)
            if (!((skip[k] >= '0' && skip[k] <= '9') || skip[k] == ',' || skip[k] == '-')) { respond_json_error(c, 400, "Bad Request", "skip: only digits, commas and dashes"); return; }
        queue_cmd("select", hash + " " + skip);
        respond(c, "application/json", "{\"ok\":true}");
    } else if (path == "/api/pause_all" || path == "/api/resume_all") {
        queue_cmd(path == "/api/pause_all" ? "pause_all" : "resume_all", "");
        respond(c, "application/json", "{\"ok\":true}");
    } else if (path == "/api/delete") {
        std::string hash = qget(query, "hash");
        if (hash.size() != 40 || !hex_ok(hash)) { respond_json_error(c, 400, "Bad Request", "delete needs the full 40-character hash"); return; }
        queue_cmd(qget(query, "files") == "1" ? "delete_files" : "delete", hash);
        respond(c, "application/json", "{\"ok\":true}");
    } else if (path == "/api/set") {
        std::string mp = qget(query, "max_parallel"), lp = qget(query, "listen_port"), as = qget(query, "autostart"),
                    sv = qget(query, "save_to"), rsv = qget(query, "resume_session"), arg;
        if (mp.empty() && lp.empty() && as.empty() && sv.empty() && rsv.empty()) { respond_json_error(c, 400, "Bad Request", "nothing to change"); return; }
        if (!rsv.empty()) {
            if (rsv != "0" && rsv != "1") { respond_json_error(c, 400, "Bad Request", "resume_session must be 0 or 1"); return; }
            arg += "resume_session=" + rsv + "&";
        }
        if (!sv.empty()) {
            if (sv != "usb" && sv != "internal") { respond_json_error(c, 400, "Bad Request", "save_to must be usb or internal"); return; }
            arg += "save_to=" + sv + "&";
        }
        if (!as.empty()) {
            if (as != "0" && as != "1") { respond_json_error(c, 400, "Bad Request", "autostart must be 0 or 1"); return; }
            arg += "autostart=" + as + "&";
        }
        if (!mp.empty()) {
            int v = atoi(mp.c_str());
            if (v < 1 || v > MAX_PARALLEL_LIMIT) { respond_json_error(c, 400, "Bad Request", "max_parallel must be 1.." + std::to_string(MAX_PARALLEL_LIMIT)); return; }
            arg += "max_parallel=" + std::to_string(v) + "&";
        }
        if (!lp.empty()) {
            int v = atoi(lp.c_str());
            if (v != 0 && (v < 1024 || v > 65535)) { respond_json_error(c, 400, "Bad Request", "listen_port must be 0 or 1024..65535"); return; }
            arg += "listen_port=" + std::to_string(v) + "&";
        }
        queue_cmd("set", arg);
        respond(c, "application/json", "{\"ok\":true}");
    } else if (path == "/api/add") {
        if (method != "POST") { respond_json_error(c, 405, "Method Not Allowed", "use POST with the .torrent file as the body"); return; }
        size_t clen = header_content_length(head);
        if (clen == 0) { respond_json_error(c, 400, "Bad Request", "empty body"); return; }
        if (clen > MAX_UPLOAD) { respond_json_error(c, 413, "Payload Too Large", "file is larger than 1 MB"); return; }
        time_t deadline = time(NULL) + 20;
        while (body.size() < clen) {
            if (time(NULL) > deadline || !read_more(c, body, 3)) break;
        }
        if (body.size() < clen) { respond_json_error(c, 408, "Request Timeout", "incomplete upload"); return; }
        body.resize(clen);
        handle_add(c, query, body);
    } else if (path == "/quit") {
        respond(c, "text/plain", "bye\n");
        g_quit = true;
#ifdef HOST_TEST
    } else if (path == "/_throw") {
        throw std::runtime_error("selftest exception");
    } else if (path == "/_abort") {
        abort();
#endif
    } else {
        respond_status(c, 404, "Not Found", "text/plain", "not found\n");
    }
}

static std::atomic<int> g_clients(0);              // сколько соединений обслуживается сейчас
static const int MAX_CLIENTS = 12;

struct ClientArg { int fd; };

static void serve_client(int fd)
{
    // Ошибка при обработке одного запроса не должна останавливать сервер.
    try {
        handle_client(fd);
    } catch (const std::exception& e) {
        logf_("!!! EXCEPTION in http request: %s", e.what());
    } catch (...) {
        logf_("!!! UNKNOWN EXCEPTION in http request");
    }
    close(fd);
}

static void* client_thread(void* arg)
{
    ClientArg* ca = (ClientArg*)arg;
    int fd = ca->fd;
    delete ca;
    serve_client(fd);
    g_clients.fetch_sub(1);
    return NULL;
}

static int http_listen_socket_fwd(int* err);

static std::string ip_to_string(uint32_t netOrder)
{
    const unsigned char* b = (const unsigned char*)&netOrder;
    char buf[24];
    snprintf(buf, sizeof(buf), "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    return buf;
}

// Принимает соединения. Каждое обслуживается в своём потоке: браузеры заранее открывают пустые соединения,
// и при одном потоке на всех такое соединение держало бы остальные запросы до двух секунд.
// Слушающий сокет может быть пересоздан главным потоком (см. net_watch), поэтому читаем g_srv на каждом обороте.
// Поток сам пересоздаёт слушающий сокет (закрывать и создавать его нужно именно из этого потока).
// true: новый сокет создан.
static bool http_thread_recreate(int* err)
{
    int old = g_srv.exchange(-1);
    if (old >= 0) close(old);
    int ns = http_listen_socket_fwd(err);
    for (int i = 0; i < 10 && ns < 0; i++) { usleep(100000); ns = http_listen_socket_fwd(err); }
    if (ns < 0) return false;
    g_srv = ns;
    return true;
}

static void* http_thread(void*)
{
    unsigned acceptFails = 0, selectFails = 0;
    time_t lastHealLog = 0;
    // Сокет мог сломаться (после сна консоли accept() начинает стабильно возвращать ошибку, а select() всё время
    // сообщает "есть подключение"): создаём его заново.
    auto heal = [&](const char* what, int e) {
        int err = 0;
        bool ok = http_thread_recreate(&err);
        g_httpHeals++;
        time_t now = time(NULL);
        if (now - lastHealLog >= 10) {
            lastHealLog = now;
            if (ok) logf_("http: the listening socket was broken (%s, errno %d), created a new one", what, e);
            else logf_("http: the listening socket is broken (%s, errno %d) and a new one could not be created (errno %d)", what, e, err);
        }
        acceptFails = selectFails = 0;
        usleep(500000);
    };
    while (!g_quit) {
        // Просьба главного потока пересоздать сокет. Закрывать и создавать его должен именно этот поток: пока он
        // сидит в select() на сокете, закрытие из другого потока не освобождает порт, и bind() получает "адрес занят".
        if (g_rebindReq.load() == 1) {
            int err = 0;
            bool ok = http_thread_recreate(&err);
            if (ok) { g_rebindDone = 1; } else { g_rebindErr = err; g_rebindDone = -1; }
            g_rebindReq = 0;
            continue;
        }
        int s = g_srv.load();
        if (s < 0) { usleep(100000); continue; }                       // сокет не создан
        fd_set rf;
        struct timeval tv = {0, 200000};                               // 0,2 с: просьбу о пересоздании увидим быстро
        FD_ZERO(&rf);
        FD_SET(s, &rf);
        int r = select(s + 1, &rf, NULL, NULL, &tv);
        if (r < 0) {
            if (errno == EINTR || errno == EBADF) { usleep(100000); continue; }
            if (++selectFails >= 10) heal("select keeps failing", errno);
            else usleep(100000);
            continue;
        }
        selectFails = 0;
        if (r == 0) continue;
        struct sockaddr_in pa;
        socklen_t pl = sizeof(pa);
        memset(&pa, 0, sizeof(pa));
        int c;
#ifdef HOST_TEST
        {   // тесты: пока в файле число больше нуля, accept() "ломается" (как после сна консоли)
            FILE* ff = fopen("/tmp/ft/accept_fail", "r");
            int n = 0;
            if (ff) { if (fscanf(ff, "%d", &n) != 1) n = 0; fclose(ff); }
            if (n > 0) {
                ff = fopen("/tmp/ft/accept_fail", "w");
                if (ff) { fprintf(ff, "%d", n - 1); fclose(ff); }
                c = -1;
                errno = 163;
            } else {
                c = accept(s, (struct sockaddr*)&pa, &pl);
            }
        }
#else
        c = accept(s, (struct sockaddr*)&pa, &pl);
#endif
        if (c < 0) {
            int e = errno;
            if (e == EINTR || e == EBADF || e == ECONNABORTED || e == EAGAIN || e == EWOULDBLOCK) continue;
            if (e == EMFILE || e == ENFILE) { usleep(200000); continue; }       // нехватка дескрипторов: пересоздание не поможет
            if (++acceptFails >= 3) heal("accept keeps failing", e);
            else usleep(100000);
            continue;
        }
        acceptFails = 0;
        // Первые соединения пишем в лог: если страница не открывается, а таких строк нет, запросы до сервера не доходят.
        unsigned nconn = ++g_httpConns;
        if (nconn <= 10 || nconn % 100 == 0) logf_("http: connection #%u from %s", nconn, ip_to_string(pa.sin_addr.s_addr).c_str());

        if (g_clients.fetch_add(1) >= MAX_CLIENTS) {                      // слишком много одновременно
            g_clients.fetch_sub(1);
            close(c);
            continue;
        }
        ClientArg* ca = new ClientArg();
        ca->fd = c;
        pthread_t th;
        if (pthread_create(&th, NULL, client_thread, ca) == 0) {
            pthread_detach(th);
        } else {
            delete ca;
            serve_client(c);                                             // поток не создался: обслуживаем здесь
            g_clients.fetch_sub(1);
        }
    }
    return NULL;
}

// Создаёт слушающий сокет веб-сервера. -1 при ошибке (причина в err).
static int http_listen_socket(int* err)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) { *err = errno; return -1; }
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
#ifndef __linux__
    sa.sin_len = sizeof(sa);
#endif
    sa.sin_family = AF_INET;
    sa.sin_port = htons(HTTP_PORT);
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, (struct sockaddr*)&sa, sizeof(sa)) < 0) { *err = errno; close(s); return -1; }
    if (listen(s, 8) < 0) { *err = errno; close(s); return -1; }
    return s;
}

static int http_listen_socket_fwd(int* err) { return http_listen_socket(err); }

// Главный поток просит поток сервера пересоздать слушающий сокет и ждёт результат (до 5 секунд).
static bool http_rebind(int* err)
{
#ifdef HOST_TEST
    {   // тесты: файл со счётчиком; пока он больше нуля, пересоздание "не удаётся"
        FILE* ff = fopen("/tmp/ft/rebind_fail", "r");
        if (ff) {
            int n = 0;
            if (fscanf(ff, "%d", &n) != 1) n = 0;
            fclose(ff);
            if (n > 0) { ff = fopen("/tmp/ft/rebind_fail", "w"); if (ff) { fprintf(ff, "%d", n - 1); fclose(ff); } *err = 98; return false; }
        }
    }
#endif
    g_rebindDone = 0;
    g_rebindReq = 1;
    for (int waited = 0; waited < 5000; waited += 10) {
        int d = g_rebindDone.load();
        if (d != 0) { *err = g_rebindErr.load(); return d == 1; }
        usleep(10000);
    }
    *err = -1;
    return false;
}

// false, если порт занят (скорее всего уже запущена другая копия). Сам в лог не пишет:
// вторая копия не должна стирать лог первой, поэтому причина возвращается через err.
static bool http_start(int* err)
{
    int s = http_listen_socket(err);
    if (s < 0) return false;
    g_srv = s;
    return true;
}

// Поток обслуживания HTTP запускается после чтения настроек (пароль читается из config.txt),
// иначе поток мог бы обратиться к g_token в тот момент, когда главный поток его записывает.
static bool http_spawn()
{
    if (g_srv.load() < 0) return false;
    if (pthread_create(&g_srvThread, NULL, http_thread, NULL) != 0) return false;
    g_srvStarted = true;
    return true;
}

static void apply_set(const std::string& arg)
{
    std::string mp = qget(arg, "max_parallel"), lp = qget(arg, "listen_port"), as = qget(arg, "autostart"), sv = qget(arg, "save_to"), rsv = qget(arg, "resume_session");
    if (!rsv.empty()) { g_resumeSession = (rsv == "1"); logf_("settings: resume_session=%s", rsv.c_str()); }
    if (!sv.empty()) {
        g_saveTo = (sv == "internal") ? 1 : 0;
        if (g_saveTo.load() == 1) { make_dirs(INTERNAL_ROOT); g_rescan = true; }     // папка создаётся при первом выборе
        logf_("settings: save_to=%s", sv.c_str());
    }
    if (!as.empty()) { g_autostart = (as == "1"); logf_("settings: autostart=%d", g_autostart ? 1 : 0); }
    if (!mp.empty()) {
        int v = atoi(mp.c_str());
        if (v >= 1 && v <= MAX_PARALLEL_LIMIT) { g_maxParallel = v; logf_("settings: max_parallel=%d", v); }
    }
    if (!lp.empty()) {
        int v = atoi(lp.c_str());
        if ((v == 0 || (v >= 1024 && v <= 65535)) && v != g_listenPort) {
            incoming_stop();
            g_listenPort = v;
            if (v > 0) {
                int e = 0;
                if (incoming_start(v, &e)) { tracker_set_port(v); logf_("settings: now listening on port %d", v); }
                else logf_("settings: cannot listen on port %d (errno %d)", v, e);
            } else {
                logf_("settings: incoming connections disabled");
            }
        }
    }
    save_config();
}

// Применяет отложенную смену выбора файлов, когда у задачи нет работающей загрузки и недописанных записей.
// Важно при возврате ранее снятого файла: куски на его границе считались скачанными, хотя байты снятого файла на диск
// не писались; такие куски объявляются нескачанными (прогресс перезаписывается целиком), иначе внутри файла осталась бы дыра.
static void try_apply_selection(Item& it, time_t now)
{
    (void)now;
    if (!it.selPending || it.running) return;
    for (size_t k = 0; k < g_zombies.size(); k++) if (g_zombies[k].hash == it.t.infoHashHex) return;     // ждём окончания записей
    it.selPending = false;
    const size_t nf = it.t.files.size();
    std::vector<bool> sel;
    if (it.complete || !sel_from_text(it.selPendingTxt, nf, sel) || !sel_has_data(it.t, sel)) return;
    const uint32_t n = it.t.numPieces;
    const uint64_t pl = it.t.pieceLength;

    std::vector<bool> oldSel = it.fileSel.empty() ? std::vector<bool>(nf, true) : it.fileSel;
    std::vector<bool> redo(n, false);
    bool reopened = false;
    uint64_t off = 0;
    for (size_t k = 0; k < nf; k++) {
        uint64_t len = it.t.files[k].length;
        if (len > 0 && pl > 0 && !oldSel[k] && sel[k]) {
            reopened = true;
            for (uint64_t p = off / pl; p <= (off + len - 1) / pl && p < n; p++) redo[p] = true;
        }
        off += len;
    }
    if (reopened) {
        std::vector<bool> hv;
        std::vector<uint32_t> rec;
        use_state(it);
        bool have = (it.haveValid && it.haveMap.size() == n) ? (hv = it.haveMap, true) : resume_load(it.t.infoHashHex, n, hv, rec);
        if (have && hv.size() == n) {
            uint32_t cleared = 0, cnt = 0;
            for (uint32_t i = 0; i < n; i++) { if (redo[i] && hv[i]) { hv[i] = false; cleared++; } if (hv[i]) cnt++; }
            if (cleared) {
                it.haveMap = hv;
                it.haveValid = true;
                it.have = cnt;
                const std::string dir = it.stateDir(), h = it.t.infoHashHex;
                writer_post([dir, h, n, hv, rec]() { resume_save_at(dir, h, n, hv, rec); });
                logf_("  %u piece(s) at the edges of newly selected files will be downloaded again", cleared);
            }
        }
    }
    apply_selection(it, sel);
    const std::string dir = it.stateDir(), h = it.t.infoHashHex, norm = sel_to_text(sel);
    writer_post([dir, h, norm]() { resume_save_sel_at(dir, h, norm); });
    it.failed = false;
    it.retryAt = 0;
    logf_("command: files of %s changed: %d of %d selected (%llu MB to download)", it.title.c_str(),
          (int)(nf - item_skipped_files(it)), (int)nf, (unsigned long long)(item_size_bytes(it) >> 20));
}

// Список файлов задачи (для /api/files) и смена выбора файлов (для /api/select): выполняются в главном потоке.
static void handle_files_cmd(const std::string& cmd, const std::string& arg, time_t now)
{
    size_t sp = arg.find(' ');
    std::string hash = arg.substr(0, sp), rest = sp == std::string::npos ? "" : arg.substr(sp + 1);
    for (size_t k = 0; k < hash.size(); k++) if (hash[k] >= 'A' && hash[k] <= 'F') hash[k] = (char)(hash[k] - 'A' + 'a');
    Item* it = NULL;
    for (size_t k = 0; k < g_items.size(); k++)
        if (g_items[k]->t.infoHashHex.compare(0, hash.size(), hash) == 0) { it = g_items[k]; break; }

    if (cmd == "files") {
        std::string j;
        if (!it) j = "{\"ok\":false,\"error\":\"unknown task\"}";
        else if (it->t.files.size() > 5000) j = "{\"ok\":false,\"error\":\"too many files to show\"}";
        else {
            j = "{\"ok\":true,\"hash\":\"" + it->t.infoHashHex + "\",\"title\":\"" + json_escape(it->title) + "\",\"complete\":" + (it->complete ? "true" : "false") + ",\"files\":[";
            for (size_t k = 0; k < it->t.files.size(); k++) {
                char b[96];
                snprintf(b, sizeof(b), "%s{\"i\":%u,\"size\":%llu,\"sel\":%s,\"path\":\"", k ? "," : "", (unsigned)k,
                         (unsigned long long)it->t.files[k].length, (it->fileSel.empty() || it->fileSel[k]) ? "true" : "false");
                j += b;
                j += json_escape(it->t.files[k].path) + "\"}";
            }
            j += "]}";
        }
        pthread_mutex_lock(&g_mu);
        g_filesReply = j;
        pthread_mutex_unlock(&g_mu);
        return;
    }

    // select
    if (!it) { logf_("command select: unknown hash %s", hash.c_str()); return; }
    if (it->complete) { logf_("command select %s: a finished task cannot change its files", it->title.c_str()); return; }
    std::vector<bool> sel;
    if (!sel_from_text(rest, it->t.files.size(), sel) || !sel_has_data(it->t, sel)) {
        logf_("command select %s: bad selection '%s'", it->title.c_str(), rest.c_str());
        return;
    }
    it->selPending = true;                               // применим, когда загрузка остановится и допишутся записи
    it->selPendingTxt = rest;
    if (it->running) stop_job_for(it, now);              // задача перезапустится сама (если не на паузе) уже с новым выбором
    try_apply_selection(*it, now);
}

static size_t apply_commands(time_t now)
{
    std::vector<std::pair<std::string, std::string> > cmds;
    pthread_mutex_lock(&g_mu);
    cmds.swap(g_cmds);
    g_cmdTaken = g_cmdSeq;
    pthread_mutex_unlock(&g_mu);

    for (size_t i = 0; i < cmds.size(); i++) {
        const std::string& cmd = cmds[i].first;
        if (cmd == "set") { apply_set(cmds[i].second); continue; }
        if (cmd == "pause_all" || cmd == "resume_all") {
            bool pause = (cmd == "pause_all");
            int n = 0;
            for (size_t k = 0; k < g_items.size(); k++) {
                Item* o = g_items[k];
                if (o->complete || o->paused == pause) continue;
                o->paused = pause;
                if (!pause) { o->failed = false; o->retryAt = 0; }
                n++;
            }
            logf_("command: %s (%d tasks)", cmd.c_str(), n);
            continue;
        }

        if (cmd == "files" || cmd == "select") { handle_files_cmd(cmd, cmds[i].second, now); continue; }

        std::string hash = cmds[i].second;
        for (size_t k = 0; k < hash.size(); k++) if (hash[k] >= 'A' && hash[k] <= 'F') hash[k] = (char)(hash[k] - 'A' + 'a');
        Item* it = NULL;
        for (size_t k = 0; k < g_items.size(); k++)
            if (g_items[k]->t.infoHashHex.compare(0, hash.size(), hash) == 0) { it = g_items[k]; break; }
        if (!it) { logf_("command %s: unknown hash %s", cmd.c_str(), hash.c_str()); continue; }

        if (cmd == "pause") {
            it->paused = true;
            logf_("command: pause %s", it->title.c_str());
        } else if (cmd == "resume") {
            it->paused = false;
            it->failed = false;
            it->retryAt = 0;
            logf_("command: resume %s", it->title.c_str());
        } else if (cmd == "delete" || cmd == "delete_files") {
            request_delete(it, cmd == "delete_files", now);
        }
    }
    return cmds.size();
}

// ---------------------------------------------------------------- сеть

// После сна консоли или сбоя сети: закрываем открытые файлы (флешка могла переподключиться, старые дескрипторы
// недействительны, они откроются заново при следующей записи) и снова допускаем пиров, которых мы "забанили"
// из-за оборвавшихся соединений.
static void recover_after_pause(bool closeFiles)
{
    if (closeFiles) {
        // Остановившиеся из-за ошибок раздачи пробуем запустить снова сразу, не дожидаясь таймеров (часы консоли
        // после сна могут идти не так, как мы ждём), и сразу проверяем, на месте ли флешка.
        for (size_t i = 0; i < g_items.size(); i++)
            if (g_items[i]->failed && !g_items[i]->paused) g_items[i]->retryAt = 0;
        g_rescan = true;
        logf_("after the pause the process is: %s", process_identity().c_str());       // сравнить с запуском: не изменились ли права
    }
    int files = 0, swarms = 0;
    for (size_t i = 0; i < g_active.size(); i++) {
        if (closeFiles) { int c = g_active[i]->st.close_all_fds(); if (c > 0) files += c; }   // -1: идёт запись, файл не трогаем
        if (g_active[i]->sw) {
            g_active[i]->sw->network_changed();
            if (closeFiles) g_active[i]->sw->forget_persisted();   // записи в кэш флешки могли пропасть вместе с ней
            swarms++;
        }
    }
    if (swarms || files)
        logf_("recovery: %d downloads will reconnect to peers%s", swarms,
              closeFiles ? (", " + std::to_string(files) + " open files were closed and will be reopened").c_str() : "");
}

// Если payload запускается автозапуском (сразу после включения консоли или выхода из режима покоя), сеть ещё
// не готова, и сокеты, созданные в этот момент, потом не принимают подключения: порт занят, а страница не
// открывается. Поэтому программа следит за сетью (раз в 5 секунд, пока её нет, раз в 30, когда есть) и
// пересоздаёт слушающие сокеты, когда сеть появляется или меняется адрес консоли.

#ifndef LOG_DIR
#define LOG_DIR "/data/ps4torrent"
#endif
#define QUIT_FLAG_FILE LOG_DIR "/quit.flag"       // новая копия создаёт его, чтобы попросить работающую завершиться
#ifndef TAKEOVER_WAIT_S
#define TAKEOVER_WAIT_S 90                      // сколько ждать, пока прежняя копия освободит порт
#endif
#ifndef LOOP_GAP_S
#define LOOP_GAP_S 8                            // пауза главного цикла дольше этого = сон консоли или зависший диск
#endif
#ifndef NET_REFRESH_S
#define NET_REFRESH_S 600                       // тихое профилактическое пересоздание сокетов
#endif
#ifndef NET_POLL_DOWN_S
#define NET_POLL_DOWN_S 5                       // как часто проверять сеть, пока её нет
#endif
#ifndef NET_POLL_UP_S
#define NET_POLL_UP_S 30                        // как часто проверять сеть, когда она есть (смена адреса)
#endif
#ifndef NET_FALLBACK_S
#define NET_FALLBACK_S 60                       // если адрес так и не определился, пересоздать сокеты через столько секунд
#endif
#ifndef NETMOCK_FILE
#define NETMOCK_FILE "/tmp/ft/netmock"          // только для тестов на ПК: содержимое = "адрес консоли" (пусто = сети нет)
#endif

// Адрес консоли в локальной сети ("" если сети нет).
static std::string local_ip()
{
#ifdef HOST_TEST
    FILE* mf = fopen(NETMOCK_FILE, "r");
    if (mf) {
        char b[64] = {0};
        if (!fgets(b, sizeof(b), mf)) b[0] = 0;
        fclose(mf);
        std::string m = b;
        while (!m.empty() && (m[m.size() - 1] == '\n' || m[m.size() - 1] == ' ')) m.erase(m.size() - 1);
        return m;
    }
#endif
    std::string found;
    struct ifaddrs* ifa = NULL;
    if (getifaddrs(&ifa) == 0) {
        for (struct ifaddrs* p = ifa; p; p = p->ifa_next) {
            if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
            if (!(p->ifa_flags & IFF_UP) || (p->ifa_flags & IFF_LOOPBACK)) continue;
            uint32_t sa = ((struct sockaddr_in*)p->ifa_addr)->sin_addr.s_addr;
            uint32_t h = ntohl(sa);
            if (h == 0 || (h >> 24) == 127 || (h >> 16) == 0xA9FE) continue;     // пусто, loopback, link-local
            found = ip_to_string(sa);
            break;
        }
        freeifaddrs(ifa);
    }
    if (!found.empty()) return found;

    // Запасной способ: UDP "подключение" к внешнему адресу ничего не отправляет, но выбирает маршрут и адрес.
    int u = socket(AF_INET, SOCK_DGRAM, 0);
    if (u >= 0) {
        struct sockaddr_in d;
        memset(&d, 0, sizeof(d));
#ifndef __linux__
        d.sin_len = sizeof(d);
#endif
        d.sin_family = AF_INET;
        d.sin_port = htons(53);
        d.sin_addr.s_addr = inet_addr("8.8.8.8");
        if (connect(u, (struct sockaddr*)&d, sizeof(d)) == 0) {
            struct sockaddr_in me;
            socklen_t ml = sizeof(me);
            memset(&me, 0, sizeof(me));
            if (getsockname(u, (struct sockaddr*)&me, &ml) == 0) {
                uint32_t h = ntohl(me.sin_addr.s_addr);
                if (h != 0 && (h >> 24) != 127) found = ip_to_string(me.sin_addr.s_addr);
            }
        }
        close(u);
    }
    return found;
}

static bool g_listenersGood = false;     // слушающие сокеты созданы после появления сети
static bool g_noIpFallbackDone = false;
static bool g_everHadIp = false;          // сеть хотя бы раз была обнаружена
static time_t g_netLastCheck = 0, g_netStart = 0, g_lastRefresh = 0;

// Пересоздаёт слушающие сокеты (веб-страница и входящие соединения пиров). true: всё получилось.
static bool restart_listeners(const std::string& ip, const char* why, bool tellUser = true)
{
    bool ok = true;
    int err = 0;
    if (!http_rebind(&err)) {
        logf_("network: cannot restart the web server (errno %d), will retry in %d s", err, NET_POLL_DOWN_S);
        ok = false;
    }
    if (g_listenPort > 0) {
        incoming_stop();
        int e = 0;
        if (incoming_start(g_listenPort, &e)) {
            tracker_set_port(g_listenPort);
        } else {
            logf_("network: cannot restart the incoming listener on port %d (errno %d)", g_listenPort, e);
            ok = false;
        }
    }
    if (ok) {
        g_netRestarts++;
        g_listenersGood = true;
        logf_("network: listeners restarted (%s), local address %s", why, ip.empty() ? "unknown" : ip.c_str());
        g_lastRefresh = time(NULL);
        if (tellUser && !ip.empty()) notify("ps4torrent web: http://%s:%d/", ip.c_str(), HTTP_PORT);
    } else {
        g_listenersGood = false;
    }
    return ok;
}

static void net_watch(time_t now)
{
    if (g_netStart == 0) g_netStart = now;
    int interval = (g_netIp.empty() || !g_listenersGood) ? NET_POLL_DOWN_S : NET_POLL_UP_S;
    if (g_netLastCheck != 0 && now - g_netLastCheck < interval) return;
    g_netLastCheck = now;

    std::string ip = local_ip();
    if (ip != g_netIp) {
        bool had = !g_netIp.empty();
        if (ip.empty()) logf_("network: connection lost (was %s)", g_netIp.c_str());
        else logf_("network: local address %s%s", ip.c_str(), had ? " (changed)" : "");
        g_netIp = ip;
        if (!ip.empty()) { g_everHadIp = true; recover_after_pause(false); }
        if (ip.empty()) { g_listenersGood = false; return; }          // сокеты пересоздадим, когда сеть вернётся
        restart_listeners(ip, had ? "address changed" : "connection is up");
        return;
    }
    if (g_listenersGood && now - g_lastRefresh >= NET_REFRESH_S) {
        // Профилактика: сокеты, созданные давно, после сна консоли или сбоя сети могли тихо перестать работать.
        restart_listeners(ip, "periodic refresh", false);
        return;
    }
    if (!g_listenersGood) {
        if (!ip.empty()) {
            restart_listeners(ip, "retry");
        } else if (!g_everHadIp && !g_noIpFallbackDone && now - g_netStart >= NET_FALLBACK_S) {
            // Адрес определить не удалось ни разу (может быть, способ не работает на этой консоли): пересоздаём всё равно.
            logf_("network: no address detected for %d s, restarting the listeners anyway", NET_FALLBACK_S);
            if (restart_listeners("", "no address detected")) g_noIpFallbackDone = true;
        }
    }
}

// ---------------------------------------------------------------- аварии

// Если программа упадёт (ошибка памяти, необработанное исключение), причина попадает в лог,
// а на экране консоли появляется уведомление. Адреса позволяют найти место в коде.
static volatile int g_inCrash = 0;

static void write_log_line(const char* text, size_t n)
{
    int fd = open(log_path(), O_WRONLY | O_APPEND | O_CREAT, 0666);
    if (fd >= 0) { ssize_t w = write(fd, text, n); (void)w; close(fd); }
}

static void crash_handler(int sig, siginfo_t* si, void* ctx)
{
    if (g_inCrash) _exit(120);
    g_inCrash = 1;

    unsigned long long rip = 0, rsp = 0;
#ifdef __linux__
    rip = (unsigned long long)((ucontext_t*)ctx)->uc_mcontext.gregs[REG_RIP];
    rsp = (unsigned long long)((ucontext_t*)ctx)->uc_mcontext.gregs[REG_RSP];
#else
    rip = (unsigned long long)((ucontext_t*)ctx)->uc_mcontext.mc_rip;
    rsp = (unsigned long long)((ucontext_t*)ctx)->uc_mcontext.mc_rsp;
#endif
    char buf[256];
    int n = snprintf(buf, sizeof(buf),
                     "\n!!! FATAL signal %d, fault addr 0x%llx, rip 0x%llx, handler at 0x%llx !!!\n",
                     sig, (unsigned long long)(uintptr_t)(si ? si->si_addr : 0), rip,
                     (unsigned long long)(uintptr_t)&crash_handler);
    if (n > 0) write_log_line(buf, (size_t)n);
    notify_icon(NULL, "ps4torrent CRASHED (signal %d), see /data/ps4torrent/log.txt", sig);

    // Цепочка вызовов: слова на стеке, похожие на адреса внутри нашей программы
    // (диапазон считается от адреса этого обработчика). Выводятся смещения от начала
    // этого диапазона; расшифровываются по файлу .elf.
    unsigned long long hdl = (unsigned long long)(uintptr_t)&crash_handler;
    unsigned long long lo = hdl & ~0xFFFFFULL;
    unsigned long long hi = lo + 0x200000ULL;
    char line[1300];
    int ln = snprintf(line, sizeof(line), "stack (offsets from 0x%llx, nearest first):", lo);
    const unsigned long long* sp = (const unsigned long long*)rsp;
    int found = 0;
    for (int i = 0; i < 512 && found < 40 && ln < (int)sizeof(line) - 24; i++) {
        unsigned long long w = sp[i];
        if (w >= lo + 0x1000 && w < hi) { ln += snprintf(line + ln, sizeof(line) - (size_t)ln, " 0x%llx", w - lo); found++; }
    }
    if (ln < (int)sizeof(line) - 2) { line[ln++] = '\n'; }
    write_log_line(line, (size_t)ln);
    _exit(100 + sig);
}

// Необработанное исключение C++ (например, нехватка памяти): записываем причину.
static void terminate_handler()
{
    const char* what = "no active exception";
    std::exception_ptr ep = std::current_exception();
    if (ep) {
        try { std::rethrow_exception(ep); }
        catch (const std::exception& e) { what = e.what(); }
        catch (...) { what = "non-standard exception"; }
    }
    char buf[300];
    int n = snprintf(buf, sizeof(buf), "\n!!! std::terminate called: %s !!!\n", what);
    if (n > 0) write_log_line(buf, (size_t)n);
    notify_icon(NULL, "ps4torrent CRASHED: %s", what);
    _exit(121);
}

static void install_crash_handlers()
{
    static char altstack[64 * 1024];            // отдельный стек: ловим и переполнение обычного
    stack_t ss;
    memset(&ss, 0, sizeof(ss));
    ss.ss_sp = altstack;
    ss.ss_size = sizeof(altstack);
    sigaltstack(&ss, NULL);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    const int sigs[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGSYS};
    for (size_t i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++) sigaction(sigs[i], &sa, NULL);
}


// ---------------------------------------------------------------- вшитый файл (проверка)

#ifdef HAVE_BLOB
#include "blob_info.h"
extern "C" {
extern const unsigned char ps4_blob_start[];
extern const unsigned char ps4_blob_end[];
}
#ifndef LOG_DIR
#define LOG_DIR "/data/ps4torrent"
#endif

// Проверяет, что вшитый в payload файл дошёл целым (SHA-1), и что его можно записать на диск и прочитать
// обратно. Результат: строки в логе и уведомление на экране.
static void blob_selftest()
{
    const size_t len = (size_t)(ps4_blob_end - ps4_blob_start);
    uint64_t t0 = now_us();
    unsigned char h[20];
    sha1(ps4_blob_start, len, h);
    char hex[41];
    for (int i = 0; i < 20; i++) snprintf(hex + 2 * i, 3, "%02x", h[i]);
    bool hashOk = strcmp(hex, BLOB_SHA1_HEX) == 0;
    logf_("blob: %zu bytes embedded in the payload, sha1 %s (%s), checked in %llu ms", len, hex,
          hashOk ? "matches" : "DOES NOT MATCH", (unsigned long long)((now_us() - t0) / 1000));

    // запись на диск и чтение обратно кусками (большой буфер занял бы обычную память, а её у нас ~10 МБ)
    std::string path = std::string(LOG_DIR) + "/blob_test.bin";
    t0 = now_us();
    bool diskOk = false;
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) {
        logf_("blob: cannot create %s (errno %d)", path.c_str(), errno);
    } else {
        const size_t CH = 256 * 1024;
        size_t off = 0;
        bool wok = true;
        while (off < len && wok) {
            size_t n = std::min(CH, len - off);
            wok = fwrite(ps4_blob_start + off, 1, n, f) == n;
            off += n;
        }
        wok = (fclose(f) == 0) && wok;
        if (!wok) {
            logf_("blob: writing %s failed (errno %d)", path.c_str(), errno);
        } else {
            FILE* r = fopen(path.c_str(), "rb");
            if (!r) {
                logf_("blob: cannot read back %s (errno %d)", path.c_str(), errno);
            } else {
                std::vector<unsigned char> buf(CH);
                size_t pos = 0;
                bool same = true;
                while (pos < len && same) {
                    size_t n = std::min(CH, len - pos);
                    same = fread(buf.data(), 1, n, r) == n && memcmp(buf.data(), ps4_blob_start + pos, n) == 0;
                    pos += n;
                }
                fclose(r);
                diskOk = same;
                logf_("blob: written to %s and read back: %s (%llu ms)", path.c_str(), same ? "identical" : "DIFFERENT",
                      (unsigned long long)((now_us() - t0) / 1000));
            }
        }
    }
    notify("ps4torrent: embedded %zu MB file: %s, disk write %s", len >> 20, hashOk ? "intact" : "DAMAGED", diskOk ? "OK" : "FAILED");
}
#endif

// ---------------------------------------------------------------- main

int main()
{
    signal(SIGPIPE, SIG_IGN);
    std::set_terminate(terminate_handler);
    install_crash_handlers();

    // Сначала занимаем порт: если он занят, уже работает другая копия, и её лог трогать нельзя.
    int herr = 0;
    int takeoverMs = 0;
    if (!http_start(&herr)) {
        if (herr != EADDRINUSE) {
            logf_("---- cannot start: web server socket error (errno %d) ----", herr);
            notify("ps4torrent: cannot start the web server (error %d)", herr);
            return 1;
        }
        // Порт занят: скорее всего, работает прежняя копия (иногда зависшая, с неотвечающей страницей).
        // Просим её завершиться через файл-флаг и занимаем её место. Так отправка нового ELF заменяет старый.
        logf_("---- another copy is running (http port %d is busy): asking it to quit ----", HTTP_PORT);
        notify("ps4torrent: replacing the running copy, please wait...");
        FILE* qf = fopen(QUIT_FLAG_FILE, "w");
        if (qf) { fputs("quit\n", qf); fclose(qf); }
        bool got = false;
        for (int w = 0; w < TAKEOVER_WAIT_S * 2; w++) {
            usleep(500000);
            if (http_start(&herr)) { got = true; takeoverMs = (w + 1) * 500; break; }
        }
        if (!got) {
            remove(QUIT_FLAG_FILE);
            logf_("---- the previous copy did not stop within %d s: giving up ----", TAKEOVER_WAIT_S);
            notify("ps4torrent: the previous copy did not stop (is the drive busy?). Try again in a minute");
            return 1;
        }
    }
    remove(QUIT_FLAG_FILE);                  // остатки старого флага не должны остановить уже эту копию

    log_reset();
    logf_("---- ps4torrentd start ----");
    logf_("version %s", APP_VERSION);
    if (takeoverMs > 0) logf_("the previous copy was asked to quit and replaced (waited %d ms)", takeoverMs);
    logf_("http: listening on port %d", HTTP_PORT);

    load_config();
    logf_("process: %s", process_identity().c_str());
    snprintf(g_notifyIcon, sizeof(g_notifyIcon), "http://127.0.0.1:%d/notify.png", HTTP_PORT);      // иконка уведомлений (способ проверен на консоли)
    if (g_resumeSession.load()) load_session();
    if (g_saveTo.load() == 1) make_dirs(INTERNAL_ROOT);
    writer_start();
    if (!http_spawn()) {
        logf_("http: cannot start the web server thread");
        notify("ps4torrent: cannot start the web server");
        return 1;
    }
    net_init();
    if (g_listenPort > 0) {
        int lerr = 0;
        if (incoming_start(g_listenPort, &lerr)) {
            tracker_set_port(g_listenPort);
            logf_("incoming: listening on TCP port %d (forward it on the router to reach peers behind NAT)", g_listenPort);
        } else {
            logf_("incoming: cannot listen on port %d (errno %d), outgoing connections only", g_listenPort, lerr);
        }
    } else {
        logf_("incoming: disabled (listen_port=0)");
    }
    make_peer_id(g_peerId);
    logf_("max parallel downloads: %d", g_maxParallel);
    {
        std::string ip0 = local_ip();
        logf_("network: %s", ip0.empty() ? "not connected yet, the web server will be restarted when the connection appears"
                                          : ("local address " + ip0).c_str());
        notify(ip0.empty() ? "ps4torrent started, waiting for the network..." : "ps4torrent started");
    }
#ifdef HAVE_BLOB
    try { blob_selftest(); } catch (const std::exception& e) { logf_("blob: self-test failed: %s", e.what()); }
#endif

    time_t lastScan = 0, lastSnap = 0, lastBeat = time(NULL);
    std::map<std::string, time_t> slowLogged;     // чтобы строки о зависаниях не сыпались чаще раза в 5 секунд
    uint64_t lastLoopUs = 0;
    time_t lastLoopWall = 0;
    while (!g_quit) {
        try {
            time_t now = time(NULL);
            uint64_t lap = now_us();
            // Фиксирует в логе этап главного цикла, который занял больше 0,3 секунды (из-за таких этапов
            // кнопки на странице отвечают с задержкой).
            #define PHASE(name) do { uint64_t t_ = now_us(); if (t_ - lap > 300000) { \
                time_t& last_ = slowLogged[name]; if (now - last_ >= 5) { last_ = now; \
                logf_("slow main loop: %s took %llu ms", name, (unsigned long long)((t_ - lap) / 1000)); } } lap = t_; } while (0)

#ifdef HOST_TEST
            if (access("/tmp/ft/force_oom", F_OK) == 0) { unlink("/tmp/ft/force_oom"); throw std::bad_alloc(); }       // тесты: имитация нехватки памяти
#endif
#ifdef HOST_TEST
            {   // тесты: имитация сна консоли (главный цикл "замирает" на N секунд)
                FILE* ff = fopen("/tmp/ft/stall_main", "r");
                int n = 0;
                if (ff) { if (fscanf(ff, "%d", &n) != 1) n = 0; fclose(ff); }
                if (n > 0) { remove("/tmp/ft/stall_main"); sleep((unsigned)n); now = time(NULL); }
            }
#endif
            // Главный цикл не работал дольше LOOP_GAP_S: консоль спала или диск завис. После этого слушающие сокеты
            // могли сломаться, поэтому пересоздаём их (и тут же перепроверяем сеть).
            {
                uint64_t nowUs = now_us();
                long gap = 0, gapMono = 0, gapWall = 0;
                if (lastLoopUs != 0) {
                    gapMono = (long)((nowUs - lastLoopUs) / 1000000);
                    gapWall = (long)(now - lastLoopWall);
                    gap = gapMono;
                    if (gapWall > gap) gap = gapWall;
                }
                lastLoopUs = nowUs;
                lastLoopWall = now;
                if (gap >= LOOP_GAP_S) {
                    // Оба хода времени пишем в лог: если часы консоли после сна идут иначе (скачок назад или вперёд),
                    // это будет видно (wall: системные часы, monotonic: счётчик без скачков).
                    logf_("pause: the program was not running for %ld s (console sleep or a stuck drive; monotonic %lld s, wall clock %lld s), refreshing the network listeners",
                          gap, (long long)gapMono, (long long)gapWall);
                    restart_listeners(g_netIp, "after a pause", false);
                    recover_after_pause(true);
                    g_netLastCheck = 0;               // сеть перепроверим сразу
                }
                lap = now_us();
            }
            net_watch(now);
            if (incoming_take_broken() && g_listenPort > 0) {
                int e = 0;
                incoming_stop();
                if (incoming_start(g_listenPort, &e)) { tracker_set_port(g_listenPort); logf_("incoming: listener was broken, created a new one"); }
                else logf_("incoming: cannot create a new listener (errno %d)", e);
            }
            PHASE("network");
            reap_zombies(now);
            size_t ncmd = apply_commands(now);
        for (size_t k = 0; k < g_items.size(); k++) try_apply_selection(*g_items[k], now);       // отложенная смена выбора файлов
            PHASE("commands");
            process_deletions();
            PHASE("deletions");
            if (lastScan == 0 || now - lastScan >= 10 || g_rescan) { g_rescan = false; lastScan = now; scan_usb(now); }
            PHASE("scan_usb");

            run_downloads(now);
            PHASE("downloads");
            if (g_active.empty()) usleep(20000);

            // Состояние обновляем раз в секунду и сразу после любой команды.
            if (ncmd > 0 || now != lastSnap) {
                if (now != lastSnap) {
                    // Новая копия программы просит эту завершиться (обновление или перезапуск): создаёт файл-флаг.
                    struct stat qs;
                    if (stat(QUIT_FLAG_FILE, &qs) == 0) {
#ifdef HOST_TEST
                        if (stat("/tmp/ft/ignore_quit_flag", &qs) != 0)
#endif
                        {
                            remove(QUIT_FLAG_FILE);
                            logf_("another copy of the program asked this one to quit (restart or update)");
                            g_quit = true;
                        }
                    }
                }
                lastSnap = now;
                build_snapshot();
            }
            PHASE("snapshot");
            session_sync(now, false);
            // Команды, пришедшие до этого момента, теперь отражены в состоянии: HTTP-запросы могут отвечать.
            pthread_mutex_lock(&g_mu);
            g_cmdDone = g_cmdTaken;
            pthread_mutex_unlock(&g_mu);

            if (now - lastBeat >= 60) {
                lastBeat = now;
                int sp = 0;
                for (size_t i = 0; i < g_active.size(); i++) sp += g_active[i]->it->speedKB;
                struct rusage ru;
                long rss = 0;
                if (getrusage(RUSAGE_SELF, &ru) == 0) rss = ru.ru_maxrss;
                logf_("alive: %d torrents, %d active, %d KB/s, peak memory %ld KB, incoming: %u accepted %u rejected, piece buffers %u MB", (int)g_items.size(),
                      (int)g_active.size(), sp, rss, incoming_accepted(), incoming_rejected(), (unsigned)(bigbuf_total().load() >> 20));
            }
            #undef PHASE
        } catch (const std::bad_alloc&) {
            // Не хватило памяти (на консоли обычная куча ~10 МБ): не останавливаем программу, а сбрасываем все соединения с пирами
            // (их буферы самые большие) и продолжаем. Если так повторяется снова и снова, тогда останавливаемся.
            static int oomCount = 0;
            static time_t oomFirst = 0;
            time_t tn = time(NULL);
            if (oomFirst == 0 || tn - oomFirst > 600) { oomFirst = tn; oomCount = 0; }
            oomCount++;
            logf_("!!! OUT OF MEMORY (std::bad_alloc) #%d: all peer connections are dropped to free memory, the program continues", oomCount);
            for (size_t i = 0; i < g_active.size(); i++) if (g_active[i]->sw) g_active[i]->sw->trim_memory();
            if (oomCount == 1) notify("ps4torrent: low memory, peer connections were reset");
            if (oomCount >= 5) {
                logf_("!!! out of memory again and again -> stopping cleanly");
                notify("ps4torrent: out of memory (stopping)");
                g_quit = true;
            }
        } catch (const std::exception& e) {
            logf_("!!! EXCEPTION in main loop: %s -> stopping cleanly", e.what());
            notify("ps4torrent error: %s (stopping)", e.what());
            g_quit = true;
        } catch (...) {
            logf_("!!! UNKNOWN EXCEPTION in main loop -> stopping cleanly");
            g_quit = true;
        }
    }

    // Остановка: сохраняем прогресс и закрываем файлы.
    time_t now = time(NULL);
    while (!g_active.empty()) close_job(g_active.back(), now, true);
    session_sync(time(NULL), true);      // что качалось на момент остановки (запись идёт через поток записи)
    // Дать дописать уже проверенные куски на диск (до 8 секунд), чтобы они не пропали при остановке.
    for (int w = 0; w < 160 && !g_zombies.empty(); w++) { reap_zombies(time(NULL)); usleep(50000); }
    writer_stop(8000);
    reap_zombies(time(NULL));
    if (g_srvStarted) pthread_join(g_srvThread, NULL);
    for (int w = 0; w < 300 && g_clients.load() > 0; w++) usleep(10000);     // дать допоказать ответы (до 3 с)
    incoming_stop();
    if (g_srv.load() >= 0) close(g_srv.load());
    for (size_t i = 0; i < g_items.size(); i++) delete g_items[i];
    g_items.clear();
    logf_("---- ps4torrentd stop ----");
    notify("ps4torrent stopped");
    return 0;
}
