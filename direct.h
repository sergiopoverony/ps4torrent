#pragma once
// Прямые загрузки по ссылке http:// (не торрент). Список живёт в главном потоке; каждая идущая загрузка качается
// в своём потоке (обычные блокирующие сокеты и запись в файл), главный цикл только запускает, останавливает и
// показывает их. Ссылка на .torrent сюда не попадает: страница сначала спрашивает direct_probe, и торрент
// добавляется обычным путём (/api/add).

#include <string>
#include <stdint.h>
#include <time.h>
#include <vector>

struct UrlProbe {
    bool torrent = false;       // по ссылке лежит .torrent (тогда body = его содержимое)
    std::string body;
    std::string name;           // имя файла (из Content-Disposition или из адреса)
    uint64_t size = 0;
    bool sizeKnown = false;
    std::string finalUrl;       // адрес после переадресаций
    std::string error;
};

// Проверка ссылки (любой поток): переадресации, имя, размер; маленький .torrent читается целиком (не больше maxTorrent).
bool direct_probe(const std::string& url, size_t maxTorrent, UrlProbe& out);
bool direct_url_ok(const std::string& url, std::string& err);       // только http://, без пробелов и управляющих символов
bool direct_has_url(const std::string& url);                         // уже есть в списке и не докачана (любой поток)

// Только главный поток:
void direct_load();
void direct_add(const std::string& root, const std::string& url, const std::string& name, uint64_t size, bool sizeKnown, bool start);
bool direct_command(const std::string& cmd, const std::string& id);   // pause/resume/delete/delete_files; false: не наша задача
void direct_all(bool pause);
void direct_tick(time_t now, std::vector<std::string>& finished);   // finished: имена докачанных за этот проход
std::string direct_items_json(int& speedKB);                          // элементы для "items" через запятую (без скобок)
bool direct_running();                                                // хоть одна загрузка идёт (защита от сна)
void direct_shutdown();
