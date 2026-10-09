import urllib.request, urllib.error, json, time, os, subprocess
B = "http://127.0.0.1:18787"; FT = "/tmp/ft"; A = "/tmp/daemon/assets/"
res = []
def check(name, ok): res.append(ok); print(("PASS " if ok else "FAIL ") + name, flush=True)
def fetch(p, token=None):
    u = B + p + (("&" if "?" in p else "?") + "token=" + token if token else "")
    try:
        with urllib.request.urlopen(u, timeout=8) as r: return r.status, r.headers.get("Content-Type"), r.read()
    except urllib.error.HTTPError as e: return e.code, e.headers.get("Content-Type"), e.read()
def jget(p, token="sekret"): c, t, b = fetch(p, token); return c, (json.loads(b) if b[:1] == b"{" else {})
def cfg(): return jget("/api/config")[1]
def wait(cond, secs):
    t0 = time.time()
    while time.time() - t0 < secs:
        try:
            if cond(): return True
        except Exception: pass
        time.sleep(0.3)
    return False
def start():
    subprocess.Popen("setsid ./daemon_icon >> icon.out 2>&1 < /dev/null &", shell=True, cwd=FT); time.sleep(2.5)
def quit_():
    try: urllib.request.urlopen(B + "/quit?token=sekret", timeout=3).read()
    except Exception: pass
    time.sleep(3.5)

print("== the images are served from inside the program (no token needed, a token IS configured)")
for path, name, ctype in (("/favicon.ico", "favicon.ico", "image/x-icon"), ("/logo.png", "title.png", "image/png"), ("/notify.png", "notify64.png", "image/png")):
    c, t, b = fetch(path)
    check("%s: 200, %s, identical to the embedded file (%d bytes)" % (path, t, len(b)), c == 200 and t == ctype and b == open(A + name, "rb").read())
check("API without the token is still refused (401)", fetch("/api/config")[0] == 401)
page = fetch("/")[2].decode()
check("the page uses the logo image and the site icon", 'src="/logo.png"' in page and 'rel="icon" href="/favicon.ico"' in page and "<h1>ps4torrent</h1>" not in page)

print("== the notification icon is placed on disk by the program itself")
check("notify.png appears in the data folder and equals the embedded image", wait(lambda: os.path.exists(FT + "/log/notify.png") and open(FT + "/log/notify.png", "rb").read() == open(A + "notify64.png", "rb").read(), 8))
c = cfg()
check("config: the verified web-server address is the default; four test addresses are offered: %s" % c.get("notify_variants"), c.get("notify_icon") == "http://127.0.0.1:18787/notify.png" and c.get("http_port") == 18787 and len(c.get("notify_variants", [])) == 4)
check("the file address points to the data folder", any(v.endswith("/notify.png") and v.startswith("file://" + FT + "/log") for v in c["notify_variants"]))

print("== test notifications: only our own addresses are allowed")
v = c["notify_variants"]
for i, u in enumerate(v):
    code, j = jget("/api/notify_test?uri=" + urllib.request.quote(u, safe="") + "&text=hello+%d" % i)
    check("variant %d accepted: %s" % (i + 1, u), code == 200 and j.get("ok"))
bad = ["http://evil.example/x.png", "https://127.0.0.1:8787/notify.png", "file:///etc/passwd", "/etc/passwd", "cxml://psnotification/../x", "http://127.0.0.1:8787/a%2e%2e", "file://" + FT + "/log/../x.png"]
for u in bad:
    check("refused (400): %s" % u, jget("/api/notify_test?uri=" + urllib.request.quote(u, safe=""))[0] == 400)

print("== choosing the icon for all notifications")
check("invalid choice refused", jget("/api/set?notify_icon=" + urllib.request.quote("http://evil.example/x.png", safe=""))[0] == 400)
code, j = jget("/api/set?notify_icon=" + urllib.request.quote(v[1], safe=""))
check("valid choice accepted", code == 200 and j.get("ok"))
check("config shows it", wait(lambda: cfg().get("notify_icon") == v[1], 5))
check("config.txt stores it", "notify_icon=" + v[1] in open(FT + "/log/config.txt").read())
jget("/api/notify_test?text=uses+the+chosen+icon")
quit_()
check("the notification without an explicit address used the chosen icon", "(icon %s) uses the chosen icon" % v[1] in open(FT + "/icon.out").read())

print("== it survives a restart; and resetting returns the default icon")
start()
check("after the restart the choice is still active", cfg().get("notify_icon") == v[1])
jget("/api/set?notify_icon=none")
check("'none' selects the standard icon and is stored explicitly", wait(lambda: cfg().get("notify_icon") == "" and "notify_icon=none" in open(FT + "/log/config.txt").read(), 5))
quit_(); start()
check("after another restart the standard icon is still selected (the default does not come back)", cfg().get("notify_icon") == "")
quit_()
check("the 'started' notification of the second run already carried the chosen icon", ("(icon %s) ps4torrent started" % v[1]) in open(FT + "/icon.out").read())
print("RESULT: %d/%d passed" % (sum(res), len(res)))
