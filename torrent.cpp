#include "torrent.h"
#include "bencode.h"
#include "sha1.h"
#include <stdio.h>

static bool read_file(const char* path, std::string& out) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    fclose(f);
    return true;
}

static void add_tracker(Torrent& t, const std::string& url) {
    if (url.empty()) return;
    for (size_t k = 0; k < t.trackers.size(); k++)
        if (t.trackers[k] == url) return;
    t.trackers.push_back(url);
}

bool load_torrent(const char* path, Torrent& t, std::string& err) {
    std::string data;
    if (!read_file(path, data)) { err = "cannot read file"; return false; }

    BNode root;
    if (!bdecode(data, root) || root.type != BNode::DICT) { err = "bad bencode"; return false; }

    const BNode* info = root.find("info");
    if (!info || info->type != BNode::DICT) { err = "no info dict"; return false; }

    // info hash = SHA-1 от исходных байт словаря info
    sha1((const unsigned char*)data.data() + info->begin, info->end - info->begin, t.infoHash);
    static const char* hex = "0123456789abcdef";
    t.infoHashHex.clear();
    for (int i = 0; i < 20; i++) {
        t.infoHashHex += hex[t.infoHash[i] >> 4];
        t.infoHashHex += hex[t.infoHash[i] & 15];
    }

    const BNode* name = info->find("name");
    const BNode* plen = info->find("piece length");
    const BNode* pcs  = info->find("pieces");
    if (!name || name->type != BNode::STR) { err = "no name"; return false; }
    if (!plen || plen->type != BNode::INT || plen->i <= 0 || plen->i > 64 * 1024 * 1024) { err = "bad piece length"; return false; }
    if (!pcs || pcs->type != BNode::STR || pcs->s.size() % 20 != 0 || pcs->s.empty()) { err = "bad pieces"; return false; }

    t.name = name->s;
    t.pieceLength = (uint32_t)plen->i;
    t.pieces = pcs->s;
    t.numPieces = (uint32_t)(pcs->s.size() / 20);

    const BNode* length = info->find("length");
    const BNode* files  = info->find("files");
    t.files.clear();
    t.totalSize = 0;

    if (length && length->type == BNode::INT && length->i >= 0) {
        TorrentFile f;
        f.path = t.name;
        f.length = (uint64_t)length->i;
        t.files.push_back(f);
        t.totalSize = f.length;
    } else if (files && files->type == BNode::LIST) {
        for (size_t k = 0; k < files->l.size(); k++) {
            const BNode& fe = files->l[k];
            const BNode* fl = fe.find("length");
            const BNode* fp = fe.find("path");
            if (!fl || fl->type != BNode::INT || fl->i < 0 || !fp || fp->type != BNode::LIST) {
                err = "bad file entry";
                return false;
            }
            TorrentFile f;
            f.path = t.name;
            for (size_t j = 0; j < fp->l.size(); j++) {
                if (fp->l[j].type != BNode::STR) { err = "bad path"; return false; }
                f.path += "/" + fp->l[j].s;
            }
            f.length = (uint64_t)fl->i;
            t.files.push_back(f);
            t.totalSize += f.length;
        }
    } else {
        err = "no length or files";
        return false;
    }

    // проверка: число кусков должно соответствовать размеру
    uint64_t expect = (t.totalSize + t.pieceLength - 1) / t.pieceLength;
    if (expect != t.numPieces) { err = "piece count mismatch"; return false; }

    // трекеры
    const BNode* an = root.find("announce");
    if (an && an->type == BNode::STR) add_tracker(t, an->s);
    const BNode* al = root.find("announce-list");
    if (al && al->type == BNode::LIST) {
        for (size_t a = 0; a < al->l.size(); a++) {
            if (al->l[a].type != BNode::LIST) continue;
            for (size_t b = 0; b < al->l[a].l.size(); b++)
                if (al->l[a].l[b].type == BNode::STR) add_tracker(t, al->l[a].l[b].s);
        }
    }
    return true;
}