#pragma once
#include <string>

// Служба ps4torrentd (payload) живёт отдельно от приложения: приложение только запускает её
// (отправкой ELF в BinLoader GoldHEN на этой же консоли) и разговаривает с ней по HTTP.

// Записывает ELF из PKG (/app0/assets/misc/ps4torrentd.elf) в /data/payloads/ps4torrentd.elf (этот путь доступен для
// записи из песочницы приложения) и, если в /data/GoldHEN/payloads.ini ещё нет строки про ps4torrentd.elf, добавляет
// под [AutoRun] строку "/user/data/payloads/ps4torrentd.elf = 1" (именно с "/user": так GoldHEN находит файл).
// Если строка про ps4torrentd.elf уже есть (включена или выключена пользователем), файл ini не меняется.
bool service_install_autorun(std::string& err);

// Отправляет ELF из PKG в BinLoader GoldHEN (127.0.0.1:9090). Если старая копия службы ещё работает,
// новая сама попросит её завершиться. connectFailed (необязателен): true, если не удалось именно подключиться
// к порту 9090 (BinLoader выключен: GoldHEN - Servers settings - Enable Payloader Server).
bool service_send_payload(std::string& err, bool* connectFailed = nullptr);
