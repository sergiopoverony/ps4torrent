#include "storage.h"
#include "log.h"
#include <time.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <algorithm>

static const int MAX_OPEN = 32;

static void mkdirs_for_file(const std::string& path) {
    for (size_t i = 1; i < path.size(); i++) {
        if (path[i] == '/') mkdir(path.substr(0, i).c_str(), 0777);
    }
}

// Защита: путь из торрента не должен выходить за пределы папки загрузок.
static bool path_safe(const std::string& p) {
    if (p.empty() || p[0] == '/') return false;
    size_t start = 0;
    while (start <= p.size()) {
        size_t end = p.find('/', start);
        if (end == std::string::npos) end = p.size();
        std::string comp = p.substr(start, end - start);
        if (comp.empty() || comp == "." || comp == "..") return false;
        if (comp.size() > 255) return false;   // лимит имени файловой системы
        start = end + 1;
    }
    return true;
}

bool Storage::open(const Torrent& t, const std::string& root, std::string& err) {
    close();
    t_ = &t;
    root_ = root;
    uint64_t off = 0;
    for (size_t k = 0; k < t.files.size(); k++) {
        const TorrentFile& f = t.files[k];
        if (!path_safe(f.path)) { err = "unsafe or too long path: " + f.path; close(); return false; }
        fds_.push_back(-1);
        lastUse_.push_back(0);
        offsets_.push_back(off);
        off += f.length;
    }
    // Пустые файлы в данных не участвуют, поэтому создаём их сразу.
    for (size_t k = 0; k < t.files.size(); k++) {
        if (t.files[k].length != 0) continue;
        std::string full = root_ + "/" + t.files[k].path;
        mkdirs_for_file(full);
        int fd = ::open(full.c_str(), O_RDWR | O_CREAT, 0666);
        if (fd >= 0) ::close(fd);
    }
    return true;
}

void Storage::close() {
    for (size_t i = 0; i < fds_.size(); i++)
        if (fds_[i] >= 0) ::close(fds_[i]);
    fds_.clear();
    lastUse_.clear();
    offsets_.clear();
    openCount_ = 0;
    tick_ = 0;
}

void Storage::evict_lru() {
    size_t best = fds_.size();
    for (size_t k = 0; k < fds_.size(); k++) {
        if (fds_[k] < 0) continue;
        if (best == fds_.size() || lastUse_[k] < lastUse_[best]) best = k;
    }
    if (best == fds_.size()) return;
    ::close(fds_[best]);
    fds_[best] = -1;
    openCount_--;
}

int Storage::get_fd(size_t k, bool forWrite) {
    lastUse_[k] = ++tick_;
    if (fds_[k] >= 0) return fds_[k];
    if (openCount_ >= MAX_OPEN) evict_lru();

    std::string full = root_ + "/" + t_->files[k].path;
    int fd = -1;
    for (int attempt = 0; attempt < 3; attempt++) {
        if (forWrite) {
            mkdirs_for_file(full);
            fd = ::open(full.c_str(), O_RDWR | O_CREAT, 0666);
        } else {
            fd = ::open(full.c_str(), O_RDWR);
        }
        if (fd >= 0) break;
        if (errno == EMFILE || errno == ENFILE) { evict_lru(); continue; }
        break;
    }
    if (fd < 0) {
        // Нет файла при чтении (проверка докачки) это нормально, не логируем.
        if (forWrite || errno != ENOENT)
            logf_("    open failed errno=%d: %s", errno, full.c_str());
        return -1;
    }
    fds_[k] = fd;
    openCount_++;
    return fd;
}

uint32_t Storage::piece_size(uint32_t i) const {
    uint64_t start = (uint64_t)i * t_->pieceLength;
    uint64_t rem = t_->totalSize - start;
    return (uint32_t)(rem < t_->pieceLength ? rem : t_->pieceLength);
}

static uint64_t mono_us() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000;
}

// Обёртка с замером времени: если запись или чтение с флешки заняло больше секунды, пишем в лог
// (так видно, что главный цикл завис именно на диске). Не чаще раза в 5 секунд.
bool Storage::io(uint32_t index, uint32_t pieceOffset, unsigned char* buf, size_t len, bool isWrite) {
    pthread_mutex_lock(&mu_);
    uint64_t t0 = mono_us();
#ifdef HOST_TEST
    {   // тесты: задержка в миллисекундах из файла
        FILE* ff = fopen("/tmp/ft/slow_io", "r");
        int ms = 0;
        if (ff) { if (fscanf(ff, "%d", &ms) != 1) ms = 0; fclose(ff); }
        if (ms > 0) usleep((useconds_t)ms * 1000);
    }
#endif
#ifdef HOST_TEST
    if (isWrite) {   // тесты: модель медленной флешки "задержка на вызов (мс) + время на объём (мкс на КБ)"
        FILE* wf = fopen("/tmp/ft/write_cost", "r");
        int perCall = 0, perKB = 0;
        if (wf) { if (fscanf(wf, "%d %d", &perCall, &perKB) != 2) { perCall = 0; perKB = 0; } fclose(wf); }
        if (perCall > 0 || perKB > 0) usleep((useconds_t)(perCall * 1000 + (len / 1024) * perKB));
    }
#endif
    bool ok = io_impl(index, pieceOffset, buf, len, isWrite);
    uint64_t ms = (mono_us() - t0) / 1000;
    pthread_mutex_unlock(&mu_);
    if (ms >= 1000) {
        static time_t lastLog = 0;
        time_t now = time(NULL);
        if (now - lastLog >= 5) {
            lastLog = now;
            logf_("storage: slow %s of %zu KB (piece %u) took %llu ms%s", isWrite ? "write" : "read", len / 1024, (unsigned)index,
                  (unsigned long long)ms, ok ? "" : " and FAILED");
        }
    }
    return ok;
}

