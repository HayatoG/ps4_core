/* Reader-preferring read/write lock (the semantics of glibc's default pthread_rwlock_t): a
 * reader waits only while a writer holds the lock, never for a writer that is still waiting.
 * Readers may then nest, on one thread or across threads that wait on each other.
 * winpthreads' pthread_rwlock_wrlock keeps its internal mutex while it waits for the readers to
 * leave (rwlock.c: rwlock_gain_both_locks, then pthread_cond_wait on ccomplete), so a reader that
 * reads again behind a queued writer blocks forever, and the writer with it. runtime_memory.c
 * nests readers: code under its table lock writes guest memory, a GPU-tracked page faults, and
 * the fault handler looks the table up again (or waits on the GPU thread, which does).
 * Writers can starve while readers keep overlapping, as with glibc's default. */
#ifndef TLG_RWLOCK_RP_H
#define TLG_RWLOCK_RP_H
#include <errno.h>
#include <pthread.h>

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t readers_done, writer_done;
    unsigned readers, writers_waiting;
    int writer;
} RpLock;
#define RP_LOCK_INITIALIZER {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, PTHREAD_COND_INITIALIZER, 0, 0, 0}

static inline void rp_read_lock(RpLock *l) {
    pthread_mutex_lock(&l->mutex);
    while (l->writer) pthread_cond_wait(&l->writer_done, &l->mutex);
    ++l->readers;
    pthread_mutex_unlock(&l->mutex);
}
static inline int rp_try_read_lock(RpLock *l) {
    pthread_mutex_lock(&l->mutex);
    const int busy = l->writer;
    if (!busy) ++l->readers;
    pthread_mutex_unlock(&l->mutex);
    return busy ? EBUSY : 0;
}
static inline void rp_read_unlock(RpLock *l) {
    pthread_mutex_lock(&l->mutex);
    if (!--l->readers && l->writers_waiting) pthread_cond_signal(&l->readers_done);
    pthread_mutex_unlock(&l->mutex);
}
static inline void rp_write_lock(RpLock *l) {
    pthread_mutex_lock(&l->mutex);
    ++l->writers_waiting;
    while (l->writer || l->readers) pthread_cond_wait(&l->readers_done, &l->mutex);
    --l->writers_waiting;
    l->writer = 1;
    pthread_mutex_unlock(&l->mutex);
}
static inline int rp_try_write_lock(RpLock *l) {
    pthread_mutex_lock(&l->mutex);
    const int busy = l->writer || l->readers;
    if (!busy) l->writer = 1;
    pthread_mutex_unlock(&l->mutex);
    return busy ? EBUSY : 0;
}
static inline void rp_write_unlock(RpLock *l) {
    pthread_mutex_lock(&l->mutex);
    l->writer = 0;
    if (l->writers_waiting) pthread_cond_signal(&l->readers_done);
    pthread_cond_broadcast(&l->writer_done);
    pthread_mutex_unlock(&l->mutex);
}
#endif
