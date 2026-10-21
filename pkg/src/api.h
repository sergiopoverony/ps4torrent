#pragma once
#include <string>

// Обновляет g_status, обратившись к GET /status. Возвращает true при успехе
// (соединились, получили и разобрали JSON). При неудаче g_status.ok = false,
// но старые данные в g_status.items не стираются — экран продолжает показывать
// последнее известное состояние, а не пустой список.
bool api_refresh_status();

// Пауза/продолжить одну раздачу или все сразу.
bool api_pause(const std::string& hash);
bool api_resume(const std::string& hash);
bool api_pause_all();
bool api_resume_all();

// Удалить раздачу: withFiles=true также стирает скачанные файлы (нужен полный хэш).
bool api_delete(const std::string& hash, bool withFiles);

// Выключить ps4torrentd (GET /quit): демон сохраняет прогресс и завершается.
// Запустить снова можно только повторной отправкой payload.
bool api_quit();

// Последняя ошибка от сервера (для подсказки на экране), пусто если её не было.
std::string api_last_error();
