/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * libSceFiber: cooperative fibers on the game's own context memory (ucontext on Linux, Win32
 * fibers on Windows). First written for Gravity Rush 2; GetSelf/GetInfo/Rename/OptParam added
 * for The Last of Us Remastered, whose job system runs on fibers.
 */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <ucontext.h>
#else
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif


#define FIBER_MAGIC UINT64_C(0x5245424946325247) /* "GR2FIBER" */
#define FIBER_ERR_NULL ((int32_t)0x80590001)
#define FIBER_ERR_INVALID ((int32_t)0x80590004)
#define FIBER_ERR_PERMISSION ((int32_t)0x80590005)
#define FIBER_ERR_STATE ((int32_t)0x80590006)
typedef void (ABI *FiberEntry)(uint64_t arg_on_initialize,uint64_t arg_on_run);
#ifndef _WIN32
typedef struct HostFiber {
    ucontext_t context;
    FiberEntry entry;
    uint64_t arg_on_initialize, arg_to_deliver;
    uint64_t *resume_out; /* where the fiber's own Switch/ReturnToThread wants its next argOnRun */
    int started, running;
    char name[32];
    void *guest, *guest_context; /* for sceFiberGetSelf / sceFiberGetInfo */
    uint64_t context_size;
} HostFiber;
/* The guest SceFiber (opaque, at least 16 bytes here) holds the magic and the host object. */
typedef struct { uint64_t magic; HostFiber *host; } GuestFiber;
static __thread struct {
    ucontext_t thread_context;
    HostFiber *current;
    uint64_t *thread_return_out;
} fiber_tls;
static HostFiber *fiber_of(void *guest) {
    GuestFiber *g=guest;
    return g && g->magic==FIBER_MAGIC ? g->host : NULL;
}
static void fiber_trampoline(void) {
    HostFiber *f=fiber_tls.current;
    f->entry(f->arg_on_initialize,f->arg_to_deliver);
    fprintf(stderr,"STOP: fiber %s returned from its entry function\n",f->name);
    abort();
}
/* Separate from fiber_initialize: getcontext returns twice like setjmp, and nothing of the
 * caller's must live across it. */
static __attribute__((noinline)) void fiber_make_context(HostFiber *f,void *stack,size_t size) {
    getcontext(&f->context);
    f->context.uc_stack.ss_sp=stack;
    f->context.uc_stack.ss_size=size;
    f->context.uc_link=NULL;
    makecontext(&f->context,fiber_trampoline,0);
}
static ABI int32_t fiber_initialize(void *guest,const char *name,FiberEntry entry,uint64_t arg_on_initialize,
                                    void *context,uint64_t context_size,const void *option,uint32_t build_version) {
    (void)option; (void)build_version;
    if (!guest || !entry) return FIBER_ERR_NULL;
    HostFiber *f=calloc(1,sizeof(*f));
    if (!f) return FIBER_ERR_INVALID;
    f->entry=entry; f->arg_on_initialize=arg_on_initialize;
    f->guest=guest; f->guest_context=context; f->context_size=context_size;
    snprintf(f->name,sizeof(f->name),"%s",name ? name : "");
    size_t size=(size_t)context_size;
    void *stack=context;
    if (!stack || size<16384) { size=size<65536 ? 65536 : size; stack=malloc(size); }
    fiber_make_context(f,stack,size);
    ((GuestFiber *)guest)->magic=FIBER_MAGIC;
    ((GuestFiber *)guest)->host=f;
    return 0;
}
static ABI int32_t fiber_finalize(void *guest) {
    HostFiber *f=fiber_of(guest);
    if (!f) return FIBER_ERR_INVALID;
    if (f->running) return FIBER_ERR_STATE;
    ((GuestFiber *)guest)->magic=0;
    free(f); /* the context memory belongs to the game */
    return 0;
}
static void fiber_enter(HostFiber *f,uint64_t arg) {
    if (f->started && f->resume_out) *f->resume_out=arg;
    f->arg_to_deliver=arg;
    f->started=1; f->running=1;
    fiber_tls.current=f;
}
static ABI int32_t fiber_run(void *guest,uint64_t arg_on_run_to,uint64_t *arg_on_return) {
    HostFiber *f=fiber_of(guest);
    if (!f) return FIBER_ERR_INVALID;
    if (fiber_tls.current) return FIBER_ERR_PERMISSION; /* only a thread runs a fiber */
    if (f->running) return FIBER_ERR_STATE;
    fiber_tls.thread_return_out=arg_on_return;
    fiber_enter(f,arg_on_run_to);
    swapcontext(&fiber_tls.thread_context,&f->context);
    fiber_tls.current=NULL;
    return 0;
}
static ABI int32_t fiber_switch(void *guest,uint64_t arg_on_run_to,uint64_t *arg_on_run) {
    HostFiber *to=fiber_of(guest), *from=fiber_tls.current;
    if (!to) return FIBER_ERR_INVALID;
    if (!from) return FIBER_ERR_PERMISSION;
    if (to->running) return FIBER_ERR_STATE;
    from->resume_out=arg_on_run; from->running=0;
    fiber_enter(to,arg_on_run_to);
    swapcontext(&from->context,&to->context);
    return 0;
}
static ABI int32_t fiber_return_to_thread(uint64_t arg_on_return,uint64_t *arg_on_run) {
    HostFiber *from=fiber_tls.current;
    if (!from) return FIBER_ERR_PERMISSION;
    if (fiber_tls.thread_return_out) *fiber_tls.thread_return_out=arg_on_return;
    from->resume_out=arg_on_run; from->running=0;
    fiber_tls.current=NULL;
    swapcontext(&from->context,&fiber_tls.thread_context);
    return 0;
}
#else
/* Windows: Win32 fibers. Each fiber gets its own stack from the system (the game's context
 * memory is not needed); the thread becomes a fiber the first time it runs one. */
