#include "i18n.h"
#include "app.h"
#include "httpc.h"
#include "json.h"
#include "log.h"

#include <map>
#include <vector>
#include <stdio.h>

// Язык системы PS4: sceSystemServiceParamGetInt(SCE_SYSTEM_SERVICE_PARAM_ID_LANG, &id).
extern "C" int sceSystemServiceParamGetInt(int paramId, int* value);
static const int PARAM_ID_LANG = 1;

static std::string g_want = "en";     // какой файл просим: "ru" или "en"
static bool        g_loaded = false;  // true: либо загрузили, либо решили остаться на английском
static uint64_t    g_lastTryUs = 0;
static std::map<std::string, std::string> g_strings;

static void merge_json(const JsonValue& j)
{
    for (size_t i = 0; i < j.obj.size(); i++)
        if (j.obj[i].second.type == JsonValue::STR) g_strings[j.obj[i].first] = j.obj[i].second.str;
}

static bool load_bundled(const std::string& path)
{
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    std::string body;
    char buf[2048];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) body.append(buf, n);
    fclose(f);
    JsonValue j;
    if (!json_parse(body, j) || j.type != JsonValue::OBJ) return false;
    merge_json(j);
    return true;
}

// ID языка PS4 (SCE_SYSTEM_PARAM_LANG) -> коды файлов lang_<код>.json по порядку предпочтения.
// Сначала точный вариант (pt-br), затем общий (pt). Положите файл assets/misc/lang_<код>.json
// и язык подхватится сам, менять код не нужно.
static std::vector<std::string> lang_candidates(int id)
{
    std::vector<std::string> c;
    switch (id) {
    case 0:  c.push_back("ja"); break;
    case 1:  c.push_back("en"); break;
    case 2:  c.push_back("fr"); break;
    case 3:  c.push_back("es"); break;
    case 4:  c.push_back("de"); break;
    case 5:  c.push_back("it"); break;
    case 6:  c.push_back("nl"); break;
    case 7:  c.push_back("pt"); break;
    case 8:  c.push_back("ru"); break;
    case 9:  c.push_back("ko"); break;
    case 10: c.push_back("zh-hant"); c.push_back("zh"); break;
    case 11: c.push_back("zh-hans"); c.push_back("zh"); break;
    case 12: c.push_back("fi"); break;
    case 13: c.push_back("sv"); break;
    case 14: c.push_back("da"); break;
    case 15: c.push_back("no"); break;
    case 16: c.push_back("pl"); break;
    case 17: c.push_back("pt-br"); c.push_back("pt"); break;
    case 18: c.push_back("en-gb"); c.push_back("en"); break;
    case 19: c.push_back("tr"); break;
    case 20: c.push_back("es-la"); c.push_back("es"); break;
    case 21: c.push_back("ar"); break;
    case 22: c.push_back("fr-ca"); c.push_back("fr"); break;
    case 23: c.push_back("cs"); break;
    case 24: c.push_back("hu"); break;
    case 25: c.push_back("el"); break;
    case 26: c.push_back("ro"); break;
    case 27: c.push_back("th"); break;
    case 28: c.push_back("vi"); break;
    case 29: c.push_back("id"); break;
    default: c.push_back("en"); break;
    }
    return c;
}

void i18n_init(const char* assetDir)
{
    int id = -1;
    int r = sceSystemServiceParamGetInt(PARAM_ID_LANG, &id);
    if (r != 0) id = -1;
    std::vector<std::string> cands = lang_candidates(id);
    logf_("i18n: system language id %d (rc 0x%08X), candidates: %s%s", id, (unsigned)r,
          cands[0].c_str(), cands.size() > 1 ? (" , " + cands[1]).c_str() : "");

    // Берём первый вариант, для которого есть встроенный файл. Если файла нет ни для одного,
    // остаётся английский по умолчанию, а на демон спросим общий код (последний в списке).
    g_want = cands.back();
    bool ok = false;
    for (size_t i = 0; i < cands.size() && !ok; i++) {
        if (load_bundled(std::string(assetDir) + "/lang_" + cands[i] + ".json")) { g_want = cands[i]; ok = true; }
    }
    logf_("i18n: using '%s', bundled file %s (%d strings)", g_want.c_str(), ok ? "loaded" : "not found, English defaults", (int)g_strings.size());
}

void i18n_tick(bool serverOk, uint64_t nowUs)
{
    if (g_loaded || !serverOk) return;
    if (g_lastTryUs != 0 && nowUs - g_lastTryUs < 5000000ULL) return;
    g_lastTryUs = nowUs;

    HttpResult r = http_get(g_host, g_port, "/lang/" + g_want + ".json", 3000);
    if (!r.connected) return;   // демон пока недоступен: попробуем позже

    JsonValue j;
    if (r.status == 200 && json_parse(r.body, j) && j.type == JsonValue::OBJ) {
        merge_json(j);   // строки демона перекрывают встроенные
        logf_("i18n: merged /lang/%s.json from daemon (%d strings total)", g_want.c_str(), (int)g_strings.size());
    } else {
        logf_("i18n: /lang/%s.json unavailable (status %d), keeping bundled strings", g_want.c_str(), r.status);
    }
    g_loaded = true;   // при любой ошибке остаётся встроенный английский
}

std::string tr(const char* key, const char* def)
{
    std::map<std::string, std::string>::const_iterator it = g_strings.find(key);
    if (it != g_strings.end() && !it->second.empty()) return it->second;
    return def;
}
