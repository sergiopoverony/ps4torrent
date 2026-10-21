#pragma once
#include <stdint.h>
#include <string>

// Что писать в шапке вместо IP-адреса.
enum ConnState {
    CONN_OK,            // сервер отвечает и есть настоящий адрес в сети: показываем IP:port
    CONN_NO_INTERNET,   // сервер отвечает (127.0.0.1), но сети нет: NO INTERNET
    CONN_NO_PAYLOAD     // сервер (payload) не отвечает: NO PAYLOAD
};

// Сколько ждать, прежде чем признать, что payload пропал.
// ps4torrentd при смене состояния сети на время пересоздаёт свой веб-сокет (до нескольких секунд,
// проверка сети у него каждые 5 с), и порт 8787 в эти моменты может недолго не отвечать.
// Без запаса шапка при каждом таком промахе мигала бы надписью NO PAYLOAD.
static const uint64_t CONN_GRACE_US = 10ULL * 1000000ULL;

// lastOkUs: время (микросекунды) последнего УСПЕШНОГО ответа, 0 если ещё ни разу не было.
// netIp: последний известный адрес консоли (при неудачном опросе это данные прошлого успешного ответа).
inline ConnState conn_state(bool statusOk, uint64_t lastOkUs, uint64_t nowUs,
                            const std::string& netIp, uint64_t graceUs = CONN_GRACE_US)
{
    bool payloadDown = !statusOk && (lastOkUs == 0 || nowUs - lastOkUs > graceUs);
    if (payloadDown) return CONN_NO_PAYLOAD;

    bool hasExternalIp = !netIp.empty() && netIp != "127.0.0.1";
    return hasExternalIp ? CONN_OK : CONN_NO_INTERNET;
}
