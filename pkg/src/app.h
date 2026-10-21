#pragma once
#include <string>
#include <vector>
#include <stdint.h>

// Одна раздача, как её видит клиент (поля ровно из ответа ps4torrentd /status).
struct Item {
    std::string hash;      // полный 40-символьный хэш (нужен для /api/delete)
    std::string status;    // "queued" | "checking" | "downloading" | "paused" | "complete" | "offline" | "waiting"
    uint64_t size = 0;
    uint64_t done = 0;
    int pct = 0;
    int speedKB = 0;
    long long etaSec = -1;  // -1 неизвестно, 0 готово
    int peers = 0;
    bool paused = false;
    bool lowSpace = false;
    std::string root;       // куда качается (для информации)
    std::string title;
};

// Общее состояние сервера (хвост /status, без списка раздач).
struct Drive {
    std::string root;   // "/mnt/usb0/torrents" или "/data/pkg/torrents"
    uint64_t freeBytes = 0;
    uint64_t totalBytes = 0;
    bool isInternal = false;
    bool low = false;
};

struct ServerStatus {
    bool ok = false;         // удалось получить и разобрать ответ
    std::vector<Item> items;
    std::vector<Drive> drives;
    int totalSpeedKB = 0;
    int maxParallel = 0;
    int listenPort = 0;
    unsigned incoming = 0;
    int deleting = 0;
    std::string netIp;
    std::string version;
};

extern ServerStatus g_status;
extern std::string g_host;     // обычно "127.0.0.1"
extern int g_port;             // обычно 8787
extern std::string g_token;    // необязательный пароль, см. /data/ps4tc_client/config.txt

// Когда последний раз успешно получили /status (0, если ни разу).
extern uint64_t g_lastOkUs;
