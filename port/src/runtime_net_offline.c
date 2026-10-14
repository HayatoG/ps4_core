/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * PSN and network as an offline console whose user is signed out: NpManager, Http, Ssl, Net and
 * ShareUtility. First written for Gravity Rush 2 (runtime_gr2_np.c, which keeps the game-specific
 * NpToolkit layouts); moved to the core for The Last of Us Remastered.
 */
#define _GNU_SOURCE
#include "runtime.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NP_SIGNED_OUT ((int32_t)0x80550006)
#define NP_INVALID_ARGUMENT ((int32_t)0x80550003)
#define HTTP_NETWORK ((int32_t)0x80431063)
#define HTTP_OUT_OF_MEMORY ((int32_t)0x80431022)
#define HTTP_INVALID_VALUE ((int32_t)0x804311fe)
#define HTTP_PARSE_NOT_FOUND ((int32_t)0x80432025)
#define NET_ENETUNREACH ((int32_t)0x80410133)
#define NET_ENOTCONN ((int32_t)0x80410139)
#define NET_EINVAL ((int32_t)0x80410116)
#define SSL_INVALID_CERT ((int32_t)0x8095f005)

static void once(const char *what) {
    /* First call of a placeholder is logged; the list is short and fixed. */
    static const char *seen[64];
    static int count;
    static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&lock);
    for (int i=0;i<count;++i) if (seen[i]==what) { pthread_mutex_unlock(&lock); return; }
    if (count<64) seen[count++]=what;
    pthread_mutex_unlock(&lock);
    printf("Runtime: %s (offline)\n",what);
}
/* ---- NpManager ---- */
static _Atomic int32_t next_request=0x100;
static ABI int32_t np_create_request(void) { return atomic_fetch_add(&next_request,1); }
static ABI int32_t np_wait_async(int32_t request,int32_t *result) { (void)request; if (result) *result=NP_SIGNED_OUT; return 0; }
/* PS4 language id (sceSystemServiceParamGetInt, TLG_LANGUAGE) -> account language and country. */
static void account_locale(const char **lang,const char **country) {
    static const char *const langs[][2]={
        {"ja","jp"},{"en","us"},{"fr","fr"},{"es","es"},{"de","de"},{"it","it"},{"nl","nl"},{"pt","pt"},
        {"ru","ru"},{"ko","kr"},{"zh-TW","tw"},{"zh-CN","cn"},{"fi","fi"},{"sv","se"},{"da","dk"},{"no","no"},
        {"pl","pl"},{"pt-BR","br"},{"en-GB","gb"},{"tr","tr"},{"es-LA","mx"},{"ar","ae"},{"fr-CA","ca"},
        {"cs","cz"},{"hu","hu"},{"el","gr"},{"ro","ro"},{"th","th"},{"vi","vn"},{"id","id"}};
    const char *v=getenv("TLG_LANGUAGE");
    int id=v ? atoi(v) : 1;
    if (id<0 || id>=(int)(sizeof(langs)/sizeof(*langs))) id=1;
    *lang=langs[id][0]; *country=langs[id][1];
}
/* Out-params get the system locale, but the user is signed out, as on a console. */
static ABI int32_t np_account_country(const void *online_id,char *country) {
    const char *lang,*code; (void)online_id;
    if (!country) return NP_INVALID_ARGUMENT;
    account_locale(&lang,&code);
    memset(country,0,4); memcpy(country,code,2);
    return NP_SIGNED_OUT;
}
static ABI int32_t np_account_language(int32_t request,const void *online_id,char *language) {
    const char *lang,*code; (void)request; (void)online_id;
    if (!language) return NP_INVALID_ARGUMENT;
    account_locale(&lang,&code);
    memset(language,0,6); memcpy(language,lang,strlen(lang)<5 ? strlen(lang) : 5);
    return NP_SIGNED_OUT;
}
static ABI int32_t np_account_id(const void *online_id,uint64_t *account) {
    (void)online_id; if (account) *account=0; return NP_SIGNED_OUT;
}
static ABI int32_t np_user_by_online_id(const void *online_id,int32_t *user) {
    (void)online_id; if (user) *user=-1; return NP_SIGNED_OUT;
}
static ABI int32_t np_register_reachability(void *callback,void *user) {
    (void)callback; (void)user; /* never called: the state stays "unreachable" */
    once("sceNpRegisterNpReachabilityStateCallback");
    return 0;
}
static ABI int32_t np_unregister_reachability(void) { return 0; }

