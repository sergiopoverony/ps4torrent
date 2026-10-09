#include "resume.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static std::string g_dir = "/data/ps4torrent/resume";

void resume_set_dir(const std::string& dir) { g_dir = dir; }

static bool hash_ok(const std::string& h)
{
    if (h.size() < 8 || h.size() > 64) return false;
    for (size_t i = 0; i < h.size(); i++) {
        char c = h[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
    }
    return true;
}

static std::string path_for(const std::string& dir, const std::string& hash) { return dir + "/" + hash + ".have"; }
static std::string part_path_for(const std::string& dir, const std::string& hash) { return dir + "/" + hash + ".part"; }

static bool save_impl(const std::string& dir, const std::string& hash, uint32_t numPieces,
                      const std::vector<bool>& have, const std::vector<uint32_t>& recent)
{
    if (!hash_ok(hash) || have.size() != numPieces) return false;

    mkdir(dir.c_str(), 0777);
    std::string tmp = path_for(dir, hash) + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return false;

    static const char* HEX = "0123456789abcdef";
    std::string bits;
    bits.reserve((numPieces + 3) / 4);
    for (uint32_t i = 0; i < numPieces; i += 4) {
        int v = 0;
        for (uint32_t k = 0; k < 4; k++)
            if (i + k < numPieces && have[i + k]) v |= 8 >> k;
        bits += HEX[v];
    }

    fprintf(f, "PTRESUME 1\npieces %u\nrecent", numPieces);
    for (size_t i = 0; i < recent.size(); i++) fprintf(f, " %u", recent[i]);
    fprintf(f, "\nhave %s\nend\n", bits.c_str());

    bool ok = (fflush(f) == 0);
    ok = (fclose(f) == 0) && ok;
    if (!ok) { remove(tmp.c_str()); return false; }

    // Сначала пишем во временный файл и только потом подменяем: оборванная запись не испортит старый.
    if (rename(tmp.c_str(), path_for(dir, hash).c_str()) != 0) { remove(tmp.c_str()); return false; }
    return true;
}

static bool load_impl(const std::string& dir, const std::string& hash, uint32_t numPieces,
                      std::vector<bool>& have, std::vector<uint32_t>& recent)
{
    if (!hash_ok(hash)) return false;
    FILE* f = fopen(path_for(dir, hash).c_str(), "rb");
    if (!f) return false;

    std::string content;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) content.append(buf, n);
    fclose(f);

    // Файл должен быть целым: начинаться с заголовка и заканчиваться строкой "end".
    if (content.compare(0, 11, "PTRESUME 1\n") != 0) return false;
    if (content.size() < 5 || content.compare(content.size() - 4, 4, "end\n") != 0) return false;

    size_t p = 11;
    unsigned fileN = 0;
    if (sscanf(content.c_str() + p, "pieces %u", &fileN) != 1 || fileN != numPieces) return false;
    p = content.find('\n', p);
    if (p == std::string::npos) return false;
    p++;

    // recent
    size_t eol = content.find('\n', p);
    if (eol == std::string::npos || content.compare(p, 6, "recent") != 0) return false;
    std::vector<uint32_t> rec;
    {
        const char* s = content.c_str() + p + 6;
        const char* end = content.c_str() + eol;
        while (s < end) {
            char* e = NULL;
            unsigned long v = strtoul(s, &e, 10);
            if (e == s) break;
            if (v < numPieces) rec.push_back((uint32_t)v);
            s = e;
        }
    }
    p = eol + 1;

    // have
    eol = content.find('\n', p);
    if (eol == std::string::npos || content.compare(p, 5, "have ") != 0) return false;
    std::string bits = content.substr(p + 5, eol - (p + 5));
    if (bits.size() != (numPieces + 3) / 4) return false;

    std::vector<bool> h(numPieces, false);
    for (uint32_t i = 0; i < bits.size(); i++) {
        char c = bits[i];
        int v;
        if (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
        else return false;
        for (uint32_t k = 0; k < 4; k++)
            if ((v & (8 >> k)) && i * 4 + k < numPieces) h[i * 4 + k] = true;
    }

    have = h;
    recent = rec;
    return true;
}

static std::string sel_path(const std::string& dir, const std::string& hash) { return dir + "/" + hash + ".sel"; }

static void remove_impl(const std::string& dir, const std::string& hash)
{
    if (!hash_ok(hash)) return;
    remove(path_for(dir, hash).c_str());
    remove(part_path_for(dir, hash).c_str());
}

static bool save_partial_impl(const std::string& dir, const std::string& hash, uint32_t numPieces, const std::vector<PartialPiece>& parts)
{
    if (!hash_ok(hash)) return false;
    if (parts.empty()) { remove(part_path_for(dir, hash).c_str()); return true; }

    mkdir(dir.c_str(), 0777);
    std::string tmp = part_path_for(dir, hash) + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return false;

    static const char* HEX = "0123456789abcdef";
    fprintf(f, "PTPART 1\npieces %u\n", numPieces);
    for (size_t i = 0; i < parts.size(); i++) {
        const PartialPiece& p = parts[i];
        std::string bits;
        for (uint32_t b = 0; b < p.nblocks; b += 4) {
            int v = 0;
            for (uint32_t k = 0; k < 4; k++)
                if (b + k < p.nblocks && b + k < p.got.size() && p.got[b + k]) v |= 8 >> k;
            bits += HEX[v];
        }
        fprintf(f, "piece %u %u %s\n", p.index, p.nblocks, bits.c_str());
    }
    fprintf(f, "end\n");

    bool ok = (fflush(f) == 0);
    ok = (fclose(f) == 0) && ok;
    if (!ok) { remove(tmp.c_str()); return false; }
    if (rename(tmp.c_str(), part_path_for(dir, hash).c_str()) != 0) { remove(tmp.c_str()); return false; }
    return true;
}

static bool load_partial_impl(const std::string& dir, const std::string& hash, uint32_t numPieces, std::vector<PartialPiece>& parts)
{
    parts.clear();
    if (!hash_ok(hash)) return false;
    FILE* f = fopen(part_path_for(dir, hash).c_str(), "rb");
    if (!f) return false;

    std::string content;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        content.append(buf, n);
        if (content.size() > 1024 * 1024) { fclose(f); return false; }   // подозрительно большой файл
    }
    fclose(f);

    if (content.compare(0, 9, "PTPART 1\n") != 0) return false;
    if (content.size() < 5 || content.compare(content.size() - 4, 4, "end\n") != 0) return false;

    size_t p = 9;
    unsigned fileN = 0;
    if (sscanf(content.c_str() + p, "pieces %u", &fileN) != 1 || fileN != numPieces) return false;
    p = content.find('\n', p);
    if (p == std::string::npos) return false;
    p++;

    std::vector<PartialPiece> out;
    while (p < content.size() && content.compare(p, 4, "end\n") != 0) {
        size_t eol = content.find('\n', p);
        if (eol == std::string::npos) return false;
        unsigned idx = 0, nb = 0;
        char bits[1100];
        if (sscanf(content.substr(p, eol - p).c_str(), "piece %u %u %1099s", &idx, &nb, bits) != 3) return false;
        p = eol + 1;
        if (idx >= numPieces || nb == 0 || nb > 4096 || strlen(bits) != (nb + 3) / 4) return false;
        PartialPiece pp;
        pp.index = idx;
        pp.nblocks = nb;
        pp.got.assign(nb, 0);
        for (uint32_t i = 0; bits[i]; i++) {
            char c = bits[i];
            int v;
            if (c >= '0' && c <= '9') v = c - '0';
            else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
            else return false;
            for (uint32_t k = 0; k < 4; k++)
                if ((v & (8 >> k)) && i * 4 + k < nb) pp.got[i * 4 + k] = 1;
        }
        out.push_back(pp);
        if (out.size() > 64) return false;
    }
    parts = out;
    return true;
}


