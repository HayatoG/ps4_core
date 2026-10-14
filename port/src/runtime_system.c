/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Small system contracts every game needs at boot, moved into the core from runtime_tlg.c (The
 * Last Guardian) and runtime_gr2.c (Gravity Rush 2) once the other games imported them too:
 * kernel queries (Neo mode, flexible memory, getdents, PRT aperture, module/unwind info), time
 * zone conversion, libc heap tracing, SDK version, LoadExec, the pad speaker mix level, DiscMap,
 * the PlayGo dialog and the error dialog. Same code and answers as in the game files.
 */
#define _GNU_SOURCE
#include "runtime.h"
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define KERR(n) ((int32_t)(UINT32_C(0x80020000)|(n)))
#define KERR_ESRCH KERR(3)
#define KERR_EINVAL KERR(22)

static void once(const char *what) {
    /* First call of a placeholder is logged; the list is short and fixed. */
    static const char *seen[64];
    static int count;
    static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&lock);
    for (int i=0;i<count;++i) if (seen[i]==what) { pthread_mutex_unlock(&lock); return; }
    if (count<64) seen[count++]=what;
    pthread_mutex_unlock(&lock);
    printf("Runtime: %s (placeholder)\n",what);
}

/* ---- Kernel queries ---- */
/* Base PS4 (not Pro): 1080p output path and the 0xb0000000 GPU memory layout. */
static ABI int32_t is_neo_mode(void) { return 0; }
static ABI int32_t available_flexible_memory(uint64_t *size) {
    if (!size) return KERR_EINVAL;
    *size=UINT64_C(448)*1024*1024; return 0;
}
static ABI int32_t getdents(int32_t fd,char *buffer,uint64_t size) {
    return (int32_t)runtime_file_getdents(fd,buffer,size,NULL);
}
static ABI int32_t prt_aperture(int32_t index,void **address,uint64_t *size) {
    if (index<0 || index>2 || !address || !size) return KERR_EINVAL;
    *address=NULL; *size=0; return 0;
}
/* The libc unwinder asks for a module's .eh_frame_hdr when a C++ exception is thrown. No
 * exception has been observed yet; report "no module" so it fails loudly instead of misbehaving. */
static ABI int32_t module_info_for_unwind(uintptr_t address,int32_t flags,void *info) {
    (void)flags; (void)info;
    fprintf(stderr,"Runtime: sceKernelGetModuleInfoForUnwind(%#lx) not implemented (C++ exception?)\n",(unsigned long)address);
    return KERR_ESRCH;
}
static ABI int32_t module_segment_info(void *info) {
    (void)info; once("sceKernelInternalMemoryGetModuleSegmentInfo"); return KERR_ESRCH;
}

/* ---- Time zone ---- */
/* Local time and its offset from UTC in seconds (Windows has no tm_gmtoff: the offset is the
 * local broken-down time read back as UTC, minus the instant). */
static int host_localtime(time_t t,struct tm *out,int64_t *offset) {
#ifdef _WIN32
    if (localtime_s(out,&t)) return 0;
    struct tm copy=*out;
    *offset=(int64_t)_mkgmtime(&copy)-(int64_t)t;
#else
    if (!localtime_r(&t,out)) return 0;
    *offset=out->tm_gmtoff;
#endif
    return 1;
}
typedef struct { int64_t t; uint32_t west_sec, dst_sec; } Timesec;
typedef struct { int32_t minutes_west, dst_time; } Timezone;
static ABI int32_t utc_to_local(int64_t utc,int64_t *local,Timesec *ts,uint64_t *dst) {
    struct tm tm; int64_t offset;
    if (!host_localtime((time_t)utc,&tm,&offset)) return KERR_EINVAL;
    int64_t dst_sec=tm.tm_isdst>0 ? 3600 : 0;
    if (local) *local=utc+offset;
    if (ts) { ts->t=utc; ts->west_sec=(uint32_t)(offset-dst_sec); ts->dst_sec=(uint32_t)dst_sec; }
    if (dst) *dst=(uint64_t)dst_sec;
    return 0;
}
static ABI int32_t local_to_utc(int64_t local,int64_t unused,int64_t *utc,Timezone *tz,int32_t *dst) {
    (void)unused;
    struct tm tm; int64_t offset;
    if (!host_localtime((time_t)local,&tm,&offset)) return KERR_EINVAL;
    if (utc) *utc=local-offset;
    if (tz) { tz->minutes_west=(int32_t)(-offset/60); tz->dst_time=tm.tm_isdst>0; }
    if (dst) *dst=tm.tm_isdst>0 ? 3600 : 0;
    return 0;
}

/* ---- libc internal heap tracing: report no tracing ---- */
typedef struct { uint64_t size; uint32_t flag, get_segment_info; uint64_t *mspace_id_mask, *mstate_table; } HeapTraceInfo;
static ABI int32_t libc_heap_trace_info(HeapTraceInfo *info) {
    static uint64_t mask, table[64];
    if (!info) return -1;
    info->get_segment_info=0; info->mspace_id_mask=&mask; info->mstate_table=table;
    return 0;
}

