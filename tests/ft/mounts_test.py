import urllib.request, urllib.error, json, time, os, shutil, subprocess
B = "http://127.0.0.1:18787"; FT = "/tmp/ft"
res = []
def check(name, ok): res.append(ok); print(("PASS " if ok else "FAIL ") + name, flush=True)
def status(): return json.load(urllib.request.urlopen(B + "/status", timeout=8))
def get(p): return json.load(urllib.request.urlopen(B + p, timeout=10))
def add(name, extra):
    data = open(FT + "/torrents_src/" + name, "rb").read()
    r = urllib.request.Request(B + "/api/add?name=" + urllib.request.quote(name) + extra, data=data, method="POST")
    try: return 200, json.load(urllib.request.urlopen(r, timeout=15))
    except urllib.error.HTTPError as e: return e.code, json.loads(e.read())
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
def mounts(): return {m["n"]: m for m in status()["mounts"]}

print("== which drives does the program know about")
check("config answers", "version" in get("/api/config"))
m = mounts()
check("mounted drives are listed (0, 1, 4 read-only, 5, ext0 as 8) and NOT the unmounted 2: %s" % sorted(m), sorted(m) == [0, 1, 4, 5, 8])
check("the read-only drive is flagged: writable=%s" % m[4]["writable"], m[4]["writable"] is False and m[0]["writable"] is True)
check("usb0 has the torrents folder, usb1 does not: %s/%s" % (m[0]["has_torrents"], m[1]["has_torrents"]), m[0]["has_torrents"] is True and m[1]["has_torrents"] is False)
check("free space is reported for a drive without the folder", m[1]["free"] > 0 and m[1]["total"] >= m[1]["free"])
check("the drive list (places with the folder) still contains only usb0 + console memory", sorted(x["root"] for x in status()["drives"] if x["kind"] == "usb") == [FT + "/usb0/torrents"])

print("== choosing a drive that is not usable")
c, j = add("Small.torrent", "&drive=2&start=1"); check("an unmounted drive is refused (409): %s" % j.get("error"), c == 409 and "not mounted" in j.get("error", ""))
c, j = add("Small.torrent", "&drive=3&start=1"); check("a drive that does not exist is refused (409)", c == 409)
c, j = add("Small.torrent", "&drive=10&start=1"); check("an invalid number is refused (400)", c == 400)
c, j = add("Small.torrent", "&drive=abc&start=1"); check("garbage is refused (400)", c == 400)
c, j = add("Small.torrent", "&drive=4&start=1"); check("a read-only drive: clear error, nothing created (500): %s" % j.get("error"), c == 500 and "cannot create the 'torrents' folder" in j.get("error", "") and "read-only" in j.get("error", "") and not os.path.exists(FT + "/usb4/torrents"))

print("== adding to usb1 which has NO torrents folder: the folder is created and everything works")
check("before: no folder", not os.path.exists(FT + "/usb1/torrents"))
c, j = add("Small.torrent", "&drive=1&start=1")
check("reply ok, placed under usb1: %s" % j.get("root"), c == 200 and j.get("ok") and j["root"] == FT + "/usb1/torrents")
check("the folder was created and the .torrent is in it", os.path.exists(FT + "/usb1/torrents/Small.torrent") or os.path.exists(FT + "/usb1/torrents/complete/Small.torrent"))
check("log says it created the folder", "created the 'torrents' folder on %s" % (FT + "/usb1") in log())
check("the working subfolders appear (downloads, complete, .state)", wait(lambda: all(os.path.isdir(FT + "/usb1/torrents/" + x) for x in ("downloads", "complete", ".state")), 15))
check("the task downloads onto usb1 and completes", wait(lambda: items().get("Small", {}).get("status") == "complete", 30) and os.path.exists(FT + "/usb1/torrents/downloads/Small/only.bin"))
check("usb1 now counts as a place with the folder", wait(lambda: mounts()[1]["has_torrents"] is True and any(x["root"] == FT + "/usb1/torrents" for x in status()["drives"]), 15))

print("== no drive given and 'save to USB' chosen: the best mounted drive is used, folder created if none has it")
get("/api/set?save_to=usb")
shutil.move(FT + "/usb0", FT + "/usb0_gone"); shutil.move(FT + "/usb1", FT + "/usb1_gone")
time.sleep(1)
c, j = add("Many.torrent", "&start=1")
check("with only empty mounts left it skips the read-only usb4, picks usb5 and creates the folder: %s" % j.get("root"), c == 200 and j.get("ok") and j["root"] == FT + "/usb5/torrents" and os.path.isdir(j["root"]) and not os.path.exists(FT + "/usb4/torrents"))
print("== extended disk ext0 (slot 8)")
check("ext0 is listed as kind=ext: %s" % m[8].get("kind"), m[8].get("kind") == "ext" and m[0].get("kind") == "usb" and m[8]["mount"] == FT + "/ext0")
c, j = add("Small.torrent", "&drive=8&start=1")
check("adding to ext0 creates its torrents folder: %s" % j.get("root"), c == 200 and j.get("ok") and j["root"] == FT + "/ext0/torrents" and os.path.isdir(j["root"]))
check("the task downloads onto ext0 and completes", wait(lambda: items().get("Small", {}).get("status") == "complete" and os.path.exists(FT + "/ext0/torrents/downloads/Small/only.bin"), 30))
check("ext0 appears in the drive list with kind=ext", wait(lambda: any(x["root"] == FT + "/ext0/torrents" and x["kind"] == "ext" for x in status()["drives"]), 15))
c, j = add("Small.torrent", "&drive=9&start=1"); check("ext1 (not mounted) is refused (409)", c == 409)
print("RESULT: %d/%d passed" % (sum(res), len(res)))
