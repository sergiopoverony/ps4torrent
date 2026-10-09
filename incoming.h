#pragma once
// Приём входящих соединений от пиров (порт BitTorrent, по умолчанию 6881).
//
// Зачем: большинство пиров сидит за NAT, и мы не можем к ним подключиться. Зато они сами могут
// подключиться к нам, если на роутере проброшен порт на консоль. Это даёт намного больше пиров.
//
// Как работает: слушающий сокет + список "ожидающих handshake" соединений. Как только пир прислал
// handshake (68 байт), по info_hash находим раздачу, которая сейчас качается, и передаём ей сокет.
// Незнакомые, битые и слишком медленные соединения закрываются.

#include <sys/select.h>
#include <string>
#include <time.h>

// Возвращает: 1 раздача приняла соединение, 2 приняла, но сразу закрыла (плохой пир / дубликат),
// 0 не приняла (сокет остаётся у вызывающего, он его закроет).
typedef int (*IncomingAdoptFn)(const unsigned char infoHash[20], int fd, const std::string& key,
                               const std::string& initial);

bool incoming_start(int port, int* err);        // false: слушать не получилось (err = errno)
void incoming_stop();
int  incoming_prepare(fd_set& rf);              // добавляет сокеты в select, возвращает наибольший fd или -1
void incoming_process(time_t now, fd_set& rf, IncomingAdoptFn adopt);
int  incoming_port();                           // 0, если не слушаем
bool incoming_take_broken();                    // true один раз, если слушающий сокет сломался (надо создать заново)
unsigned incoming_accepted();                   // сколько входящих принято раздачами
unsigned incoming_rejected();                   // сколько отклонено