/* Needed by libSceNgs2.sprx (console firmware module, TLG_NGS2=native). */
static ABI int32_t compiled_sdk_version(int32_t *version) {
    if (!version) return KERR_EINVAL;
    *version = 0x04008061; /* the SDK the game's own modules report (FIOS2 log) */
    return 0;
}
static ABI int32_t posix_pthread_equal(void *a, void *b) { return a == b; }
/* libSceLibcInternal export the game's libc.prx does not carry. */
static ABI void guest_sincosf(float x, float *s, float *c) { *s = sinf(x); *c = cosf(x); }

/* ---- System services ---- */
static ABI int32_t load_exec(const char *path,char *const *argv) {
    printf("Runtime: sceSystemServiceLoadExec(%s%s) -> restart\n",path ? path : "(null)",argv && argv[0] ? " ..." : "");
    fflush(NULL);
    runtime_restart();
    return 0;
}
static ABI int32_t pad_speaker_mix(int32_t handle,int32_t level) { (void)handle; (void)level; return 0; }
#define DISC_MAP_NO_BITMAP ((int32_t)0x81100004)
static ABI int32_t discmap_7c98(const char *path,int64_t offset,int64_t size,int32_t *a,int32_t *b,int32_t *c) {
    (void)path; (void)offset; (void)size; (void)a; (void)b; (void)c; return DISC_MAP_NO_BITMAP;
}

/* PlayGo dialog: only opened when a chunk is missing, which never happens here. */
#define DIALOG_STATUS_FINISHED 3
static ABI int32_t playgo_dialog_init(void) { return 0; }
static ABI int32_t playgo_dialog_open(const void *param) { (void)param; once("scePlayGoDialogOpen"); return 0; }
static ABI int32_t playgo_dialog_status(void) { return DIALOG_STATUS_FINISHED; }
static ABI int32_t playgo_dialog_result(int32_t *result) { if (result) *result=0; return 0; }
static ABI int32_t playgo_dialog_term(void) { return 0; }
static ABI int32_t playgo_dialog_close(void) { return 0; }

/* ---- Error dialog: logged, finishes at once ---- */
typedef struct { uint64_t size; int32_t error_code; int32_t user_id; } ErrorDialogParam;
static int error_dialog_status;
static ABI int32_t error_dialog_init(void) { error_dialog_status=1; return 0; }
static ABI int32_t error_dialog_open(const ErrorDialogParam *param) {
    printf("Runtime: sceErrorDialogOpen(error %#x)\n",param ? (unsigned)param->error_code : 0u);
    fflush(stdout);
    error_dialog_status=3; /* FINISHED */
    return 0;
}
static ABI int32_t error_dialog_status_get(void) { return error_dialog_status; }
static ABI int32_t error_dialog_term(void) { error_dialog_status=0; return 0; }

static const RuntimeExport exports[]={
    {"sceKernelIsNeoMode",is_neo_mode}, {"sceKernelAvailableFlexibleMemorySize",available_flexible_memory},
    {"sceKernelGetdents",getdents}, {"sceKernelGetPrtAperture",prt_aperture},
    {"sceKernelGetModuleInfoForUnwind",module_info_for_unwind},
    {"sceKernelInternalMemoryGetModuleSegmentInfo",module_segment_info},
    {"sceKernelConvertUtcToLocaltime",utc_to_local}, {"sceKernelConvertLocaltimeToUtc",local_to_utc},
    {"sceLibcHeapGetTraceInfo",libc_heap_trace_info},
    {"sceKernelGetCompiledSdkVersion",compiled_sdk_version}, {"pthread_equal",posix_pthread_equal},
    {"sincosf",guest_sincosf},
    {"sceSystemServiceLoadExec",load_exec},
    {"sceAudioOutSetMixLevelPadSpk",pad_speaker_mix},
    {"sceDiscMap_7C980FFB0AA27E7A",discmap_7c98},
    {"scePlayGoDialogInitialize",playgo_dialog_init}, {"scePlayGoDialogOpen",playgo_dialog_open},
    {"scePlayGoDialogUpdateStatus",playgo_dialog_status}, {"scePlayGoDialogGetResult",playgo_dialog_result},
    {"scePlayGoDialogTerminate",playgo_dialog_term}, {"scePlayGoDialogClose",playgo_dialog_close},
    {"sceErrorDialogInitialize",error_dialog_init}, {"sceErrorDialogOpen",error_dialog_open},
    {"sceErrorDialogUpdateStatus",error_dialog_status_get}, {"sceErrorDialogGetStatus",error_dialog_status_get},
    {"sceErrorDialogTerminate",error_dialog_term},
};
uintptr_t runtime_system_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