typedef struct HostFiber {
    void *fiber;
    FiberEntry entry;
    uint64_t arg_on_initialize, arg_to_deliver;
    uint64_t *resume_out;
    int started, running;
    char name[32];
    void *guest, *guest_context; /* for sceFiberGetSelf / sceFiberGetInfo */
    uint64_t context_size;
} HostFiber;
typedef struct { uint64_t magic; HostFiber *host; } GuestFiber;
static __thread struct {
    void *thread_fiber;
    HostFiber *current;
    uint64_t *thread_return_out;
} fiber_tls;
static HostFiber *fiber_of(void *guest) {
    GuestFiber *g=guest;
    return g && g->magic==FIBER_MAGIC ? g->host : NULL;
}
static void WINAPI fiber_trampoline(void *param) {
    HostFiber *f=param;
    f->entry(f->arg_on_initialize,f->arg_to_deliver);
    fprintf(stderr,"STOP: fiber %s returned from its entry function\n",f->name);
    abort();
}
static ABI int32_t fiber_initialize(void *guest,const char *name,FiberEntry entry,uint64_t arg_on_initialize,
                                    void *context,uint64_t context_size,const void *option,uint32_t build_version) {
    (void)context; (void)option; (void)build_version;
    if (!guest || !entry) return FIBER_ERR_NULL;
    HostFiber *f=calloc(1,sizeof(*f));
    if (!f) return FIBER_ERR_INVALID;
    f->entry=entry; f->arg_on_initialize=arg_on_initialize;
    f->guest=guest; f->guest_context=context; f->context_size=context_size;
    snprintf(f->name,sizeof(f->name),"%s",name ? name : "");
    SIZE_T size=context_size<65536 ? 65536 : (SIZE_T)context_size;
    f->fiber=CreateFiber(size,fiber_trampoline,f);
    if (!f->fiber) { free(f); return FIBER_ERR_INVALID; }
    ((GuestFiber *)guest)->magic=FIBER_MAGIC;
    ((GuestFiber *)guest)->host=f;
    return 0;
}
static ABI int32_t fiber_finalize(void *guest) {
    HostFiber *f=fiber_of(guest);
    if (!f) return FIBER_ERR_INVALID;
    if (f->running) return FIBER_ERR_STATE;
    ((GuestFiber *)guest)->magic=0;
    DeleteFiber(f->fiber);
    free(f);
    return 0;
}
static void fiber_enter(HostFiber *f,uint64_t arg) {
    if (f->started && f->resume_out) *f->resume_out=arg;
    f->arg_to_deliver=arg;
    f->started=1; f->running=1;
    fiber_tls.current=f;
}
static ABI int32_t fiber_run(void *guest,uint64_t arg_on_run_to,uint64_t *arg_on_return) {
    HostFiber *f=fiber_of(guest);
    if (!f) return FIBER_ERR_INVALID;
    if (fiber_tls.current) return FIBER_ERR_PERMISSION;
    if (f->running) return FIBER_ERR_STATE;
    if (!fiber_tls.thread_fiber) {
        fiber_tls.thread_fiber=IsThreadAFiber() ? GetCurrentFiber() : ConvertThreadToFiber(NULL);
        if (!fiber_tls.thread_fiber) return FIBER_ERR_INVALID;
    }
    fiber_tls.thread_return_out=arg_on_return;
    fiber_enter(f,arg_on_run_to);
    SwitchToFiber(f->fiber);
    fiber_tls.current=NULL;
    return 0;
}
static ABI int32_t fiber_switch(void *guest,uint64_t arg_on_run_to,uint64_t *arg_on_run) {
    HostFiber *to=fiber_of(guest), *from=fiber_tls.current;
    if (!to) return FIBER_ERR_INVALID;
    if (!from) return FIBER_ERR_PERMISSION;
    if (to->running) return FIBER_ERR_STATE;
    from->resume_out=arg_on_run; from->running=0;
    fiber_enter(to,arg_on_run_to);
    SwitchToFiber(to->fiber);
    return 0;
}
static ABI int32_t fiber_return_to_thread(uint64_t arg_on_return,uint64_t *arg_on_run) {
    HostFiber *from=fiber_tls.current;
    if (!from) return FIBER_ERR_PERMISSION;
    if (fiber_tls.thread_return_out) *fiber_tls.thread_return_out=arg_on_return;
    from->resume_out=arg_on_run; from->running=0;
    fiber_tls.current=NULL;
    SwitchToFiber(fiber_tls.thread_fiber);
    return 0;
}
#endif

