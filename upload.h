#pragma once
// Отдача (upload): общие настройки и ограничитель скорости на все раздачи сразу. Всё вызывается только из главного потока.
#include <stdint.h>
#include <stddef.h>

// Допустимые пределы скорости отдачи, КБ/с (семь ступеней; 32 по умолчанию).
extern const int UPLOAD_STEPS[7];
#define UPLOAD_DEFAULT_KB 32

void upload_set(bool enabled, int limitKB);       // limitKB приводится к ближайшей ступени
bool upload_enabled();
int  upload_limit_kb();
int  upload_snap_kb(int kb);                      // ближайшая допустимая ступень

// Ограничитель: пополняется по времени; сколько байт можно отправить прямо сейчас.
size_t upload_available();
void   upload_consume(size_t n);

// Статистика: всего отдано за сессию и текущая скорость (сглаженная), КБ/с.
uint64_t upload_total_bytes();
int      upload_speed_kb();
void     upload_tick();                           // раз за проход главного цикла: пересчёт скорости
