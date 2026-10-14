/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * System-library functions first needed by Gravity Rush 2 (CUSA03694 v1.00), moved to the core
 * for The Last of Us Remastered (fibers live in runtime_fiber.c, ErrorDialog in runtime_system.c).
 *
 * Small contracts of a console without extra content, network or optional features: content
 * search/export/delete, error dialogs, temporary data, kernel queries, POSIX sockets. Image
 * codecs log their first use; they are filled in when the game is seen using them. libSecure is
 * the game's own sysdata/libSceSecure.prx, linked as a module.
 */
#define _GNU_SOURCE
#include "runtime.h"
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#ifndef _WIN32
#include <sys/random.h>
#include <ucontext.h>
#else
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#define KERR(n) ((int32_t)(UINT32_C(0x80020000)|(n)))
#define KERR_EINVAL KERR(22)
#define KERR_ENOENT KERR(2)
#define KERR_EPERM KERR(1)

static void once(const char *what) {
    static const char *seen[128];
    static int count;
    static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&lock);
    for (int i=0;i<count;++i) if (seen[i]==what) { pthread_mutex_unlock(&lock); return; }
    if (count<128) seen[count++]=what;
    pthread_mutex_unlock(&lock);
    printf("Runtime: %s (GR2 placeholder)\n",what);
    fflush(stdout);
}

/* ---- Kernel ---- */
static ABI int32_t kernel_queue_event_ok(void *eq,int32_t fd,uint64_t size,void *udata) {
    (void)eq; (void)fd; (void)size; (void)udata;
    once("sceKernelAdd/DeleteRead/WriteEvent (fd events never fire)");
    return 0;
}
static ABI int32_t kernel_current_cpu(void) {
#ifdef _WIN32
    int cpu=(int)GetCurrentProcessorNumber();
#else
    int cpu=sched_getcpu();
#endif
    return cpu<0 ? 0 : cpu%7; /* the game sees the PS4's seven usable cores */
}
static ABI int32_t kernel_is_stack(void *address,void **start,void **end) {
    void *base=NULL; size_t size=0;
#ifdef _WIN32
    ULONG_PTR low=0, high=0;
    GetCurrentThreadStackLimits(&low,&high);
    base=(void *)low; size=(size_t)(high-low);
#else
    pthread_attr_t attr;
    if (!pthread_getattr_np(pthread_self(),&attr)) {
        pthread_attr_getstack(&attr,&base,&size);
        pthread_attr_destroy(&attr);
    }
#endif
    int inside=base && (char *)address>=(char *)base && (char *)address<(char *)base+size;
    if (start) *start=inside ? base : NULL;
    if (end) *end=inside ? (char *)base+size : NULL;
    return 0;
}
static ABI int32_t kernel_load_start_module(const char *path,uint64_t args,const void *argp,uint32_t flags,
                                            const void *option,int32_t *result) {
    (void)args; (void)argp; (void)flags; (void)option;
    /* The game's own modules (/app0/...) are linked ahead of time (link_modules.py --modules),
     * so loading one succeeds: its exports are already bound to the eboot's imports. */
    static int32_t next_handle=0x100;
    const int own=path && !strncmp(path,"/app0/",6);
    printf("Runtime: sceKernelLoadStartModule(%s): %s\n",path ? path : "(null)",
           own ? "linked ahead of time" : "not available");
    fflush(stdout);
    if (result) *result=0;
    return own ? __atomic_fetch_add(&next_handle,1,__ATOMIC_RELAXED) : KERR_ENOENT;
}
static ABI int32_t kernel_set_gpo(uint32_t bits) { (void)bits; return 0; }
static ABI int32_t kernel_sleep(uint32_t seconds) { sleep(seconds); return 0; }
static ABI int32_t kernel_uuid_create(uint8_t *uuid) {
    if (!uuid) return KERR_EINVAL;
#ifdef _WIN32
    for (int i=0;i<16;++i) uuid[i]=(uint8_t)rand();
#else
    if (getrandom(uuid,16,0)!=16) for (int i=0;i<16;++i) uuid[i]=(uint8_t)rand();
#endif
    uuid[6]=(uint8_t)((uuid[6]&0x0f)|0x40); /* version 4 */
    uuid[8]=(uint8_t)((uuid[8]&0x3f)|0x80);
    return 0;
}

/* ---- POSIX sockets: no network ---- */
#define GUEST_ENETDOWN 50 /* FreeBSD */
static ABI int32_t socket_down(void) {
    once("POSIX sockets (network is down)");
    *runtime_errno()=GUEST_ENETDOWN;
    return -1;
}

/* ---- App content: temporary data, no add-on entitlements ---- */
static ABI int32_t appcontent_temp_mount2(uint32_t option,char *mount_point) {
    (void)option;
    if (!mount_point) return (int32_t)0x80D90002;
    memset(mount_point,0,16);
    strcpy(mount_point,"/temp0"); /* runtime_file maps it into the user directory */
    return 0;
}
static ABI int32_t appcontent_temp_unmount(const char *mount_point) { (void)mount_point; return 0; }
static ABI int32_t appcontent_entitlement_key(const void *label,void *key) {
    (void)label; (void)key;
    return (int32_t)0x80D90007; /* SCE_APP_CONTENT_ERROR_DRM_NO_ENTITLEMENT */
}

