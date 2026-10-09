import urllib.request, json, time
B = "http://127.0.0.1:18787"
def status(): return json.load(urllib.request.urlopen(B + "/status", timeout=5))
def log(): return open("/tmp/ft/log/log.txt").read()
res = []
def check(n, ok): res.append(ok); print(("PASS " if ok else "FAIL ") + n, flush=True)
check("network never appears: no restart in the first seconds", (time.sleep(6) or status()["net_restarts"]) == 0)
t = time.time()
while time.time() - t < 20 and status()["net_restarts"] == 0: time.sleep(0.5)
check("after the fallback time the listeners are restarted anyway (restarts=%d)" % status()["net_restarts"], status()["net_restarts"] == 1)
check("log explains it", "no address detected for 12 s" in log())
check("web server answers", status()["version"] != "")
time.sleep(14)
check("it does not keep restarting afterwards", status()["net_restarts"] == 1)
print("RESULT: %d/%d passed" % (sum(res), len(res)))
