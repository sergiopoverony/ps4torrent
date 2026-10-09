#include "bencode.h"
#include <utility>

const BNode* BNode::find(const char* key) const {
    if (type != DICT) return nullptr;
    for (size_t k = 0; k < keys.size(); k++)
        if (keys[k] == key) return &vals[k];
    return nullptr;
}

static bool parse(const std::string& s, size_t& pos, BNode& n, int depth) {
    if (depth > 32 || pos >= s.size()) return false;
    n.begin = pos;
    char c = s[pos];

    if (c == 'i') {
        pos++;
        bool neg = false;
        if (pos < s.size() && s[pos] == '-') { neg = true; pos++; }
        int64_t v = 0;
        int digits = 0;
        while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') {
            v = v * 10 + (s[pos] - '0');
            pos++;
            if (++digits > 18) return false;
        }
        if (digits == 0 || pos >= s.size() || s[pos] != 'e') return false;
        pos++;
        n.type = BNode::INT;
        n.i = neg ? -v : v;
    } else if (c >= '0' && c <= '9') {
        size_t len = 0;
        int digits = 0;
        while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') {
            len = len * 10 + (s[pos] - '0');
            pos++;
            if (++digits > 10) return false;
        }
        if (pos >= s.size() || s[pos] != ':') return false;
        pos++;
        if (len > s.size() - pos) return false;
        n.type = BNode::STR;
        n.s.assign(s, pos, len);
        pos += len;
    } else if (c == 'l') {
        pos++;
        n.type = BNode::LIST;
        for (;;) {
            if (pos >= s.size()) return false;
            if (s[pos] == 'e') { pos++; break; }
            BNode child;
            if (!parse(s, pos, child, depth + 1)) return false;
            n.l.push_back(std::move(child));
        }
    } else if (c == 'd') {
        pos++;
        n.type = BNode::DICT;
        for (;;) {
            if (pos >= s.size()) return false;
            if (s[pos] == 'e') { pos++; break; }
            BNode key;
            if (!parse(s, pos, key, depth + 1) || key.type != BNode::STR) return false;
            BNode val;
            if (!parse(s, pos, val, depth + 1)) return false;
            n.keys.push_back(key.s);
            n.vals.push_back(std::move(val));
        }
    } else {
        return false;
    }
    n.end = pos;
    return true;
}

bool bdecode(const std::string& data, BNode& out) {
    size_t pos = 0;
    return parse(data, pos, out, 0);
}