#include "magnet.h"
#include <ctype.h>
#include <strings.h>
#include <stdlib.h>
#include <string.h>

const char* const MAGNET_DEFAULT_TRACKER = "http://bt.t-ru.org/ann?magnet";

static std::string url_decode(const std::string& s)
{
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size() && isxdigit((unsigned char)s[i + 1]) && isxdigit((unsigned char)s[i + 2])) {
            char b[3] = { s[i + 1], s[i + 2], 0 };
            o += (char)strtol(b, NULL, 16);
            i += 2;
        } else if (s[i] == '+') o += ' ';
        else o += s[i];
    }
    return o;
}

static int hexv(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool decode_hash(const std::string& v, unsigned char out[20])
{
    if (v.size() == 40) {
        for (int i = 0; i < 20; i++) {
            int a = hexv(v[(size_t)i * 2]), b = hexv(v[(size_t)i * 2 + 1]);
            if (a < 0 || b < 0) return false;
            out[i] = (unsigned char)(a * 16 + b);
        }
        return true;
    }
    if (v.size() == 32) {   // base32 (RFC 4648)
        unsigned long long acc = 0;
        int bits = 0, n = 0;
        for (size_t i = 0; i < v.size(); i++) {
            char c = (char)toupper((unsigned char)v[i]);
            int d;
            if (c >= 'A' && c <= 'Z') d = c - 'A';
            else if (c >= '2' && c <= '7') d = c - '2' + 26;
            else return false;
            acc = (acc << 5) | (unsigned)d;
            bits += 5;
            if (bits >= 8) {
                bits -= 8;
                if (n >= 20) return false;
                out[n++] = (unsigned char)((acc >> bits) & 0xFF);
            }
        }
        return n == 20;
    }
    return false;
}

bool magnet_parse(const std::string& uri, Magnet& m, std::string& err)
{
    m = Magnet();
    size_t a = 0;
    while (a < uri.size() && isspace((unsigned char)uri[a])) a++;
    size_t b = uri.size();
    while (b > a && isspace((unsigned char)uri[b - 1])) b--;
    std::string u = uri.substr(a, b - a);
    if (u.size() > 3 && strncasecmp(u.c_str(), "xt=", 3) == 0) u = "magnet:?" + u;   // ссылка без начала: xt=urn:btih:...&tr=...
    if (u.size() < 8 || strncasecmp(u.c_str(), "magnet:?", 8) != 0) { err = "not a magnet link"; return false; }
    bool have = false;
    size_t i = 8;
    while (i <= u.size()) {
        size_t e = u.find('&', i);
        if (e == std::string::npos) e = u.size();
        std::string kv = u.substr(i, e - i);
        size_t eq = kv.find('=');
        if (eq != std::string::npos) {
            std::string k = kv.substr(0, eq), v = url_decode(kv.substr(eq + 1));
            if (k == "xt" && !have && v.size() > 9 && strncasecmp(v.c_str(), "urn:btih:", 9) == 0) {
                have = decode_hash(v.substr(9), m.hash);
                if (!have) { err = "bad info hash"; return false; }
            } else if (k == "tr" && v.compare(0, 7, "http://") == 0 && m.trackers.size() < 8 && v.size() < 512) {
                bool dup = false;
                for (size_t t = 0; t < m.trackers.size(); t++) if (m.trackers[t] == v) dup = true;
                if (!dup) m.trackers.push_back(v);
            } else if (k == "dn" && m.name.empty() && v.size() < 512) m.name = v;
        }
        i = e + 1;
    }
    if (!have) { err = "no btih info hash"; return false; }
    static const char* hx = "0123456789abcdef";
    m.hex.clear();
    for (int k = 0; k < 20; k++) { m.hex += hx[m.hash[k] >> 4]; m.hex += hx[m.hash[k] & 15]; }
    if (m.trackers.empty()) m.trackers.push_back(MAGNET_DEFAULT_TRACKER);
    return true;
}

static void bstr(std::string& o, const std::string& s) { o += std::to_string(s.size()); o += ':'; o += s; }

std::string magnet_build_torrent(const std::string& infoRaw, const std::vector<std::string>& trackers)
{
    std::string o = "d";
    if (!trackers.empty()) {
        bstr(o, "announce"); bstr(o, trackers[0]);
        bstr(o, "announce-list");
        o += "l";
        for (size_t i = 0; i < trackers.size(); i++) { o += "l"; bstr(o, trackers[i]); o += "e"; }
        o += "e";
    }
    bstr(o, "info");
    o += infoRaw;
    o += "e";
    return o;
}
