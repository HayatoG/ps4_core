#define _GNU_SOURCE
#include "runtime.h"
#include <stdlib.h>

/* The settings menu of the GPU library restarts through probe.c, which tests do not link. */
void runtime_restart(void) { abort(); }
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

typedef int32_t (ABI *MutexInit)(void **, void **, const char *);
typedef int32_t (ABI *MutexOp)(void **);
typedef int32_t (ABI *CondInit)(void **, void **, const char *);
typedef int32_t (ABI *CondWait)(void **, void **);
typedef int32_t (ABI *CondOp)(void **);
typedef int32_t (ABI *SemCreate)(uint32_t *, const char *, uint32_t, int32_t, int32_t, const void *);
typedef int32_t (ABI *SemWait)(uint32_t, int32_t, uint32_t *);
typedef int32_t (ABI *SemSignal)(uint32_t, int32_t);
#define GET(t, n) ((t)runtime_resolve(n, 0))
static MutexInit mutex_init;
static MutexOp mutex_lock, mutex_unlock;
static CondInit cond_init;
static CondWait cond_wait;
static CondOp cond_signal;
static SemCreate sem_create;
static SemWait sem_wait;
static SemSignal sem_signal;

static void nap(void) { struct timespec t = {0, 2000000}; nanosleep(&t, NULL); }

/* Snapshot entry of the thread named `name` once it is in a marked wait. */
static RuntimeThreadInfo infos[256];
static size_t count;
static const RuntimeThreadInfo *waiting(const char *name) {
    for (int attempt = 0; attempt < 1000; ++attempt) {
        count = runtime_thread_snapshot(infos, 256);
        for (size_t i = 0; i < count; ++i)
            if (infos[i].record && !strcmp(infos[i].name, name) && infos[i].wait.depth) return &infos[i];
        nap();
    }
    return NULL;
}
static void describe(const RuntimeThreadInfo *t, char *out, size_t size) {
    runtime_freeze_describe_wait(out, size, &t->wait, runtime_wait_now_ms(), infos, count);
}

static void monitor_steps(void) {
    FreezeMonitor m = {.seconds = 10};
    assert(runtime_freeze_step(&m, 0, 50000) == 0);   /* no frame yet: not armed */
    assert(runtime_freeze_step(&m, 1, 60000) == 0);
    assert(runtime_freeze_step(&m, 1, 69999) == 0);
    assert(runtime_freeze_step(&m, 1, 70000) == 1);   /* 10 s without a new frame */
    assert(runtime_freeze_step(&m, 1, 90000) == 0);   /* once per freeze */
    assert(runtime_freeze_step(&m, 2, 95000) == 2 && m.freeze_ms == 35000);
    assert(runtime_freeze_step(&m, 3, 96000) == 0);
    assert(runtime_freeze_step(&m, 3, 106000) == 1);  /* re-armed */
    FreezeMonitor off = {.seconds = 0};
    assert(runtime_freeze_step(&off, 1, 0) == 0 && runtime_freeze_step(&off, 1, 1000000) == 0);
}

static void slot_levels(void) {
    RuntimeWait w = {0};
    char text[512];
    runtime_freeze_describe_wait(text, sizeof(text), &w, 0, NULL, 0);
    assert(!strcmp(text, "running (not in a marked wait)"));
    runtime_wait_slot_begin(&w, "file read", (const void *)5, "/app0/very/long/path/to/an/archive/file/name.psarc", NULL, 0, 1000);
    runtime_wait_slot_begin(&w, "GPU readback", (const void *)0x1234000, NULL, NULL, 0, 1500);
    assert(w.depth == 2 && w.level[0].kind && w.level[1].kind);
    assert(strlen(w.level[0].name) == sizeof(w.level[0].name) - 1 && !strncmp(w.level[0].name, "...", 3));
    assert(strstr(w.level[0].name, "name.psarc"));
    runtime_freeze_describe_wait(text, sizeof(text), &w, 2000, NULL, 0);
    assert(!strncmp(text, "waiting on GPU readback 0x1234000 for 500 ms (inside file read '...", 66));
    assert(strstr(text, "(fd 5) for 1000 ms)"));
    runtime_wait_slot_begin(&w, "sleep", (const void *)250, NULL, NULL, 0, 1900); /* third level: counted only */
    runtime_freeze_describe_wait(text, sizeof(text), &w, 2000, NULL, 0);
    assert(strstr(text, "(+1 nested waits)"));
    runtime_wait_slot_end(&w);
    runtime_wait_slot_end(&w);
    assert(w.depth == 1 && !w.level[1].kind);
    runtime_wait_slot_end(&w);
    runtime_wait_slot_end(&w); /* unbalanced end: ignored */
    assert(w.depth == 0 && !w.level[0].kind);
    runtime_wait_slot_begin(&w, "sleep", (const void *)250, NULL, NULL, 0, 0);
    runtime_freeze_describe_wait(text, sizeof(text), &w, 3, NULL, 0);
    assert(!strcmp(text, "waiting on sleep of 250 us for 3 ms"));
    RuntimeWait copy;
    assert(runtime_wait_copy(&w, &copy) && copy.depth == 1 && copy.level[0].kind == w.level[0].kind);
}

