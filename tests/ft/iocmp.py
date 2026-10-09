import urllib.request, json, time, sys, statistics
B = "http://127.0.0.1:18787"
label = sys.argv[1]
lat = []; t0 = time.time(); last = None
while time.time() - t0 < 75:
    t = time.time()
    try:
        urllib.request.urlopen(B + "/api/set?max_parallel=3", timeout=40).read()    # применяется главным циклом: измеряет его отзывчивость
        lat.append(time.time() - t)
    except Exception: lat.append(40.0)
    try: last = json.load(urllib.request.urlopen(B + "/status", timeout=40))["items"][0]
    except Exception: pass
    time.sleep(2)
print("%-26s main-loop responsiveness: %2d probes, median %5.2f s, 90th pct %5.2f s, WORST %5.2f s | progress after 75 s: %s %d%%" % (
      label, len(lat), statistics.median(lat), sorted(lat)[int(len(lat)*0.9)-1], max(lat), last["status"] if last else "?", last["pct"] if last else -1))
