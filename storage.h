#pragma once
#include <stdint.h>
#include <pthread.h>
#include <atomic>
#include <string>
#include <vector>
#include "torrent.h"

// Работа с файлами раздачи. Операции чтения и записи (io) защищены замком: с одним и тем же Storage работают
// главный поток (проверка, восстановление, сохранение при остановке) и поток записи (writer.cpp).
class Storage {
public:
    Storage() : pending_(0) { pthread_mutex_init(&mu_, NULL); }
    ~Storage() { close(); pthread_mutex_destroy(&mu_); }
    Storage(const Storage&) = delete;
    Storage& operator=(const Storage&) = delete;

    bool open(const Torrent& t, const std::string& root, std::string& err);
    void close();
    bool write_piece(uint32_t index, const unsigned char* data, size_t len);
    // Записывает часть куска (offset внутри куска). Нужна для порционной записи и сохранения недокачанных кусков.
    bool write_piece_part(uint32_t index, uint32_t offset, const unsigned char* data, size_t len);
    bool read_piece(uint32_t index, std::vector<unsigned char>& out);
    // Читает часть куска (для проверки по кусочкам). offset и len считаются внутри куска.
    bool read_piece_part(uint32_t index, uint32_t offset, unsigned char* buf, size_t len);
    uint32_t piece_size(uint32_t index) const;
    // Файлы, снятые с загрузки: в них ничего не пишется (они не создаются), прочитать из них нельзя.
    void set_skip_files(const std::vector<bool>& skip) { skipFiles_ = skip; }

    // Закрывает все открытые файлы (они откроются заново при следующем обращении). Нужно после сна консоли:
    // флешка могла переподключиться, и старые дескрипторы стали недействительны. Если в этот момент идёт запись
    // в потоке записи, файлы не трогаем (возвращает -1): он сам переоткроет файл при ошибке.
    int close_all_fds();                // сколько файлов было открыто; -1: сейчас идёт запись

    // Сколько задач записи в потоке записи ещё работает с этим Storage (освобождать его можно, когда 0).
    int pending_writes() const { return pending_.load(); }
    void add_pending(int d) { pending_ += d; }

private:
    const Torrent* t_ = nullptr;
    std::string root_;
    std::vector<bool> skipFiles_;
    std::vector<int> fds_;              // -1 = файл сейчас закрыт
    std::vector<uint64_t> lastUse_;
    std::vector<uint64_t> offsets_;
    uint64_t tick_ = 0;
    int openCount_ = 0;
    pthread_mutex_t mu_;
    std::atomic<int> pending_;

    int get_fd(size_t k, bool forWrite);
    void evict_lru();
    void drop_fd(size_t k);
    bool io(uint32_t index, uint32_t pieceOffset, unsigned char* buf, size_t len, bool isWrite);        // с замером времени и замком
    bool io_impl(uint32_t index, uint32_t pieceOffset, unsigned char* buf, size_t len, bool isWrite);   // сама работа
};
