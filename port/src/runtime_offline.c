/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Offline contracts first needed by The Last of Us Remastered (CUSA00552 1.11), generic for any
 * game. They describe an offline console with the whole game installed: PSN signed out (social, invitations,
 * Facebook, web API and in-game messages fail the way they do without an account), no
 * network (sockets report the network down), no add-ons, no remote play, no camera keyboard. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "runtime.h"

#define NP_SIGNED_OUT ((int32_t)0x80550006)
#define APP_CONTENT_NOT_FOUND ((int32_t)0x80d90005)
#define HTTP_INVALID_URL ((int32_t)0x80431060)
#define RTC_INVALID_VALUE ((int32_t)0x80b50002)
#define KERNEL_ESRCH ((int32_t)0x80020003)
#define GUEST_ENETDOWN 50 /* FreeBSD */

static void once(const char *what, int *done) {
    if (*done) return;
    *done=1; printf("Runtime: %s (offline)\n", what);
}
#define ONCE(what) do { static int done_; once(what, &done_); } while (0)

static ABI int32_t ok(void) { return 0; }
static ABI int32_t signed_out(void) { return NP_SIGNED_OUT; }
static ABI int32_t not_found(void) { return APP_CONTENT_NOT_FOUND; }
/* Handle/context creators: a positive id, so the game's bookkeeping stays valid; every
 * request made with it then fails as signed out. */
static ABI int32_t new_id(void) { static int32_t next=0x40; return ++next; }

/* ---- libkernel ---- */
static ABI int32_t kernel_utimes(const char *path, const void *times) { (void)path; (void)times; return 0; }
static ABI int32_t kernel_set_range_name(void *address, uint64_t size, const char *name) {
    (void)address; (void)size; (void)name; return 0;
}
/* sceKernelGetModuleList(handles, capacity, count): the runtime keeps no module records. */
static ABI int32_t kernel_module_list(int32_t *handles, uint64_t capacity, uint64_t *count) {
    (void)handles; (void)capacity;
    ONCE("sceKernelGetModuleList -> no modules");
    if (count) *count=0;
    return 0;
}
static ABI int32_t kernel_module_info(int32_t handle, void *info) {
    printf("Runtime: sceKernelGetModuleInfo(%d) -> ESRCH\n", handle);
    (void)info; return KERNEL_ESRCH;
}
static ABI int32_t kernel_dlsym(int32_t handle, const char *symbol, void **address) {
    printf("Runtime: sceKernelDlsym(%d, %s) -> not found\n", handle, symbol ? symbol : "(null)");
    if (address) *address=NULL;
    return KERNEL_ESRCH;
}

/* ---- POSIX sockets: no network ---- */
static ABI int32_t socket_down(void) {
    ONCE("POSIX sockets (network is down)");
    *runtime_errno()=GUEST_ENETDOWN;
    return -1;
}
/* inet_pton(af, text, out): IPv4 dotted quads (FreeBSD AF_INET = 2); IPv6 (28) unsupported. */
static ABI int32_t guest_inet_pton(int32_t af, const char *text, void *out) {
    if (af!=2) { *runtime_errno()=47; return -1; } /* EAFNOSUPPORT */
    unsigned a, b, c, d; char tail;
    if (!text || sscanf(text, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail)!=4 || a>255 || b>255 || c>255 || d>255)
        return 0;
    uint8_t bytes[4]={(uint8_t)a, (uint8_t)b, (uint8_t)c, (uint8_t)d};
    memcpy(out, bytes, 4);
    return 1;
}
static ABI uint64_t net_swap64(uint64_t value) { return __builtin_bswap64(value); }

/* ---- PlayGo, AppContent: everything is installed, no add-ons ---- */
static ABI int32_t playgo_language_mask(int32_t handle, uint64_t *mask) {
    (void)handle;
    if (mask) *mask=~UINT64_C(0); /* every language chunk is present */
    return 0;
}
static ABI int32_t playgo_prefetch(int32_t handle, const uint16_t *chunks, uint32_t count, int32_t minimum) {
    (void)handle; (void)chunks; (void)count; (void)minimum; return 0;
}
static ABI int32_t download_space_kb(const char *mount_point, uint64_t *kb) {
    (void)mount_point;
    if (kb) *kb=UINT64_C(16)*1024*1024; /* 16 GiB free */
    return 0;
}

/* ---- Voice chat: no microphone; volume and mute are remembered ---- */
static uint32_t voice_volume[64], voice_muted[64];
static ABI int32_t voice_set_volume(uint32_t port, float volume) {
    if (port<64) memcpy(&voice_volume[port], &volume, 4);
    return 0;
}
static ABI int32_t voice_get_volume(uint32_t port, float *volume) {
    if (volume) { if (port<64 && voice_volume[port]) memcpy(volume, &voice_volume[port], 4); else *volume=1.0f; }
    return 0;
}
static ABI int32_t voice_set_mute(uint32_t port, int32_t muted) { if (port<64) voice_muted[port]=muted!=0; return 0; }
static ABI int32_t voice_set_mute_all(int32_t muted) { for (int i=0; i<64; i++) voice_muted[i]=muted!=0; return 0; }
static ABI int32_t voice_get_mute(uint32_t port, int32_t *muted) {
    if (muted) *muted=port<64 ? (int32_t)voice_muted[port] : 0;
    return 0;
}

