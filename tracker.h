#pragma once
#include <string>
#include <vector>
#include <stdint.h>
#include "torrent.h"

struct Peer {
    unsigned char ip[4];
    uint16_t port;
};

struct AnnounceResult {
    std::string error;
    int interval = 0;
    std::vector<Peer> peers;
};

bool tracker_announce_http(const std::string& url, const unsigned char infoHash[20],
                           const unsigned char peerId[20], uint64_t left,
                           AnnounceResult& res);

// Порт, который сообщаем трекеру (на него пиры смогут к нам подключаться).
void tracker_set_port(int port);
