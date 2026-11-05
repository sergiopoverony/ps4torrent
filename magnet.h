#pragma once
// Magnet-ссылки: разбор и сборка .torrent из полученного описания раздачи (info).
#include <string>
#include <vector>

struct Magnet {
    unsigned char hash[20];
    std::string hex;                     // info hash, 40 символов a-f0-9 (нижний регистр)
    std::string name;                    // dn= (может быть пустым)
    std::vector<std::string> trackers;   // tr= (только http://)
};

// magnet:?xt=urn:btih:<40 hex или 32 base32>&tr=...&dn=... ; false и текст ошибки, если ссылка негодная.
bool magnet_parse(const std::string& uri, Magnet& m, std::string& err);

// Собирает содержимое файла .torrent: announce, announce-list и info (байты info берутся как есть, чтобы хэш не изменился).
std::string magnet_build_torrent(const std::string& infoRaw, const std::vector<std::string>& trackers);

// Трекер, который добавляется, если в ссылке трекеров нет (рутрекер отдаёт пиров для magnet без личного ключа).
extern const char* const MAGNET_DEFAULT_TRACKER;