/* ---- Common dialogs that need PSN (invitations, Facebook): they never open ---- */
static ABI int32_t dialog_status(void) { return 1; } /* SCE_COMMON_DIALOG_STATUS_INITIALIZED */
static ABI int32_t dialog_open(void) { ONCE("PSN dialog (invitation/Facebook) not available"); return NP_SIGNED_OUT; }

/* ---- IME keyboard: no USB keyboard is connected ---- */
static ABI int32_t ime_resource_id(int32_t user, uint32_t *ids) {
    (void)user;
    if (ids) memset(ids, 0, 4*6); /* size + 5 ids, all empty */
    return 0;
}

/* ---- Http URI helpers: only used for online requests ---- */
static ABI int32_t http_uri_parse(void) { ONCE("sceHttpUriParse/UriBuild (offline)"); return HTTP_INVALID_URL; }
static ABI int32_t rtc_parse(void *tick, const char *text) {
    printf("Runtime: sceRtcParseDateTime(%s) -> invalid\n", text ? text : "(null)");
    (void)tick; return RTC_INVALID_VALUE;
}

static const RuntimeExport exports[] = {
    {"sceKernelUtimes", kernel_utimes}, {"sceKernelSync", ok},
    {"sceKernelSetVirtualRangeName", kernel_set_range_name},
    {"sceKernelGetModuleList", kernel_module_list}, {"sceKernelGetModuleInfo", kernel_module_info},
    {"sceKernelDlsym", kernel_dlsym},
    {"connect", socket_down}, {"getpeername", socket_down}, {"getsockopt", socket_down},
    {"recvfrom", socket_down}, {"sendto", socket_down}, {"setsockopt", socket_down},
    {"inet_pton", guest_inet_pton}, {"sceNetHtonll", net_swap64}, {"sceNetNtohll", net_swap64},
    {"scePlayGoGetLanguageMask", playgo_language_mask}, {"scePlayGoPrefetch", playgo_prefetch},
    {"sceAppContentDownloadDataGetAvailableSpaceKb", download_space_kb},
    {"sceAppContentAddcontMount", not_found}, {"sceAppContentAddcontEnqueueDownload", not_found},
    {"sceVoiceSetVolume", voice_set_volume}, {"sceVoiceGetVolume", voice_get_volume},
    {"sceVoiceSetMuteFlag", voice_set_mute}, {"sceVoiceSetMuteFlagAll", voice_set_mute_all},
    {"sceVoiceGetMuteFlag", voice_get_mute},
    {"sceRemoteplayInitialize", ok}, {"sceRemoteplayTerminate", ok},
    {"sceNpCommerceShowPsStoreIcon", ok}, {"sceNpCommerceHidePsStoreIcon", ok},
    {"sceCoredumpRegisterCoredumpHandler", ok}, {"sceCoredumpWriteUserData", ok},
    {"sceInvitationDialogInitialize", ok}, {"sceInvitationDialogTerminate", ok},
    {"sceInvitationDialogOpen", dialog_open}, {"sceInvitationDialogUpdateStatus", dialog_status},
    {"sceInvitationDialogGetResult", signed_out},
    {"sceNpSnsFacebookDialogInitialize", ok}, {"sceNpSnsFacebookDialogTerminate", ok},
    {"sceNpSnsFacebookDialogOpen", dialog_open}, {"sceNpSnsFacebookDialogUpdateStatus", dialog_status},
    {"sceNpSnsFacebookDialogGetResult", signed_out},
    {"sceNpSnsFacebookCreateRequest", new_id}, {"sceNpSnsFacebookAbortRequest", ok},
    {"sceNpSnsFacebookDeleteRequest", ok}, {"sceNpSnsFacebookGetAccessToken", signed_out},
    {"sceNpLookupCreateRequest", new_id},
    {"sceNpAuthCreateRequest", new_id}, {"sceNpAuthAbortRequest", ok},
    {"sceNpSignalingGetConnectionFromNpId", signed_out}, {"sceNpSignalingGetConnectionInfo", signed_out},
    {"sceNpSignalingGetPeerNetInfoResult", signed_out}, {"sceNpSignalingTerminateConnection", ok},
    {"sceNpInGameMessageInitialize", new_id}, {"sceNpInGameMessageTerminate", ok},
    {"sceNpInGameMessageCreateHandle", new_id}, {"sceNpInGameMessageAbortHandle", ok},
    {"sceNpInGameMessagePrepare", signed_out}, {"sceNpInGameMessageSendData", signed_out},
    {"sceNpWebApiCreateHandle", new_id}, {"sceNpWebApiSendRequest2", signed_out},
    {"sceNpWebApiCreateExtdPushEventFilter", new_id}, {"sceNpWebApiDeleteExtdPushEventFilter", ok},
    {"sceNpWebApiCreateServicePushEventFilter", new_id}, {"sceNpWebApiDeleteServicePushEventFilter", ok},
    {"sceNpWebApiRegisterExtdPushEventCallback", new_id}, {"sceNpWebApiUnregisterExtdPushEventCallback", ok},
    {"sceNpWebApiRegisterServicePushEventCallback", new_id},
    {"sceImeKeyboardOpen", ok}, {"sceImeKeyboardClose", ok}, {"sceImeUpdate", ok},
    {"sceImeKeyboardGetResourceId", ime_resource_id},
    {"sceHttpUriParse", http_uri_parse}, {"sceHttpUriBuild", http_uri_parse}, {"sceHttpRemoveRequestHeader", ok},
    {"sceRtcParseDateTime", rtc_parse},
};

uintptr_t runtime_offline_resolve(const char *name) { return RUNTIME_LOOKUP(exports, name); }
