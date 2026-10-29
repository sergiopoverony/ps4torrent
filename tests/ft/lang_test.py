# Файлы языков, вшитые в программу: /lang/index.json и /lang/<код>.json отдаются без пароля, выключение через /quit.
import urllib.request, urllib.error, json, time, subprocess, sys
B = "http://127.0.0.1:18787"; FT = "/tmp/ft"; BIN = sys.argv[1]
res = []
def check(name, ok): ok = bool(ok); res.append(ok); print(("PASS " if ok else "FAIL ") + name, flush=True)
def sh(c): subprocess.run(c, shell=True, cwd=FT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
def get(p): 
    try:
        r = urllib.request.urlopen(B + p, timeout=10); return r.status, r.headers.get("Content-Type", ""), r.read()
    except urllib.error.HTTPError as e: return e.code, "", e.read()
sh("pkill -9 -x %s; rm -rf usb0 usb1 internal log; mkdir -p usb0/torrents log; echo web_token=sekret > log/config.txt" % BIN)
subprocess.Popen("setsid ./%s > lang.out 2>&1 < /dev/null &" % BIN, shell=True, cwd=FT); time.sleep(2.5)
s, ct, b = get("/lang/index.json")
idx = json.loads(b)
names = {x["code"]: x["name"] for x in idx}
check("index.json is served WITHOUT the token and lists all 20 languages with names (en English, ru Русский, de Deutsch...): %d" % len(names),
      s == 200 and len(names) == 20 and names.get("en") == "English" and names.get("ru") == "Русский" and names.get("de") == "Deutsch" and names.get("el") == "Ελληνικά")
s, ct, b = get("/lang/ru.json")
d = json.loads(b.decode("utf-8"))
check("ru.json is valid UTF-8 JSON with the Russian texts (%s)" % ct, s == 200 and "json" in ct and d["btn_quit"] == "выключить" and d["_name"] == "Русский")
s, ct, b = get("/lang/en.json")
check("en.json is served", s == 200 and json.loads(b)["btn_quit"] == "shut down")
check("an unknown language is 404", get("/lang/xx.json")[0] == 404)
check("path tricks are 404, not files", get("/lang/../main.cpp")[0] == 404 and get("/lang/..%2Fconfig.txt")[0] == 404)
check("the API still requires the token", get("/status")[0] == 401 and get("/quit")[0] == 401)
s, ct, b = get("/")
check("the page loads without a token and has the shut-down button", s == 200 and b'id="quitb"' in b)
s, ct, b = get("/quit?token=sekret")
time.sleep(3)
try: urllib.request.urlopen(B + "/status?token=sekret", timeout=3); alive = True
except Exception: alive = False
check("/quit with the token shuts the program down", s == 200 and not alive)
out = open(FT + "/lang.out").read()
check("no sanitizer report", "ERROR: AddressSanitizer" not in out and "runtime error" not in out)
sh("pkill -9 -x %s" % BIN)
print("RESULT: %d/%d passed" % (sum(res), len(res)))