/* ---- Content search / export / delete: nothing installed, exports accepted and dropped ---- */
static ABI int32_t content_ok(void) { once("sceContentSearch/Export/Delete (no content)"); return 0; }
static ABI int32_t content_search_count(uint32_t *count) { if (count) *count=0; return 0; }
static ABI int32_t content_search_search(const void *condition,uint32_t start,uint32_t max,void *ids,uint32_t *found) {
    (void)condition; (void)start; (void)max; (void)ids;
    if (found) *found=0;
    return 0;
}
static ABI int32_t content_search_my_index(uint32_t *index) { if (index) *index=0; return 0; }
static ABI int32_t content_search_not_found(void) { return (int32_t)0x80D60003; }

/* ---- Save data transfer (PS3/Vita import): never available ---- */
static ABI int32_t savedata_transferring_mount(const void *mount,void *result) {
    (void)mount; (void)result;
    return (int32_t)0x809F0008; /* SCE_SAVE_DATA_ERROR_NOT_FOUND */
}

/* ---- Image codecs and libSecure: logged until the game is seen using them ---- */
static ABI int32_t png_unsupported(void) { once("scePngDec*"); return (int32_t)0x80690001; }
static ABI int32_t jpeg_dec_unsupported(void) { once("sceJpegDec*"); return (int32_t)0x80650001; }
static ABI int32_t jpeg_enc_unsupported(void) { once("sceJpegEnc*"); return (int32_t)0x80650101; }

static const RuntimeExport exports[]={
    {"sceKernelAddReadEvent",kernel_queue_event_ok}, {"sceKernelAddWriteEvent",kernel_queue_event_ok},
    {"sceKernelDeleteReadEvent",kernel_queue_event_ok}, {"sceKernelDeleteWriteEvent",kernel_queue_event_ok},
    {"sceKernelGetCurrentCpu",kernel_current_cpu}, {"sceKernelIsStack",kernel_is_stack},
    {"sceKernelLoadStartModule",kernel_load_start_module}, {"sceKernelSetGPO",kernel_set_gpo},
    {"sceKernelSleep",kernel_sleep}, {"sceKernelUuidCreate",kernel_uuid_create},
    {"socket",socket_down}, {"bind",socket_down}, {"listen",socket_down}, {"accept",socket_down},
    {"send",socket_down}, {"recv",socket_down}, {"select",socket_down}, {"shutdown",socket_down},
    {"sceAppContentTemporaryDataMount2",appcontent_temp_mount2},
    {"sceAppContentTemporaryDataUnmount",appcontent_temp_unmount},
    {"sceAppContentGetEntitlementKey",appcontent_entitlement_key},
    {"sceContentSearchInit",content_ok}, {"sceContentSearchTerm",content_ok},
    {"sceContentSearchGetNumOfContent",content_search_count},
    {"sceContentSearchSearchContent",content_search_search},
    {"sceContentSearchGetMyApplicationIndex",content_search_my_index},
    {"sceContentSearchOpenMetadataByContentId",content_search_not_found},
    {"sceContentSearchGetMetadataValue",content_search_not_found},
    {"sceContentSearchCloseMetadata",content_ok},
    {"sceContentExportInit",content_ok}, {"sceContentExportTerm",content_ok},
    {"sceContentExportStart",content_ok}, {"sceContentExportFinish",content_ok},
    {"sceContentExportFromData",content_ok}, {"sceContentExportFromDataWithThumbnail",content_ok},
    {"sceContentDeleteInitialize",content_ok}, {"sceContentDeleteTerminate",content_ok},
    {"sceContentDeleteById",content_ok},
    {"sceSaveDataTransferringMount",savedata_transferring_mount},
    {"scePngDecQueryMemorySize",png_unsupported}, {"scePngDecCreate",png_unsupported},
    {"scePngDecParseHeader",png_unsupported}, {"scePngDecDecode",png_unsupported},
    {"scePngDecDelete",png_unsupported},
    {"sceJpegDecQueryMemorySize",jpeg_dec_unsupported}, {"sceJpegDecCreate",jpeg_dec_unsupported},
    {"sceJpegDecParseHeader",jpeg_dec_unsupported}, {"sceJpegDecDecode",jpeg_dec_unsupported},
    {"sceJpegDecDelete",jpeg_dec_unsupported},
    {"sceJpegEncQueryMemorySize",jpeg_enc_unsupported}, {"sceJpegEncCreate",jpeg_enc_unsupported},
    {"sceJpegEncEncode",jpeg_enc_unsupported}, {"sceJpegEncDelete",jpeg_enc_unsupported},
};
uintptr_t runtime_misc_resolve(const char *name) {
    return RUNTIME_LOOKUP(exports,name);
}