/* Shared by both fiber back ends (TLOU imports them). SceFiberInfo: size, entry,
 * argOnInitialize, addrContext, sizeContext, name[32], sizeContextMargin (128 bytes). */
typedef struct {
    uint64_t size; FiberEntry entry; uint64_t arg_on_initialize;
    void *addr_context; uint64_t size_context; char name[32]; uint64_t size_context_margin;
} GuestFiberInfo;
static ABI int32_t fiber_get_self(void **out) {
    if (!out) return FIBER_ERR_NULL;
    if (!fiber_tls.current) return FIBER_ERR_PERMISSION; /* called from a thread, not a fiber */
    *out=fiber_tls.current->guest;
    return 0;
}
static ABI int32_t fiber_get_info(void *guest,GuestFiberInfo *info) {
    HostFiber *f=fiber_of(guest);
    if (!guest || !info) return FIBER_ERR_NULL;
    if (!f) return FIBER_ERR_INVALID;
    info->entry=f->entry; info->arg_on_initialize=f->arg_on_initialize;
    info->addr_context=f->guest_context; info->size_context=f->context_size;
    memcpy(info->name,f->name,sizeof(info->name));
    info->size_context_margin=(uint64_t)-1; /* no stack-usage tracking */
    return 0;
}
static ABI int32_t fiber_rename(void *guest,const char *name) {
    HostFiber *f=fiber_of(guest);
    if (!f) return FIBER_ERR_INVALID;
    snprintf(f->name,sizeof(f->name),"%s",name ? name : "");
    return 0;
}
static ABI int32_t fiber_opt_param_initialize(void *param) {
    if (!param) return FIBER_ERR_NULL;
    memset(param,0,128);
    return 0;
}

static const RuntimeExport exports[] = {
    {"_sceFiberInitializeImpl",fiber_initialize}, {"sceFiberFinalize",fiber_finalize},
    {"sceFiberRun",fiber_run}, {"sceFiberSwitch",fiber_switch},
    {"sceFiberReturnToThread",fiber_return_to_thread},
    {"sceFiberGetSelf",fiber_get_self}, {"sceFiberGetInfo",fiber_get_info},
    {"sceFiberRename",fiber_rename}, {"sceFiberOptParamInitialize",fiber_opt_param_initialize},
};
uintptr_t runtime_fiber_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
