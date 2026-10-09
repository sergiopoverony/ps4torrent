#!/bin/bash
cd /tmp/ft; pkill -9 -x daemon_tsan 2>/dev/null
bash seeder_ctl.sh stop; bash seeder_ctl.sh start 0.01 0 0 0 1       # трекер отвечает 1 с: фоновые запросы реально идут во время работы
rm -rf usb0 usb1 log; mkdir -p usb0/torrents log; cp torrents_src/Big8M.torrent torrents_src/Small.torrent usb0/torrents/
TSAN_OPTIONS="halt_on_error=0 second_deadlock_stack=1 log_path=/tmp/ft/tsan_report" setsid ./daemon_tsan > tsan.out 2>&1 < /dev/null &
sleep 3
python3 - << 'PY' > tsan_client.result 2>&1
import urllib.request, json, time, threading, socket
B = "http://127.0.0.1:18787"; H = "6b5d373445b803f9bb3630e88ae74f1617438cc2"
def get(p):
    try: return urllib.request.urlopen(B + p, timeout=10).read()
    except Exception as e: return b""
stop = [False]
def poller():                       # как страница: опрос состояния
    while not stop[0]: get("/status"); time.sleep(0.05)
def idle():                         # пустые соединения
    while not stop[0]:
        try:
            s = socket.create_connection(("127.0.0.1", 18787), timeout=3); time.sleep(0.3); s.close()
        except Exception: pass
ts = [threading.Thread(target=poller) for _ in range(3)] + [threading.Thread(target=idle) for _ in range(2)]
for t in ts: t.start()
small = open("/tmp/ft/torrents_src/Small.torrent", "rb").read()
for i in range(6):
    get("/api/pause?hash=" + H); time.sleep(0.4); get("/api/resume?hash=" + H); time.sleep(0.8)
    get("/api/config"); get("/api/set?max_parallel=%d" % (2 + i % 3))
    r = urllib.request.Request(B + "/api/add?name=t%d.torrent" % i, data=small, method="POST")
    try: urllib.request.urlopen(r, timeout=10).read()
    except Exception: pass
time.sleep(3); stop[0] = True
for t in ts: t.join()
print("client done")
PY
curl -s -m 3 localhost:18787/quit >/dev/null; sleep 4; pkill -9 -x daemon_tsan 2>/dev/null
echo done > tsan.done
