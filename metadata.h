#pragma once
// Получение описания раздачи (info-словаря) по одному info hash: трекер -> пиры -> BEP 10 / BEP 9 (ut_metadata).
// DHT не используется: пиры берутся только у HTTP-трекеров.
#include <string>
#include <vector>

// Блокирующая функция (запускать в отдельном потоке). budgetSec: общий лимит времени. should_stop() (можно NULL) опрашивается
// во время работы. true: info содержит проверенный по SHA-1 словарь info; false: err с причиной.
bool metadata_fetch(const unsigned char infoHash[20], const std::vector<std::string>& trackers, int budgetSec,
                    bool (*should_stop)(), std::string& info, std::string& err);

// Предел размера описания: в 1 МБ укладывается любой нормальный .torrent (веб-добавление тоже принимает не больше 1 МБ).
#define METADATA_MAX_BYTES (1u * 1024 * 1024)
