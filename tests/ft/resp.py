import urllib.request, time, json, sys, statistics
B = "http://127.0.0.1:18787"; label = sys.argv[1]; duration = int(sys.argv[2])
lat = []; t0 = time.time(); fails = 0; last_done = 0
while time.time() - t0 < duration:
    t = time.time()
    try:
        # команда, которую выполняет главный цикл: измеряем, насколько он отзывчив
        urllib.request.urlopen(B + "/api/set?max_parallel=3", timeout=15).read()
        lat.append(time.time() - t)
    except Exception: fails += 1
    time.sleep(1.5)
try:
    d = json.load(urllib.request.urlopen(B + "/status", timeout=10)); last_done = d["items"][0]["done"]; pct = d["items"][0]["pct"]
except Exception: pct = -1
lat.sort()
print("%-10s main-loop response over %ds: %d commands, median %.2fs, 90%% %.2fs, WORST %.2fs, no answer %d | download reached %d%% (%.0f MB)" %
      (label, duration, len(lat), statistics.median(lat), lat[int(len(lat) * 0.9)] if lat else 0, max(lat) if lat else 0, fails, pct, last_done / 1048576))
