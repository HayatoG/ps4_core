/* Event flags (src/runtime_eventflag.c) and the shared system contracts (src/runtime_system.c),
 * moved into the core from runtime_tlg.c / runtime_gr2.c. Exports are looked up by symbol. */
#define _GNU_SOURCE
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "runtime.h"

void runtime_restart(void) { abort(); }
int64_t runtime_file_getdents(int fd, char *buffer, uint64_t size, int64_t *base) {
    (void)buffer; (void)base; return fd == 7 ? (int64_t)size : -1;
}
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *name) {
    for (size_t i = 0; i < count; ++i) if (!strcmp(name, table[i].name)) return (uintptr_t)table[i].function;
    return 0;
}
static uintptr_t get(const char *name) {
    uintptr_t f = runtime_eventflag_resolve(name);
    return f ? f : runtime_system_resolve(name);
}
#define GET(type, name) ((type)get(name))
#define KERR(n) ((int32_t)(UINT32_C(0x80020000) | (n)))

typedef int32_t (ABI *Create)(void **, const char *, uint32_t, uint64_t, const void *);
typedef int32_t (ABI *Handle)(void *);
typedef int32_t (ABI *Bits)(void *, uint64_t);
typedef int32_t (ABI *Poll)(void *, uint64_t, uint32_t, uint64_t *);
typedef int32_t (ABI *Wait)(void *, uint64_t, uint32_t, uint64_t *, uint32_t *);
typedef int32_t (ABI *Cancel)(void *, uint64_t, int32_t *);

static void *flag;
static void *setter(void *arg) {
    (void)arg;
    struct timespec t = {0, 20000000};
    nanosleep(&t, NULL);
    GET(Bits, "sceKernelSetEventFlag")(flag, 0x4);
    return NULL;
}

int main(void) {
    Create create = GET(Create, "sceKernelCreateEventFlag");
    Poll poll = GET(Poll, "sceKernelPollEventFlag");
    Wait wait = GET(Wait, "sceKernelWaitEventFlag");
    assert(create && poll && wait && GET(Handle, "sceKernelDeleteEventFlag") && GET(Cancel, "sceKernelCancelEventFlag"));
    assert(create(&flag, "test", 0x20, 0x3, NULL) == 0);
    assert(create(&flag, "bad", 0x100, 0, NULL) == KERR(22));
    uint64_t result = 0;
    assert(poll(flag, 0x1, 0x2 | 0x20, &result) == 0 && result == 0x3);       /* OR, clear pattern */
    assert(poll(flag, 0x1, 0x2, &result) == KERR(16) && result == 0x2);         /* EBUSY */
    assert(poll(flag, 0x6, 0x1, &result) == KERR(16));                          /* AND not met */
    assert(poll(flag, 0, 0x2, &result) == KERR(22));
    uint32_t timeout = 1000;
    assert(wait(flag, 0x8, 0x2, &result, &timeout) == KERR(60) && timeout == 0); /* ETIMEDOUT */
    pthread_t thread;
    pthread_create(&thread, NULL, setter, NULL);
    timeout = 2000000;
    assert(wait(flag, 0x6, 0x1 | 0x10, &result, &timeout) == 0 && result == 0x6 && timeout > 0);
    pthread_join(thread, NULL);
    assert(poll(flag, 0x6, 0x2, &result) == KERR(16) && result == 0);           /* cleared all */
    assert(GET(Bits, "sceKernelClearEventFlag")(flag, 0) == 0);
    int32_t waiters = -1;
    assert(GET(Cancel, "sceKernelCancelEventFlag")(flag, 0x9, &waiters) == 0 && waiters == 0);
    assert(poll(flag, 0x8, 0x2, &result) == 0 && result == 0x9);
    assert(GET(Handle, "sceKernelDeleteEventFlag")(flag) == 0);
    assert(GET(Handle, "sceKernelDeleteEventFlag")(flag) == KERR(3));

    /* Time zone: local = utc + offset, and back. */
    typedef int32_t (ABI *ToLocal)(int64_t, int64_t *, void *, uint64_t *);
    typedef int32_t (ABI *ToUtc)(int64_t, int64_t, int64_t *, void *, int32_t *);
    int64_t local = 0, utc = 0;
    assert(GET(ToLocal, "sceKernelConvertUtcToLocaltime")(1700000000, &local, NULL, NULL) == 0);
    assert(GET(ToUtc, "sceKernelConvertLocaltimeToUtc")(local, 0, &utc, NULL, NULL) == 0);
    assert(utc - 1700000000 <= 3600 && 1700000000 - utc <= 3600);
    /* Kernel queries and dialogs: the answers the games got from runtime_tlg.c / runtime_gr2.c. */
    assert(GET(int32_t (ABI *)(void), "sceKernelIsNeoMode")() == 0);
    uint64_t size = 0;
    assert(GET(int32_t (ABI *)(uint64_t *), "sceKernelAvailableFlexibleMemorySize")(&size) == 0 && size == 448u << 20);
    void *aperture = &size; size = 1;
    assert(GET(int32_t (ABI *)(int32_t, void **, uint64_t *), "sceKernelGetPrtAperture")(1, &aperture, &size) == 0 && !aperture && !size);
    assert(GET(int32_t (ABI *)(int32_t, void **, uint64_t *), "sceKernelGetPrtAperture")(3, &aperture, &size) == KERR(22));
    assert(GET(int32_t (ABI *)(int32_t, char *, uint64_t), "sceKernelGetdents")(7, NULL, 512) == 512);
    int32_t sdk = 0;
    assert(GET(int32_t (ABI *)(int32_t *), "sceKernelGetCompiledSdkVersion")(&sdk) == 0 && sdk == 0x04008061);
    assert(GET(int32_t (ABI *)(const char *, int64_t, int64_t, int32_t *, int32_t *, int32_t *), "sceDiscMap_7C980FFB0AA27E7A")(
               "x", 0, 0, NULL, NULL, NULL) == (int32_t)0x81100004);
    int32_t (ABI *error_status)(void) = GET(int32_t (ABI *)(void), "sceErrorDialogUpdateStatus");
    assert(GET(int32_t (ABI *)(void), "sceErrorDialogInitialize")() == 0 && error_status() == 1);
    struct { uint64_t size; int32_t error_code, user_id; } param = {16, 0x1234, 1};
    assert(GET(int32_t (ABI *)(const void *), "sceErrorDialogOpen")(&param) == 0 && error_status() == 3);
    assert(GET(int32_t (ABI *)(void), "sceErrorDialogTerminate")() == 0 && error_status() == 0);
    assert(GET(int32_t (ABI *)(void), "scePlayGoDialogUpdateStatus")() == 3);
    assert(GET(int32_t (ABI *)(void), "scePlayGoDialogClose")() == 0);
    float s = 0, c = 0;
    GET(void (ABI *)(float, float *, float *), "sincosf")(0.0f, &s, &c);
    assert(s == 0.0f && c == 1.0f);
    assert(!get("sceNgs2SystemCreateWithAllocator")); /* NGS2 stays in the game file */
    puts("PASS: event flags and shared system contracts");
    return 0;
}
