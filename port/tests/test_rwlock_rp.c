/* rwlock_rp.h: readers nest behind a queued writer (the case winpthreads' rwlock deadlocks on),
 * writers still exclude readers and each other. alarm() turns a deadlock into a failure. */
#define _GNU_SOURCE
#include "rwlock_rp.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <time.h>
#include <stdint.h>
#include <unistd.h>

static RpLock lock = RP_LOCK_INITIALIZER;
static int writer_done, shared_value;

static void nap(void) { struct timespec t = {0, 1000000}; nanosleep(&t, NULL); }
static unsigned waiting_writers(void) {
    pthread_mutex_lock(&lock.mutex);
    unsigned n = lock.writers_waiting;
    pthread_mutex_unlock(&lock.mutex);
    return n;
}
static void wait_for_queued_writer(void) {
    for (int i = 0; i < 2000 && !waiting_writers(); ++i) nap();
    assert(waiting_writers() == 1);
}
static void *writer(void *arg) {
    (void)arg;
    rp_write_lock(&lock);
    shared_value = 2;
    __atomic_store_n(&writer_done, 1, __ATOMIC_RELEASE);
    rp_write_unlock(&lock);
    return NULL;
}
static void *reader(void *arg) {
    (void)arg;
    rp_read_lock(&lock);
    int value = shared_value;
    rp_read_unlock(&lock);
    return (void *)(intptr_t)value;
}

int main(void) {
#ifndef _WIN32
    alarm(10);
#endif
    /* Same thread: a reader reads again while a writer waits (the fault handler case). */
    rp_read_lock(&lock);
    pthread_t w;
    assert(!pthread_create(&w, NULL, writer, NULL));
    wait_for_queued_writer();
    rp_read_lock(&lock);
    assert(!__atomic_load_n(&writer_done, __ATOMIC_ACQUIRE));
    assert(rp_try_read_lock(&lock) == 0);
    rp_read_unlock(&lock);
    /* Another thread reads too while the writer waits (a fault waiting on the GPU thread). */
    pthread_t r;
    assert(!pthread_create(&r, NULL, reader, NULL));
    void *seen;
    assert(!pthread_join(r, &seen) && (intptr_t)seen == 0);
    assert(rp_try_write_lock(&lock) == EBUSY);
    rp_read_unlock(&lock);
    assert(!__atomic_load_n(&writer_done, __ATOMIC_ACQUIRE));
    rp_read_unlock(&lock);
    assert(!pthread_join(w, NULL) && writer_done && shared_value == 2);

    /* A writer excludes readers and other writers. */
    rp_write_lock(&lock);
    assert(rp_try_read_lock(&lock) == EBUSY && rp_try_write_lock(&lock) == EBUSY);
    assert(!pthread_create(&r, NULL, reader, NULL));
    shared_value = 3;
    for (int i = 0; i < 20; ++i) nap(); /* the reader is blocked meanwhile */
    rp_write_unlock(&lock);
    assert(!pthread_join(r, &seen) && (intptr_t)seen == 3);
    /* Writers queued behind a writer all get through. */
    writer_done = 0;
    rp_write_lock(&lock);
    pthread_t w1, w2;
    assert(!pthread_create(&w1, NULL, writer, NULL) && !pthread_create(&w2, NULL, writer, NULL));
    for (int i = 0; i < 20; ++i) nap();
    rp_write_unlock(&lock);
    assert(!pthread_join(w1, NULL) && !pthread_join(w2, NULL));
    assert(rp_try_write_lock(&lock) == 0);
    rp_write_unlock(&lock);
    assert(!lock.readers && !lock.writer && !lock.writers_waiting);
    puts("reader-preferring rwlock tests passed");
    return 0;
}
