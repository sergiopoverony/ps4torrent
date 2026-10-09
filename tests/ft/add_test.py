import urllib.request, urllib.error, json, time, os, subprocess
B = "http://127.0.0.1:18787"; FT = "/tmp/ft"; U = FT + "/usb0/torrents"; I = FT + "/internal/torrents"
res = []
def check(name, ok): res.append(ok); print(("PASS " if ok else "FAIL ") + name, flush=True)
def status(): return json.load(urllib.request.urlopen(B + "/status", timeout=8))
def cfg(): return json.load(urllib.request.urlopen(B + "/api/config", timeout=8))
def get(p): return json.load(urllib.request.urlopen(B + p, timeout=15))
def add(name, extra=""):
    data = open(FT + "/torrents_src/" + name, "rb").read()
    r = urllib.request.Request(B + "/api/add?name=" + urllib.request.quote(name) + extra, data=data, method="POST")
    try: return json.load(urllib.request.urlopen(r, timeout=15))
    except urllib.error.HTTPError as e: return json.loads(e.read())
def items(): return {i["title"]: i for i in status()["items"]}
def log(): return open(FT + "/log/log.txt").read()
def wait(cond, secs):
    t0 = time.time()
    while time.time() - t0 < secs:
        try:
            if cond(): return True
        except Exception: pass
        time.sleep(0.3)
    return False

print("== defaults and the page")
check("default save_to is the console memory: %s" % cfg().get("save_to"), cfg().get("save_to") == "internal")
page = urllib.request.urlopen(B + "/", timeout=5).read().decode()
check("the page has the add dialog with a Download button and no storage selector in settings", 'id="dgo"' in page and ">Download<" in page and "save new torrents to" not in page)

print("== Download = start now, even though automatic start is OFF")
r = add("Small.torrent", "&drive=internal&start=1")
check("reply: ok, started=true, placed in the console memory: %s" % r, r.get("ok") and r.get("started") is True and r["root"] == I)
check("the folder in the console memory was created", os.path.isdir(I + "/downloads"))
check("the task starts by itself and completes (autostart is off)", wait(lambda: items().get("Small", {}).get("status") == "complete", 30))
check("log marks it '[starting: requested]'", "[starting: requested]" in log())

print("== 'start right away' unchecked: added paused")
r = add("Many.torrent", "&drive=internal&start=0")
check("reply: started=false", r.get("ok") and r.get("started") is False)
check("it waits on 'paused'", wait(lambda: items().get("Many", {}).get("status") == "paused", 15))
time.sleep(6)
check("and stays paused (nothing downloaded)", items()["Many"]["status"] == "paused" and not os.path.exists(I + "/downloads/Many"))
get("/api/resume?hash=" + items()["Many"]["hash"])
check("pressing resume later works", wait(lambda: items().get("Many", {}).get("status") == "complete", 40))

print("== choosing the USB drive")
r = add("GameOnePart.torrent", "&drive=0&start=1")
check("drive=0 puts it on the USB drive: %s" % r.get("root"), r.get("ok") and r["root"] == U)
check("it completes there", wait(lambda: items().get("GameOnePart", {}).get("status") == "complete", 40) and os.path.isdir(U + "/downloads/Game One"))

print("== explicit pause beats automatic start ON")
get("/api/set?autostart=1"); time.sleep(1)
r = add("Big8M.torrent", "&drive=internal&start=0")
check("reply: started=false although autostart is on", r.get("ok") and r.get("started") is False and r.get("autostart") is True)
check("it stays paused", wait(lambda: items().get("Big8M", {}).get("status") == "paused", 15))

print("== no drive and no start given (API clients): console memory + automatic-start rule")
r = add("Game One.torrent")
check("default destination is the console memory and started follows autostart: %s" % r, r.get("ok") and r["root"] == I and r.get("started") is True)
check("it completes", wait(lambda: items().get("Game One", {}).get("status") == "complete", 40))
check("log marks the paused one '[added paused]'", "[added paused]" in log())
print("RESULT: %d/%d passed" % (sum(res), len(res)))