void Storage::drop_fd(size_t k) {
    if (k < fds_.size() && fds_[k] >= 0) {
        ::close(fds_[k]);
        fds_[k] = -1;
        if (openCount_ > 0) openCount_--;
    }
}

int Storage::close_all_fds() {
    if (pthread_mutex_trylock(&mu_) != 0) return -1;      // идёт запись: её поток сам переоткроет файл при ошибке
    int n = 0;
    for (size_t k = 0; k < fds_.size(); k++)
        if (fds_[k] >= 0) { drop_fd(k); n++; }
    pthread_mutex_unlock(&mu_);
    return n;
}

bool Storage::io_impl(uint32_t index, uint32_t pieceOffset, unsigned char* buf, size_t len, bool isWrite) {
    uint64_t pos = (uint64_t)index * t_->pieceLength + pieceOffset;
    size_t done = 0;
    for (size_t k = 0; k < fds_.size() && done < len; k++) {
        uint64_t fstart = offsets_[k];
        uint64_t flen = t_->files[k].length;
        uint64_t cur = pos + done;
        if (flen == 0 || cur >= fstart + flen) continue;
        uint64_t inFile = cur - fstart;
        size_t n = (size_t)std::min<uint64_t>(len - done, fstart + flen - cur);
        if (k < skipFiles_.size() && skipFiles_[k]) {      // файл снят с загрузки
            if (isWrite) { done += n; continue; }          // записывать не надо (часть куска, лежащая в этом файле, нам не нужна)
            return false;                                  // и прочитать его нельзя
        }

        // Два захода: если операция с файлом не удалась из-за ошибки ввода-вывода, дескриптор мог устареть
        // (так бывает после сна консоли, когда флешка переподключается): закрываем его, открываем файл заново
        // и повторяем эту же операцию один раз. Конец файла при чтении (r == 0) ошибкой не считается.
        bool ok = false;
        for (int attempt = 0; attempt < 2 && !ok; attempt++) {
            int fd = get_fd(k, isWrite);
            if (fd < 0) return false;                       // открыть не удалось: причина уже в логе
            bool ioError = false;
            int err = 0;
            size_t got = 0;
            if (lseek(fd, (off_t)inFile, SEEK_SET) < 0) { ioError = true; err = errno; }
            while (!ioError && got < n) {
                ssize_t r;
#ifdef HOST_TEST
                {   // тесты: пока в файле число больше нуля, запись "не удаётся" (как с устаревшим дескриптором)
                    FILE* ff = isWrite ? fopen("/tmp/ft/storage_fail", "r") : NULL;
                    int cnt = 0;
                    if (ff) { if (fscanf(ff, "%d", &cnt) != 1) cnt = 0; fclose(ff); }
                    if (cnt > 0) {
                        ff = fopen("/tmp/ft/storage_fail", "w");
                        if (ff) { fprintf(ff, "%d", cnt - 1); fclose(ff); }
                        r = -1;
                        errno = EIO;
                    } else {
                        r = isWrite ? ::write(fd, buf + done + got, n - got) : ::read(fd, buf + done + got, n - got);
                    }
                }
#else
                r = isWrite ? ::write(fd, buf + done + got, n - got) : ::read(fd, buf + done + got, n - got);
#endif
                if (r < 0) { ioError = true; err = errno; break; }
                if (r == 0) break;                          // конец файла
                got += (size_t)r;
            }
            if (got == n) { ok = true; break; }
            if (!ioError) return false;                     // обычный конец данных: повторять нечего

            // Отдельные ограничители для первого сообщения и для повторного (иначе второе подавлялось бы первым).
            static time_t lastLog[2] = {0, 0};
            time_t now = time(NULL);
            if (now - lastLog[attempt] >= 3) {
                lastLog[attempt] = now;
                const char* hint = (err == 5 || err == 6 || err == 19 || err == 2) ? " [the drive may have been disconnected]" : "";
                logf_("storage: %s failed on %s (errno %d)%s%s", isWrite ? "write" : "read", t_->files[k].path.c_str(), err, hint,
                      attempt == 0 ? ", reopening the file and trying again" : ", still failing");
            }
            drop_fd(k);                                     // следующий заход откроет файл заново
        }
        if (!ok) return false;
        done += n;
    }
    return done == len;
}

bool Storage::write_piece(uint32_t index, const unsigned char* data, size_t len) {
    return io(index, 0, const_cast<unsigned char*>(data), len, true);
}

bool Storage::write_piece_part(uint32_t index, uint32_t offset, const unsigned char* data, size_t len) {
    return io(index, offset, const_cast<unsigned char*>(data), len, true);
}

bool Storage::read_piece(uint32_t index, std::vector<unsigned char>& out) {
    out.resize(piece_size(index));
    return io(index, 0, out.data(), out.size(), false);
}

bool Storage::read_piece_part(uint32_t index, uint32_t offset, unsigned char* buf, size_t len) {
    return io(index, offset, buf, len, false);
}