static void *mutex = NULL, *cond = NULL;
static uint32_t sema;
static void *lock_waiter(void *arg) {
    (void)arg;
    runtime_thread_attach_host("mutex waiter");
    assert(mutex_lock(&mutex) == 0);
    assert(mutex_unlock(&mutex) == 0);
    return NULL;
}
static int ready;
static void *cond_waiter(void *arg) {
    (void)arg;
    runtime_thread_attach_host("cond waiter");
    assert(mutex_lock(&mutex) == 0);
    while (!ready) assert(cond_wait(&cond, &mutex) == 0);
    assert(mutex_unlock(&mutex) == 0);
    return NULL;
}
static void *sema_waiter(void *arg) {
    (void)arg;
    runtime_thread_attach_host("sema waiter");
    assert(sem_wait(sema, 1, NULL) == 0);
    return NULL;
}

static void contended_mutex(void) {
    char text[512];
    assert(mutex_init(&mutex, NULL, "test lock") == 0);
    assert(mutex_lock(&mutex) == 0);
    pthread_t thread;
    assert(!pthread_create(&thread, NULL, lock_waiter, NULL));
    const RuntimeThreadInfo *t = waiting("mutex waiter");
    assert(t);
    describe(t, text, sizeof(text));
    assert(!strncmp(text, "waiting on mutex 'test lock' 0x", 31));
    assert(strstr(text, ", held by 'main' (tid "));
    assert(t->wait.level[0].frame); /* the guest-called function's frame */
    /* The whole report: header, the waiter with its wait, registers and the wait's callers. */
    FILE *out = tmpfile();
    assert(out);
    runtime_freeze_report(out, "FREEZE: test; threads:\n");
    size_t size = (size_t)ftell(out);
    char *report = calloc(1, size + 1);
    rewind(out);
    assert(fread(report, 1, size, out) == size);
    fclose(out);
    assert(!strncmp(report, "FREEZE: test; threads:\n  frames: ", 33));
    const char *line = strstr(report, "  guest 'mutex waiter' (tid ");
    assert(line && strstr(line, "): waiting on mutex 'test lock'"));
    assert(strstr(line, "      now at ") && strstr(line, "      wait called from "));
    assert(strstr(report, "FREEZE: end of report ("));
    free(report);
    assert(mutex_unlock(&mutex) == 0);
    assert(!pthread_join(thread, NULL));
    count = runtime_thread_snapshot(infos, 256);
    for (size_t i = 0; i < count; ++i)
        if (!strcmp(infos[i].name, "mutex waiter")) assert(!infos[i].wait.depth && !infos[i].wait.level[0].kind);
}

static void cond_and_sema(void) {
    char text[512];
    assert(cond_init(&cond, NULL, "test cond") == 0);
    pthread_t thread;
    assert(!pthread_create(&thread, NULL, cond_waiter, NULL));
    const RuntimeThreadInfo *t = waiting("cond waiter");
    assert(t);
    describe(t, text, sizeof(text));
    assert(!strncmp(text, "waiting on condition variable 'test cond' 0x", 44));
    /* The waiter released the mutex inside the wait: main can take it, and it shows as held by main. */
    assert(mutex_lock(&mutex) == 0);
    ready = 1;
    assert(cond_signal(&cond) == 0);
    assert(mutex_unlock(&mutex) == 0);
    assert(!pthread_join(thread, NULL));

    assert(sem_create(&sema, "test sema", 1, 0, 1, NULL) == 0);
    assert(!pthread_create(&thread, NULL, sema_waiter, NULL));
    t = waiting("sema waiter");
    assert(t);
    describe(t, text, sizeof(text));
    assert(!strncmp(text, "waiting on semaphore 'test sema' id ", 36));
    assert(sem_signal(sema, 1) == 0);
    assert(!pthread_join(thread, NULL));
}

int main(void) {
    runtime_start(1);
    runtime_thread_attach_main();
    runtime_freeze_start(0, 0, 0); /* capture signal and GPU hooks, no monitor */
    mutex_init = GET(MutexInit, "cmo1RIYva9o#libkernel");
    mutex_lock = GET(MutexOp, "9UK1vLZQft4#libkernel");
    mutex_unlock = GET(MutexOp, "tn3VlD0hG60#libkernel");
    cond_init = GET(CondInit, "2Tb92quprl0#libkernel");
    cond_wait = GET(CondWait, "WKAXJ4XBPQ4#libkernel");
    cond_signal = GET(CondOp, "kDh-NfxgMtE#libkernel");
    sem_create = GET(SemCreate, "188x57JYp0g#libkernel");
    sem_wait = GET(SemWait, "Zxa0VhQVTsk#libkernel");
    sem_signal = GET(SemSignal, "4czppHBiriw#libkernel");
    assert(mutex_init && mutex_lock && mutex_unlock && cond_init && cond_wait && cond_signal &&
           sem_create && sem_wait && sem_signal);
    monitor_steps();
    slot_levels();
    contended_mutex();
    cond_and_sema();
    puts("freeze report tests passed");
    return 0;
}
