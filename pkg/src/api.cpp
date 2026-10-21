#include "api.h"
#include "app.h"
#include "httpc.h"
#include "json.h"
#include "log.h"

static const int TIMEOUT_MS = 3000;
static std::string g_lastErr;

static std::string with_token(const std::string& path)
{
    if (g_token.empty()) return path;
    return path + (path.find('?') == std::string::npos ? "?token=" : "&token=") + g_token;
}

bool api_refresh_status()
{
    HttpResult r = http_get(g_host, g_port, with_token("/status"), TIMEOUT_MS);
    if (!r.connected) { g_status.ok = false; g_lastErr = "no connection to ps4torrentd"; return false; }
    if (r.status != 200) { g_status.ok = false; g_lastErr = "server returned status " + std::to_string(r.status); return false; }

    JsonValue j;
    if (!json_parse(r.body, j) || j.type != JsonValue::OBJ) {
        g_status.ok = false;
        g_lastErr = "bad JSON from server";
        return false;
    }

    ServerStatus st;
    st.ok = true;
    const JsonValue& items = j["items"];
    for (size_t i = 0; i < items.arr.size(); i++) {
        const JsonValue& it = items.arr[i];
        Item x;
        x.hash = it["hash"].asStr();
        x.status = it["status"].asStr("queued");
        x.size = (uint64_t)it["size"].asInt(0);
        x.done = (uint64_t)it["done"].asInt(0);
        x.pct = (int)it["pct"].asInt(0);
        x.speedKB = (int)it["speed_kb"].asInt(0);
        x.etaSec = it["eta_s"].asInt(-1);
        x.peers = (int)it["peers"].asInt(0);
        x.paused = it["paused"].asBool(false);
        x.lowSpace = it["low_space"].asBool(false);
        x.root = it["root"].asStr();
        x.title = it["title"].asStr();
        if (x.hash.empty()) continue;   // битая запись: пропускаем, а не падаем
        st.items.push_back(x);
    }

    const JsonValue& drives = j["drives"];
    for (size_t i = 0; i < drives.arr.size(); i++) {
        const JsonValue& d = drives.arr[i];
        Drive dr;
        dr.root = d["root"].asStr();
        dr.freeBytes = (uint64_t)d["free"].asInt(0);
        dr.totalBytes = (uint64_t)d["total"].asInt(0);
        dr.isInternal = (d["kind"].asStr() == "internal");
        dr.low = d["low"].asBool(false);
        st.drives.push_back(dr);
    }

    st.totalSpeedKB = (int)j["speed_kb"].asInt(0);
    st.maxParallel = (int)j["max_parallel"].asInt(0);
    st.listenPort = (int)j["listen_port"].asInt(0);
    st.incoming = (unsigned)j["incoming"].asInt(0);
    st.deleting = (int)j["deleting"].asInt(0);
    st.netIp = j["net_ip"].asStr();
    st.version = j["version"].asStr();

    g_status = st;
    g_lastErr.clear();
    return true;
}

// Общий код для команд-"одна строка результата". Разбирает {"ok":true} / {"ok":false,"error":"..."}.
static bool simple_cmd(const std::string& path)
{
    HttpResult r = http_get(g_host, g_port, with_token(path), TIMEOUT_MS);
    if (!r.connected) { g_lastErr = "no connection to ps4torrentd"; return false; }

    JsonValue j;
    bool parsed = json_parse(r.body, j) && j.type == JsonValue::OBJ;
    bool ok = parsed && j["ok"].asBool(false);

    if (!ok) {
        g_lastErr = parsed ? j["error"].asStr("request failed") : ("server returned status " + std::to_string(r.status));
        logf_("api: %s -> %s", path.c_str(), g_lastErr.c_str());
    }
    return ok;
}

bool api_pause(const std::string& hash)  { return simple_cmd("/api/pause?hash=" + hash); }
bool api_resume(const std::string& hash) { return simple_cmd("/api/resume?hash=" + hash); }
bool api_pause_all()  { return simple_cmd("/api/pause_all"); }
bool api_resume_all() { return simple_cmd("/api/resume_all"); }

bool api_delete(const std::string& hash, bool withFiles)
{
    std::string path = "/api/delete?hash=" + hash;
    if (withFiles) path += "&files=1";
    return simple_cmd(path);
}

bool api_quit()
{
    HttpResult r = http_get(g_host, g_port, with_token("/quit"), TIMEOUT_MS);
    if (!r.connected) { g_lastErr = "no connection to ps4torrentd"; return false; }
    if (r.status != 200) { g_lastErr = "server returned status " + std::to_string(r.status); return false; }
    logf_("api: /quit -> %s", r.body.c_str());   // ожидаем "bye"
    return true;
}

std::string api_last_error() { return g_lastErr; }