// ---- публичный интерфейс
// Варианты без папки (для главного потока) работают с папкой, заданной resume_set_dir().
bool resume_save(const std::string& hash, uint32_t numPieces, const std::vector<bool>& have, const std::vector<uint32_t>& recent)
{ return save_impl(g_dir, hash, numPieces, have, recent); }
bool resume_load(const std::string& hash, uint32_t numPieces, std::vector<bool>& have, std::vector<uint32_t>& recent)
{ return load_impl(g_dir, hash, numPieces, have, recent); }
void resume_remove(const std::string& hash) { remove_impl(g_dir, hash); }
bool resume_save_partial(const std::string& hash, uint32_t numPieces, const std::vector<PartialPiece>& parts)
{ return save_partial_impl(g_dir, hash, numPieces, parts); }
bool resume_load_partial(const std::string& hash, uint32_t numPieces, std::vector<PartialPiece>& parts)
{ return load_partial_impl(g_dir, hash, numPieces, parts); }

// Варианты с явной папкой не используют общее состояние: их можно вызывать из потока записи.
bool resume_save_at(const std::string& dir, const std::string& hash, uint32_t numPieces, const std::vector<bool>& have,
                    const std::vector<uint32_t>& recent)
{ return save_impl(dir, hash, numPieces, have, recent); }

// Сохраняет прогресс, объединив его с тем, что уже лежит в файле (новый запуск этой же раздачи мог его обновить).
bool resume_merge_save_at(const std::string& dir, const std::string& hash, uint32_t numPieces, const std::vector<bool>& have,
                          const std::vector<uint32_t>& recent)
{
    std::vector<bool> merged = have;
    std::vector<bool> cur;
    std::vector<uint32_t> rec;
    if (load_impl(dir, hash, numPieces, cur, rec) && cur.size() == merged.size())
        for (size_t i = 0; i < merged.size(); i++) if (cur[i]) merged[i] = true;
    return save_impl(dir, hash, numPieces, merged, recent);
}
bool resume_save_partial_at(const std::string& dir, const std::string& hash, uint32_t numPieces, const std::vector<PartialPiece>& parts)
{ return save_partial_impl(dir, hash, numPieces, parts); }


bool resume_save_sel_at(const std::string& dir, const std::string& hash, const std::string& skipList)
{
    if (!hash_ok(hash)) return false;
    std::string path = sel_path(dir, hash);
    if (skipList.empty()) { remove(path.c_str()); return true; }
    mkdir(dir.c_str(), 0777);
    std::string tmp = path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "w");
    if (!f) return false;
    fprintf(f, "skip %s\n", skipList.c_str());
    bool ok = (fclose(f) == 0);
    if (!ok || rename(tmp.c_str(), path.c_str()) != 0) { remove(tmp.c_str()); return false; }
    return true;
}

bool resume_load_sel(const std::string& dir, const std::string& hash, std::string& skipList)
{
    skipList.clear();
    if (!hash_ok(hash)) return false;
    FILE* f = fopen(sel_path(dir, hash).c_str(), "r");
    if (!f) return false;
    char line[8192];
    bool ok = false;
    if (fgets(line, sizeof(line), f) && strncmp(line, "skip ", 5) == 0) {
        skipList = line + 5;
        while (!skipList.empty() && (skipList[skipList.size() - 1] == '\n' || skipList[skipList.size() - 1] == '\r' || skipList[skipList.size() - 1] == ' ')) skipList.erase(skipList.size() - 1);
        ok = !skipList.empty();
    }
    fclose(f);
    return ok;
}
