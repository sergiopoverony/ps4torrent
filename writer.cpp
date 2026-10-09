#include "writer.h"
#include "log.h"
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <deque>

namespace {

pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
struct QueueItem {
    WriteJobPtr job;
    std::function<void()> fn;
};
std::deque<QueueItem> g_queue;
std::atomic<int> g_running(0);          // выполняется сейчас (0 или 1)
std::atomic<bool> g_quit(false);
std::atomic<bool> g_started(false);
pthread_t g_thread;

void run_job(WriteJob& job)
{
    job.state = 1;
    bool ok = true;
    for (size_t i = 0; i < job.runs.size() && ok; i++) {
        const std::pair<uint32_t, uint32_t>& r = job.runs[i];
        ok = job.st->write_piece_part(job.index, r.first, job.buf.data() + r.first, r.second);
    }
    Storage* st = job.st;
    job.state = ok ? 2 : 3;
    st->add_pending(-1);                // после этого Storage можно освобождать
}

void* writer_thread(void*)
{
    while (true) {
        QueueItem qi;
        bool have = false;
        pthread_mutex_lock(&g_mu);
        if (!g_queue.empty()) {
            qi = g_queue.front();
            g_queue.pop_front();
            g_running = 1;
            have = true;
        }
        pthread_mutex_unlock(&g_mu);

        if (!have) {
            if (g_quit.load()) break;
            usleep(10000);
            continue;
        }
        if (qi.job) run_job(*qi.job);
        if (qi.fn) { try { qi.fn(); } catch (...) { } }
        g_running = 0;
    }
    return NULL;
}

} // namespace

void writer_start()
{
    bool expected = false;
    if (!g_started.compare_exchange_strong(expected, true)) return;
    if (pthread_create(&g_thread, NULL, writer_thread, NULL) != 0) {
        g_started = false;
        logf_("writer: cannot start the disk write thread");
        return;
    }
    pthread_detach(g_thread);
}

void writer_submit(const WriteJobPtr& job)
{
    job->st->add_pending(1);
    QueueItem qi;
    qi.job = job;
    pthread_mutex_lock(&g_mu);
    g_queue.push_back(qi);
    pthread_mutex_unlock(&g_mu);
}

void writer_post(std::function<void()> fn)
{
    QueueItem qi;
    qi.fn = fn;
    pthread_mutex_lock(&g_mu);
    g_queue.push_back(qi);
    pthread_mutex_unlock(&g_mu);
}

int writer_queue_length()
{
    pthread_mutex_lock(&g_mu);
    int n = (int)g_queue.size() + g_running.load();
    pthread_mutex_unlock(&g_mu);
    return n;
}

void writer_stop(int waitMs)
{
    for (int waited = 0; waited < waitMs && writer_queue_length() > 0; waited += 10) usleep(10000);
    g_quit = true;                       // поток завершится сам, когда очередь опустеет (ждать его завершения не будем)
}