/* ---- Http: requests never reach a server (sceHttpSendRequest fails in runtime_services.c) ---- */
static ABI int32_t ok(void) { return 0; }
static ABI int32_t http_all_headers(int32_t request,char **header,uint64_t *size) {
    (void)request; if (header) *header=NULL; if (size) *size=0; return HTTP_NETWORK;
}
static ABI int32_t http_last_errno(int32_t request,int32_t *err) { (void)request; if (err) *err=51; return 0; } /* ENETUNREACH */
static int same_ci(const char *a,const char *b,uint64_t n) {
    for (uint64_t i=0;i<n;++i) if ((a[i]|32)!=(b[i]|32) || !a[i]) return 0; /* header names are ASCII */
    return 1;
}
static ABI int32_t http_parse_header(const char *header,uint64_t len,const char *field,const char **value,uint64_t *vlen) {
    if (!header || !field || !value || !vlen) return HTTP_INVALID_VALUE;
    uint64_t flen=strlen(field);
    for (uint64_t i=0;i+flen<len;) {
        uint64_t e=i; while (e<len && header[e]!='\n') ++e;
        if (e-i>flen && same_ci(header+i,field,flen) && header[i+flen]==':') {
            uint64_t v=i+flen+1; while (v<e && (header[v]==' ' || header[v]=='\t')) ++v;
            uint64_t end=e; while (end>v && (header[end-1]=='\r' || header[end-1]==' ')) --end;
            *value=header+v; *vlen=end-v; return 0;
        }
        i=e+1;
    }
    return HTTP_PARSE_NOT_FOUND;
}
static ABI int32_t https_ssl_error(int32_t id,int32_t *err,uint32_t *detail) {
    (void)id; if (err) *err=0; if (detail) *detail=0; return 0;
}
static int unreserved(unsigned char c) {
    return (c>='0' && c<='9') || ((c|32)>='a' && (c|32)<='z') || c=='-' || c=='.' || c=='_' || c=='~';
}
/* RFC 3986: unreserved characters stay, the rest becomes %XX; require counts the terminator. */
static ABI int32_t http_uri_escape(char *out,uint64_t *require,uint64_t prepare,const char *in) {
    static const char hex[]="0123456789ABCDEF";
    if (!in) return HTTP_INVALID_VALUE;
    uint64_t need=1;
    for (const unsigned char *c=(const unsigned char *)in;*c;++c)
        need+=unreserved(*c) ? 1 : 3;
    if (require) *require=need;
    if (!out) return 0;
    if (prepare<need) return HTTP_OUT_OF_MEMORY;
    char *o=out;
    for (const unsigned char *c=(const unsigned char *)in;*c;++c) {
        if (unreserved(*c)) *o++=(char)*c;
        else { *o++='%'; *o++=hex[*c>>4]; *o++=hex[*c&15]; }
    }
    *o=0;
    return 0;
}

/* ---- Ssl: only reached from the https certificate callback, which never runs ---- */
static ABI int32_t ssl_no_cert(void) { return SSL_INVALID_CERT; }
static ABI int32_t ssl_pool_stats(int32_t ctx,uint64_t *stats) {
    (void)ctx; if (stats) memset(stats,0,5*sizeof(uint64_t)); return 0;
}

/* ---- Net ---- */
static ABI int32_t net_mac_address(uint8_t *mac,int32_t flags) {
    (void)flags;
    if (!mac) return NET_EINVAL;
    static const uint8_t fixed[6]={0x02,0x00,0x00,0x00,0x47,0x52}; /* locally administered */
    memcpy(mac,fixed,6); return 0;
}
static ABI int32_t net_not_connected(void) { return NET_ENOTCONN; }
static ABI int32_t net_unreachable(void) { return NET_ENETUNREACH; }
static ABI int32_t net_resolver_error(int32_t resolver,int32_t *status) {
    (void)resolver; if (status) *status=NET_ENETUNREACH; return 0;
}

/* ---- ShareUtility: the share menu does not exist on PC ---- */
static ABI int32_t share_open(void) { once("sceShareUtilityOpenShareMenu"); return 0; }

static const RuntimeExport exports[]={
    /* NpManager */
    {"sceNpCreateRequest",np_create_request}, {"sceNpWaitAsync",np_wait_async},
    {"sceNpGetAccountCountry",np_account_country}, {"sceNpGetAccountLanguage",np_account_language},
    {"sceNpGetAccountId",np_account_id}, {"sceNpGetUserIdByOnlineId",np_user_by_online_id},
    {"sceNpRegisterNpReachabilityStateCallback",np_register_reachability},
    {"sceNpUnregisterNpReachabilityStateCallback",np_unregister_reachability},
    /* Http */
    {"sceHttpAbortRequest",ok}, {"sceHttpGetAllResponseHeaders",http_all_headers},
    {"sceHttpGetLastErrno",http_last_errno}, {"sceHttpParseResponseHeader",http_parse_header},
    {"sceHttpSetAuthEnabled",ok}, {"sceHttpSetAutoRedirect",ok}, {"sceHttpSetRecvTimeOut",ok},
    {"sceHttpSetResolveRetry",ok}, {"sceHttpSetResolveTimeOut",ok}, {"sceHttpSetSendTimeOut",ok},
    {"sceHttpsGetSslError",https_ssl_error}, {"sceHttpsSetSslCallback",ok}, {"sceHttpUriEscape",http_uri_escape},
    /* Ssl */
    {"sceSslFreeSslCertName",ok}, {"sceSslGetIssuerName",ssl_no_cert}, {"sceSslGetMemoryPoolStats",ssl_pool_stats},
    {"sceSslGetNameEntryCount",ssl_no_cert}, {"sceSslGetNameEntryInfo",ssl_no_cert},
    {"sceSslGetNotAfter",ssl_no_cert}, {"sceSslGetNotBefore",ssl_no_cert},
    {"sceSslGetSerialNumber",ssl_no_cert}, {"sceSslGetSubjectName",ssl_no_cert},
    /* Net */
    {"sceNetGetMacAddress",net_mac_address}, {"sceNetGetpeername",net_not_connected},
    {"sceNetResolverAbort",ok}, {"sceNetResolverGetError",net_resolver_error},
    {"sceNetResolverStartNtoaMultipleRecords",net_unreachable},
    /* ShareUtility */
    {"sceShareUtilityInitialize",ok}, {"sceShareUtilityTerminate",ok},
    {"sceShareUtilityOpenShareMenu",share_open}, {"sceShareUtilityOpenShareMenuDefault",share_open},
};
uintptr_t runtime_net_offline_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
