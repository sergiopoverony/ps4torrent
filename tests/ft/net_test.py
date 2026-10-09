import urllib.request, json, time, os, socket, threading, subprocess
B = "http://127.0.0.1:18787"; FT = "/tmp/ft"; MOCK = FT + "/netmock"
res = []
def check(name, ok): res.append(ok); print(("PASS " if ok else "FAIL ") + name, flush=True)
def status():
    return json.load(urllib.request.urlopen(B + "/status", timeout=5))
def log(): return open(FT + "/log/log.txt").read()
def setnet(ip): open(MOCK, "w").write(ip)
def wait(cond, secs):
    t = time.time()
    while time.time() - t < secs:
        try:
            if cond(): return True
        except Exception: pass
        time.sleep(0.2)
    return False
def can_connect(p):
    try: socket.create_connection(("127.0.0.1", p), timeout=2).close(); return True
    except Exception: return False

print("== A. started while the network is DOWN (like autostart right after wake)")
d = status()
check("web server is up from the start (bound early), net_ip empty, 0 restarts: %s/%s" % (repr(d["net_ip"]), d["net_restarts"]), d["net_ip"] == "" and d["net_restarts"] == 0)
check("log says the web server will be restarted when the connection appears", "not connected yet" in log())

print("== B. the network comes up")
setnet("192.168.0.24")
check("listeners are restarted within a few seconds", wait(lambda: status()["net_restarts"] == 1, 8))
d = status(); check("status shows the address: %s" % d["net_ip"], d["net_ip"] == "192.168.0.24")
check("log: 'local address 192.168.0.24' and 'listeners restarted (connection is up)'", "local address 192.168.0.24" in log() and "listeners restarted (connection is up)" in log())
check("web server answers right after the restart", status()["version"] != "")
check("incoming peer port 26881 accepts connections after the restart", can_connect(26881))
check("log shows the first HTTP connections", "http: connection #1 from" in log())

print("== C. the console's address changes (DHCP)")
setnet("192.168.0.77")
check("listeners restarted again (address changed)", wait(lambda: status()["net_restarts"] == 2, 10))
check("log says '(changed)'", "(changed)" in log())

print("== D. connection lost and back")
setnet("")
check("lost connection is noticed", wait(lambda: "connection lost" in log(), 10))
r_before = status()["net_restarts"]; time.sleep(15)
check("no pointless restarts while the network is down (waited longer than the 12 s fallback)", status()["net_restarts"] == r_before and "no address detected" not in log())
setnet("192.168.0.77")
check("restarted when the connection returns", wait(lambda: status()["net_restarts"] == r_before + 1, 8))

print("== D2. a restart fails twice (simulated): the program must keep retrying until it works")
open("/tmp/ft/rebind_fail", "w").write("2")
n_before = status()["net_restarts"]
setnet("172.16.0.9")
check("it succeeds on the third try", wait(lambda: status()["net_restarts"] == n_before + 1, 20))
check("log shows two failures and then the restart", log().count("cannot restart the web server") >= 2)
check("web server answers", status()["net_ip"] == "172.16.0.9")
os.remove("/tmp/ft/rebind_fail")

print("== E. restarts while clients keep polling")
stop = [False]; fails = [0]; total = [0]
def poller():
    while not stop[0]:
        total[0] += 1
        try: urllib.request.urlopen(B + "/status", timeout=3).read()
        except Exception: fails[0] += 1
        time.sleep(0.01)
ts = [threading.Thread(target=poller) for _ in range(4)]
for t in ts: t.start()
n0 = status()["net_restarts"]
for i, ip in enumerate(("10.0.0.5", "10.0.0.6", "10.0.0.7")):
    setnet(ip); wait(lambda: status()["net_restarts"] == n0 + i + 1, 10); time.sleep(0.5)
stop[0] = True
for t in ts: t.join()
print("   %d requests during 3 restarts, %d failed (a restart briefly closes the port)" % (total[0], fails[0]))
check("server keeps working: at most a handful of requests hit the tiny gap (%d of %d)" % (fails[0], total[0]), fails[0] <= 6 and total[0] > 300)
check("still alive and answering afterwards", status()["net_restarts"] == n0 + 3)
print("RESULT: %d/%d passed" % (sum(res), len(res)))
