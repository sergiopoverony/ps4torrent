import socket, time, urllib.request, statistics, sys
label = sys.argv[1]
idle = []
for i in range(6):
    s = socket.create_connection(("127.0.0.1", 18787), timeout=5); idle.append(s)       # как заготовленные соединения браузера: открыто, но молчит
times = []
for i in range(5):
    t = time.time(); urllib.request.urlopen("http://127.0.0.1:18787/status", timeout=15).read(); times.append(time.time() - t)
    time.sleep(0.3)
print("%-4s /status with 6 idle connections open: median %.2fs  max %.2fs" % (label, statistics.median(times), max(times)))
for s in idle: s.close()
