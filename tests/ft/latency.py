import urllib.request, json, time, statistics, sys
B = "http://127.0.0.1:18787"
H = "6b5d373445b803f9bb3630e88ae74f1617438cc2"            # Big8M
def status():
    return json.load(urllib.request.urlopen(B + "/status", timeout=10))["items"][0]["status"]
def click(cmd):
    t0 = time.time(); urllib.request.urlopen(B + "/api/%s?hash=%s" % (cmd, H), timeout=10).read()
    t_resp = time.time() - t0
    st0 = status()
    # как скоро /status покажет новое состояние
    while True:
        st = status()
        if (cmd == "pause" and st == "paused") or (cmd == "resume" and st != "paused"): break
        time.sleep(0.02)
        if time.time() - t0 > 20: break
    return t_resp, time.time() - t0
label = sys.argv[1]
time.sleep(8)                                          # раздача запустилась
res = {"pause": [], "resume": []}
for i in range(5):
    for cmd in ("pause", "resume"):
        r, v = click(cmd); res[cmd].append((r, v)); time.sleep(1.3)
for cmd in ("pause", "resume"):
    rs = [x[0] for x in res[cmd]]; vs = [x[1] for x in res[cmd]]
    print("%-8s %-7s HTTP answer: median %.2fs max %.2fs | until /status shows it: min %.2fs median %.2fs max %.2fs" % (label, cmd, statistics.median(rs), max(rs), min(vs), statistics.median(vs), max(vs)))
