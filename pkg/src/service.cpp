#include "service.h"
#include "net.h"
#include "log.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>

static const char* SERVICE_HOST = "127.0.0.1";
static const int   LOADER_PORT  = 9090;
static const char* PAYLOAD_PATH = "/app0/assets/misc/ps4torrentd.elf";

// Два разных пути для одного и того же файла (проверено на ps4hddtools):
//  - AUTORUN_ELF_PATH: под этим путём приложение реально может писать (fopen/mkdir); "/user/data/payloads/..." из
//    песочницы приложения для записи недоступен;
//  - AUTORUN_INI_ENTRY: этот путь (с "/user") GoldHEN ожидает в секции [AutoRun] файла payloads.ini.
static const char* AUTORUN_ELF_PATH  = "/data/payloads/ps4torrentd.elf";
static const char* AUTORUN_INI_ENTRY = "/user/data/payloads/ps4torrentd.elf";
static const char* AUTORUN_INI_PATH  = "/data/GoldHEN/payloads.ini";

static bool read_payload(std::string& data, std::string& err)
{
    FILE* f = fopen(PAYLOAD_PATH, "rb");
    if (!f) { err = "no payload file in PKG"; logf_("service: cannot open %s", PAYLOAD_PATH); return false; }
    char buf[16384];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) data.append(buf, n);
    fclose(f);
    if (data.size() < 1024) { err = "payload file is empty"; return false; }
    return true;
}

bool service_install_autorun(std::string& err)
{
    std::string data;
    if (!read_payload(data, err)) return false;

    mkdir("/data/payloads", 0777);   // если папка уже есть, mkdir вернёт ошибку: это не страшно
    FILE* out = fopen(AUTORUN_ELF_PATH, "wb");
    if (!out) { err = std::string("cannot write ") + AUTORUN_ELF_PATH; logf_("autorun: %s", err.c_str()); return false; }
    size_t wn = fwrite(data.data(), 1, data.size(), out);
    fclose(out);
    if (wn != data.size()) { err = "autorun payload write incomplete"; logf_("autorun: %s", err.c_str()); return false; }

    std::string ini;
    FILE* fi = fopen(AUTORUN_INI_PATH, "rb");
    if (fi) {
        char ibuf[4096];
        size_t rn;
        while ((rn = fread(ibuf, 1, sizeof(ibuf), fi)) > 0) ini.append(ibuf, rn);
        fclose(fi);
    }

    // Строка про наш файл уже есть (с любым значением, "= 1" или "= 0" и с любым путём): это осознанный выбор
    // пользователя, файл ini не трогаем (никаких дублей).
    if (ini.find("ps4torrentd.elf") != std::string::npos) {
        logf_("autorun: payload copied, payloads.ini already lists ps4torrentd.elf - leaving it as is");
        return true;
    }

    std::string entry = std::string(AUTORUN_INI_ENTRY) + " = 1\n";
    size_t sec = ini.find("[AutoRun]");
    if (sec == std::string::npos) {
        if (!ini.empty() && ini.back() != '\n') ini += "\n";
        if (!ini.empty()) ini += "\n";
        ini += "[AutoRun]\n";
        ini += entry;
    } else {
        size_t lineEnd = ini.find('\n', sec);
        if (lineEnd == std::string::npos) { ini += "\n"; lineEnd = ini.size() - 1; }   // [AutoRun] последняя строка без перевода строки
        ini.insert(lineEnd + 1, entry);
    }

    FILE* fw = fopen(AUTORUN_INI_PATH, "wb");
    if (!fw) { err = std::string("cannot write ") + AUTORUN_INI_PATH; logf_("autorun: %s", err.c_str()); return false; }
    fwrite(ini.data(), 1, ini.size(), fw);
    fclose(fw);
    logf_("autorun: payload installed and registered in payloads.ini (%s)", AUTORUN_INI_ENTRY);
    return true;
}

bool service_send_payload(std::string& err, bool* connectFailed)
{
    if (connectFailed) *connectFailed = false;

    std::string data;
    if (!read_payload(data, err)) return false;

    int s = net_connect(SERVICE_HOST, LOADER_PORT, 3000);
    if (s < 0) {
        err = "BinLoader (port 9090) is not available. Is GoldHEN running?";
        logf_("service: BinLoader connect failed");
        if (connectFailed) *connectFailed = true;
        return false;
    }

    struct timeval tv;                       // отправка ~1,4 МБ: даём BinLoader время (по умолчанию net_connect ставит 3 с)
    tv.tv_sec = 20; tv.tv_usec = 0;
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    size_t sent = 0;
    while (sent < data.size()) {
        int w = send(s, data.data() + sent, data.size() - sent, 0);
        if (w <= 0) {
            err = "sending to BinLoader failed";
            logf_("service: send failed after %d of %d bytes", (int)sent, (int)data.size());
            close(s);
            return false;
        }
        sent += (size_t)w;
    }
    close(s);
    logf_("service: payload sent (%d bytes)", (int)sent);
    return true;
}
