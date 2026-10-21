#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <sys/stat.h>
#include <orbis/libkernel.h>

#include "log.h"
#include "net.h"
#include "app.h"
#include "api.h"
#include "ui.h"
#include "i18n.h"

// Системная заставка запуска (sce_sys/pic1.png) остаётся поверх приложения,
// пока оно само её не уберёт. Вызываем после первого нарисованного кадра.
extern "C" int sceSystemServiceHideSplashScreen(void);

static const char* CONFIG_FILE = "/data/ps4tc_client/config.txt";

ServerStatus g_status;
std::string  g_host  = "127.0.0.1";
int          g_port  = 8787;
std::string  g_token;
uint64_t     g_lastOkUs = 0;

static std::string trim(const std::string& s)
{
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) a++;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) b--;
    return s.substr(a, b - a);
}

// config.txt необязателен. Строки: host=..., port=..., token=...
// По умолчанию клиент обращается к ps4torrentd на этой же консоли (127.0.0.1:8787).
static void load_config()
{
    FILE* f = fopen(CONFIG_FILE, "r");
    if (!f) { logf_("config: no %s, using defaults (%s:%d)", CONFIG_FILE, g_host.c_str(), g_port); return; }

    char line[400];
    while (fgets(line, sizeof(line), f)) {
        std::string l = trim(line);
        if (l.empty() || l[0] == '#') continue;
        size_t eq = l.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(l.substr(0, eq));
        std::string val = trim(l.substr(eq + 1));

        if (key == "host" && !val.empty()) g_host = val;
        else if (key == "port") { int p = atoi(val.c_str()); if (p > 0 && p < 65536) g_port = p; }
        else if (key == "token") g_token = val;
    }
    fclose(f);
    logf_("config: server %s:%d%s", g_host.c_str(), g_port, g_token.empty() ? "" : " (with token)");
}

int main()
{
    signal(SIGPIPE, SIG_IGN);

    mkdir("/data/ps4tc_client", 0777);
    log_reset();
    logf_("---- ps4tc_client start ----");

    load_config();

    if (!net_init()) logf_("net_init FAILED: the client will not be able to reach ps4torrentd");

    i18n_init();
    ui_init();   // если экран не поднялся, приложение всё равно продолжает работать (видно по логу)

    bool splashHidden = false;
    for (;;) {
        ui_pump();
        if (!splashHidden) {
            splashHidden = true;
            int r = sceSystemServiceHideSplashScreen();
            logf_("sceSystemServiceHideSplashScreen = 0x%08X", (unsigned)r);
        }
        sceKernelUsleep(30000);
    }

    return 0;
}
