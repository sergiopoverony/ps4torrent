#pragma once
// Установка и обновление экранного клиента (PKG) из релиза GitHub.
// Скачивает пакет в /data/pkg и просит консоль поставить его (BGFT) без вопросов.
#include <string>

void clientinst_start(int httpPort);                 // запускает фоновый поток (один раз)
std::string clientinst_json();                       // состояние для страницы: {"state":...}
void clientinst_request_install();                   // кнопка «установить / обновить клиент»
