#include "tracker.h"
#include "net.h"
#include "bencode.h"
#include "log.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>

static int g_announcePort = 6881;
void tracker_set_port(int port) { if (port > 0 && port < 65536) g_announcePort = port; }

static std::string urlencode(const unsigned char* d, size_t n) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = d[i];
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') || c == '-' || c == '.' || c == '_' || c == '~') {
            out += (char)c;
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

bool tracker_announce_http(const std::string& url, const unsigned char infoHash[20],
                           const unsigned char peerId[20], uint64_t left,
                           AnnounceResult& res) {
    if (url.compare(0, 7, "http://") != 0) { res.error = "not http"; return false; }

    std::string rest = url.substr(7);
    std::string hostport = rest, path = "/";
    size_t slash = rest.find('/');
    if (slash != std::string::npos) {
        hostport = rest.substr(0, slash);
        path = rest.substr(slash);
    }
    std::string host = hostport;
    int port = 80;
    size_t colon = hostport.find(':');
    if (colon != std::string::npos) {
        host = hostport.substr(0, colon);
        port = atoi(hostport.c_str() + colon + 1);
    }

    char tail[112];
    snprintf(tail, sizeof(tail),
             "&port=%d&uploaded=0&downloaded=0&left=%llu&compact=1&numwant=50&event=started",
             g_announcePort, (unsigned long long)left);
    std::string q = path;
    q += (path.find('?') == std::string::npos) ? '?' : '&';
    q += "info_hash=" + urlencode(infoHash, 20);
    q += "&peer_id=" + urlencode(peerId, 20);
    q += tail;

    std::string req = "GET " + q + " HTTP/1.0\r\nHost: " + host +
                      "\r\nUser-Agent: ps4torrent/0.1\r\nConnection: close\r\n\r\n";

    int s = net_connect(host.c_str(), port, 8000);
    if (s < 0) { res.error = "connect failed"; return false; }

    size_t sent = 0;
    while (sent < req.size()) {
        int n = send(s, req.data() + sent, req.size() - sent, 0);
        if (n <= 0) { close(s); res.error = "send failed"; return false; }
        sent += n;
    }

    std::string resp;
    char buf[4096];
    for (;;) {
        int n = recv(s, buf, sizeof(buf), 0);
        if (n <= 0) break;
        resp.append(buf, n);
        if (resp.size() > 256 * 1024) break;
    }
    close(s);

    if (resp.empty()) { res.error = "empty response"; return false; }
    size_t hdrEnd = resp.find("\r\n\r\n");
    if (hdrEnd == std::string::npos) { res.error = "bad http response"; return false; }

    size_t eol = resp.find("\r\n");
    std::string status = resp.substr(0, eol);
    if (status.find(" 200") == std::string::npos) { res.error = "http: " + status; return false; }

    std::string body = resp.substr(hdrEnd + 4);
    BNode root;
    if (!bdecode(body, root) || root.type != BNode::DICT) { res.error = "bad tracker bencode"; return false; }

    const BNode* fr = root.find("failure reason");
    if (fr && fr->type == BNode::STR) { res.error = "tracker: " + fr->s; return false; }

    const BNode* iv = root.find("interval");
    if (iv && iv->type == BNode::INT) res.interval = (int)iv->i;

    const BNode* pe = root.find("peers");
    if (pe && pe->type == BNode::STR) {
        for (size_t i = 0; i + 6 <= pe->s.size(); i += 6) {
            Peer p;
            memcpy(p.ip, pe->s.data() + i, 4);
            p.port = ((unsigned char)pe->s[i + 4] << 8) | (unsigned char)pe->s[i + 5];
            res.peers.push_back(p);
        }
    } else if (pe && pe->type == BNode::LIST) {
        for (size_t i = 0; i < pe->l.size(); i++) {
            const BNode* ip = pe->l[i].find("ip");
            const BNode* pt = pe->l[i].find("port");
            if (!ip || !pt || ip->type != BNode::STR || pt->type != BNode::INT) continue;
            unsigned a, b, c, d;
            if (sscanf(ip->s.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) != 4) continue;
            Peer p;
            p.ip[0] = a; p.ip[1] = b; p.ip[2] = c; p.ip[3] = d;
            p.port = (uint16_t)pt->i;
            res.peers.push_back(p);
        }
    }
    return true;
}