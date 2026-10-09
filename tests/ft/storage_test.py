import urllib.request, urllib.error, json, time, os, subprocess, shutil
B = "http://127.0.0.1:18787"; FT = "/tmp/ft"; U = FT + "/usb0/torrents"; I = FT + "/internal/torrents"
res = []
def check(name, ok): res.append(ok); print(("PASS " if ok else "FAIL ") + name, flush=True)
def status(): return json.load(urllib.request.urlopen(B + "/status", timeout=8))
def cfg(): return json.load(urllib.request.urlopen(B + "/api/config", timeout=8))
def get(p): return json.load(urllib.request.urlopen(B + p, timeout=15))
def post_add(name, data, extra=""):
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
def tor(n): return open(FT + "/torrents_src/" + n, "rb").read()
def sh(c): subprocess.run(c, shell=True, cwd=FT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
def same(rel, root): return open(FT + "/src/" + rel, "rb").read() == open(root + "/downloads/" + rel, "rb").read()
def start():
    subprocess.Popen("setsid ./daemon_store > store.out 2>&1 < /dev/null &", shell=True, cwd=FT); time.sleep(2.5)

print("== 1. defaults")
d = status(); c = cfg()
check("save_to defaults to the console memory: %s" % c.get("save_to"), c.get("save_to") == "internal")
check("both places are listed from the start (the console-memory folder is created because it is the default): %s" % [x["kind"] for x in d["drives"]], [x["kind"] for x in d["drives"]] == ["usb", "internal"])
check("page footer carries the credit line", b"SergioPoverony" in urllib.request.urlopen(B + "/", timeout=5).read())
r = post_add("small.torrent", tor("Small.torrent"), "&drive=0")
check("upload goes to the USB drive: %s" % r.get("root"), r.get("ok") and r["root"] == U and os.path.exists(U + "/small.torrent"))
check("USB task completes", wait(lambda: items().get("small", {}).get("status") == "complete", 20))

print("== 2. choose console memory")
try: urllib.request.urlopen(B + "/api/set?save_to=floppy", timeout=5); ok = False
except urllib.error.HTTPError as e: ok = e.code == 400
check("invalid save_to rejected (400)", ok)
get("/api/set?save_to=usb"); time.sleep(1); get("/api/set?save_to=internal")
check("setting applied", wait(lambda: cfg()["save_to"] == "internal", 5))
check("folder created in the console memory with its subfolders", wait(lambda: all(os.path.isdir(I + "/" + x) for x in ("downloads", "complete", ".state")), 15))
check("config.txt stores it", "save_to=internal" in open(FT + "/log/config.txt").read())
check("drive list now has the console memory with free space", wait(lambda: any(x["kind"] == "internal" and x["free"] > 0 for x in status()["drives"]), 15))
r = post_add("Big8M.torrent", tor("Big8M.torrent"))
check("upload now goes to the console memory: %s" % r.get("root"), r.get("ok") and r["root"] == I)
check("download completes inside the console memory", wait(lambda: items().get("Big8M", {}).get("status") == "complete", 60))
check("files are in <console memory>/downloads/Big8M and identical", same("Big8M/big.bin", I) and same("Big8M/extra/more.bin", I))
check(".torrent moved to complete/ there", os.path.exists(I + "/complete/Big8M.torrent"))
r = post_add("Many.torrent", tor("Many.torrent"), "&drive=0")
check("explicit drive=0 still means USB: %s" % r.get("root"), r.get("ok") and r["root"] == U)
shutil.copy(FT + "/torrents_src/Game One.torrent", I + "/Game One.torrent")
check("a .torrent dropped into the console-memory folder by hand is picked up and completes", wait(lambda: items().get("Game One", {}).get("status") == "complete", 40))
check("USB and console-memory tasks both finished", wait(lambda: items().get("Many", {}).get("status") == "complete", 40))

print("== 3. free-space warnings")
open(FT + "/free_mock", "w").write(str(3 * 2**30))          # 3 ГБ свободно везде
check("console memory is flagged low (3 GB < 5 GB reserve), USB is not (3 GB >= 2 GB)", wait(lambda: {x["kind"]: x["low"] for x in status()["drives"]} == {"usb": False, "internal": True}, 14))
shutil.copy(FT + "/torrents_src/Chk.torrent", I + "/Chk.torrent")            # новая раздача (400 МБ) во внутренней памяти
check("a task on the console memory carries low_space=true while it downloads", wait(lambda: any(i["title"] == "Chk" and i["low_space"] is True for i in status()["items"]), 25))
check("log has the LOW FREE SPACE warning for it", wait(lambda: "WARNING: low free space for Chk" in log(), 5))
wait(lambda: items().get("Chk", {}).get("status") == "complete", 60)

print("== 4. the console memory is almost full: downloads there must NOT start (system protection)")
open(FT + "/free_mock", "w").write(str(500 * 2**20))        # осталось 500 МБ
shutil.copy(FT + "/torrents_src/Many.torrent", I + "/Many2.torrent")
check("log: not starting ... console memory", wait(lambda: "not starting" in log() and "console memory" in log(), 25))
m2 = [i for i in status()["items"] if i["title"] == "Many2"]
check("that task is not downloading (status %s)" % (m2[0]["status"] if m2 else "?"), bool(m2) and m2[0]["status"] not in ("downloading", "checking", "complete"))
shutil.copy(FT + "/torrents_src/GameOnePart.torrent", U + "/GameOnePart.torrent")
check("but a USB task still starts (with a warning only) even though free space is low", wait(lambda: items().get("GameOnePart", {}).get("status") == "complete", 40))
check("log has the LOW FREE SPACE warning for the USB task", "WARNING: low free space for GameOnePart" in log())
os.remove(FT + "/free_mock")
check("when space is available again the blocked task starts by itself (retry within ~60 s)", wait(lambda: items().get("Many2", {}).get("status") == "complete", 100))

print("== 5. delete with files on the console memory")
h = items()["Big8M"]["hash"]
get("/api/delete?files=1&hash=" + h)
check("files removed from the console memory and .torrent kept in removed/", wait(lambda: not os.path.exists(I + "/downloads/Big8M") and os.path.exists(I + "/removed/Big8M.torrent"), 20))
check("files of the USB tasks untouched", os.path.exists(U + "/downloads/Many/many.bin"))
print("RESULT: %d/%d passed" % (sum(res), len(res)))
