#pragma once
#include <string>
#include <vector>
#include <stdint.h>

struct TorrentFile {
    std::string path;      // относительный путь внутри раздачи
    uint64_t length;
};

struct Torrent {
    std::string name;
    uint64_t totalSize = 0;
    uint32_t pieceLength = 0;
    uint32_t numPieces = 0;
    std::string pieces;    // 20 байт SHA-1 на каждый кусок
    unsigned char infoHash[20];
    std::string infoHashHex;
    std::vector<std::string> trackers;
    std::vector<TorrentFile> files;
};

bool load_torrent(const char* path, Torrent& t, std::string& err);