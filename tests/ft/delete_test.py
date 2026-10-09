import urllib.request, json, time, os, subprocess, sys
B = "http://127.0.0.1:18787"
def get(p):
    return json.load(urllib.request.urlopen(B + p, timeout=5))
def items(): return {i["title"]: i for i in get("/status")["items"]}
def wait(cond, secs=40):
    t = time.time()
    while time.time() - t < secs:
        try:
            if cond(): return True
        except Exception: pass
        time.sleep(0.5)
    return False
res = []
def check(name, ok): res.append(ok); print(("PASS " if ok else "FAIL ") + name, flush=True)
R = "/tmp/ft/usb0/torrents"
def tree(): 
    out = []
    for dp, dn, fn in os.walk(R):
        if "/.state" in dp: continue
        for f in fn: out.append(os.path.relpath(os.path.join(dp, f), R))
    return sorted(out)

check("all test torrents completed", wait(lambda: all(items().get(t, {}).get("status") == "complete" for t in ("Game One", "Small")) and "Game One" in items()))
time.sleep(1)
it = items()
print("items:", {k: v["status"] for k, v in it.items()})
open(R + "/evil.txt", "w").write("sentinel: must survive")      # лежит там, куда ведёт путь ../../evil.txt из Evil
print("before:", tree())

# 1. GameOnePart (подмножество Game One): файлы с delete_files, но a.bin нужен «Game One» и должен остаться
gp = [t for t in it if t != "Game One" and "Game One" in t or t == "GameOnePart"]
gp_title = "GameOnePart"
check("subset torrent GameOnePart is known", gp_title in it)
if gp_title in it:
    get("/api/delete?files=1&hash=" + it[gp_title]["hash"]); time.sleep(3)
    check("subset removed from the list", gp_title not in items())
    check("shared file a.bin is kept (needed by Game One)", os.path.exists(R + "/downloads/Game One/a.bin"))
    check("its .torrent moved to removed/", os.path.exists(R + "/removed/GameOnePart.torrent") and not os.path.exists(R + "/complete/GameOnePart.torrent"))

# 2. Small: delete БЕЗ файлов -> файлы остаются
get("/api/delete?hash=" + it["Small"]["hash"]); time.sleep(2)
check("Small removed from the list", "Small" not in items())
check("Small's files stay on the drive (delete without files)", os.path.exists(R + "/downloads/Small/only.bin"))
check("Small.torrent moved to removed/", os.path.exists(R + "/removed/Small.torrent"))

# 3. вредоносный торрент: delete_files не должен задеть файл вне downloads
if "Evil" in it:
    get("/api/delete?files=1&hash=" + it["Evil"]["hash"]); time.sleep(3)
    check("Evil removed from the list", "Evil" not in items())
check("file outside downloads survives a malicious ../../ path", os.path.exists(R + "/evil.txt") and open(R + "/evil.txt").read().startswith("sentinel"))

# 4. Game One с файлами: всё стирается, папки тоже
get("/api/delete?files=1&hash=" + it["Game One"]["hash"]); time.sleep(4)
check("Game One removed from the list", "Game One" not in items())
check("Game One's files and empty folders are gone", not os.path.exists(R + "/downloads/Game One"))
check("Game One.torrent moved to removed/", os.path.exists(R + "/removed/Game One.torrent"))
check("resume files cleaned up", not [f for f in os.listdir(R + "/.state")] if os.path.isdir(R + "/.state") else True)

# 5. короткий хэш для delete запрещён
try:
    urllib.request.urlopen(B + "/api/delete?hash=" + "ab" * 4, timeout=5); ok = False
except urllib.error.HTTPError as e: ok = (e.code == 400)
check("delete with a short hash is refused (400)", ok)
print("after:", tree())
print("RESULT: %d/%d passed" % (sum(res), len(res)))
