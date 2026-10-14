/* TLG_NGS2_TRACE=<file>: records how the game drives the firmware's libSceNgs2.sprx
 * (TLG_NGS2=native), as the reference for a from-scratch NGS2. The eboot's NGS2 imports are
 * bound to thin wrappers that call the native export and log:
 *   - every create/destroy with its option block (hex), rack ids and voice indexes;
 *   - every sceNgs2VoiceControl parameter (the header chain: u16 size, s16 next, u32 id), with
 *     call counts per rack type and up to EXAMPLES distinct blocks per (rack, id);
 *   - render buffer layouts and the VoiceGetState sizes.
 * The summary file is rewritten every few seconds. TLG_NGS2_TRACE_WAV=<file> also writes the
 * first render buffer (the mastered mix) as float WAV, to compare a reimplementation against. */
#include <inttypes.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "runtime.h"

#define MAX_KEYS 1024
#define MAX_VOICES 8192
#define EXAMPLES 4
#define EXAMPLE_BYTES 256
#define MAX_EVENTS 256
#define POINTEE_BYTES 192
#define MAX_LINKS 128

typedef ABI int32_t (*Fn3)(uintptr_t, uintptr_t, uintptr_t);
typedef ABI int32_t (*Fn5)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
typedef struct { uint16_t size; int16_t next; uint32_t id; } ParamHeader;
typedef struct { void *buffer; uint64_t size; uint32_t waveform, channels; } RenderBuffer;
typedef struct {
    uint64_t key, count, bytes_total;
    uint32_t min_size, max_size, examples;
    uint8_t example[EXAMPLES][EXAMPLE_BYTES];
    uint16_t example_size[EXAMPLES];
    uint8_t pointee[EXAMPLES][POINTEE_BYTES]; /* data a pointer field of the example points to */
    uint16_t pointee_size[EXAMPLES];
} ParamStat;

static atomic_flag lock = ATOMIC_FLAG_INIT;
static const char *trace_path;
static ParamStat stats[MAX_KEYS];
static struct { uintptr_t voice; uint32_t rack_id, index; } voices[MAX_VOICES];
static struct { uintptr_t rack; uint32_t rack_id; } racks[256];
static unsigned rack_count;
static char events[MAX_EVENTS][320];
static unsigned event_count;
static uint64_t calls[16], failures[16], renders, state_sizes[8][2], voice_misses;
static RenderBuffer first_render[8];
static uint32_t first_render_count;
static FILE *wav;
static uint64_t wav_frames;
static uintptr_t native[16];
static struct { uint32_t from, to, port; uint64_t count; } links[MAX_LINKS]; /* PATCH: rack -> rack */
static unsigned link_count;

enum { F_SYS_CREATE_ALLOC, F_SYS_DESTROY, F_RENDER, F_RACK_QUERY, F_RACK_CREATE, F_RACK_CREATE_ALLOC,
       F_RACK_DESTROY, F_VOICE_HANDLE, F_VOICE_CONTROL, F_VOICE_STATE, F_COUNT };
static const char *const names[F_COUNT]={
    "sceNgs2SystemCreateWithAllocator","sceNgs2SystemDestroy","sceNgs2SystemRender",
    "sceNgs2RackQueryBufferSize","sceNgs2RackCreate","sceNgs2RackCreateWithAllocator",
    "sceNgs2RackDestroy","sceNgs2RackGetVoiceHandle","sceNgs2VoiceControl","sceNgs2VoiceGetState",
};

static void acquire(void) { while (atomic_flag_test_and_set_explicit(&lock, memory_order_acquire)) {} }
static void release(void) { atomic_flag_clear_explicit(&lock, memory_order_release); }

static void hex(char *out, size_t room, const void *data, size_t size) {
    const uint8_t *p = data;
    size_t used = 0;
    out[0] = 0;
    for (size_t i = 0; i < size && used + 4 < room; ++i)
        used += (size_t)snprintf(out + used, room - used, "%02x%s", p[i], (i & 3) == 3 ? " " : "");
}
static void event(const char *format, ...) __attribute__((format(printf, 1, 2)));
static void event(const char *format, ...) {
    if (event_count >= MAX_EVENTS) return;
    va_list args;
    va_start(args, format);
    vsnprintf(events[event_count++], sizeof(*events), format, args);
    va_end(args);
}

static uint32_t rack_of(uintptr_t rack) {
    for (unsigned i = 0; i < rack_count; ++i) if (racks[i].rack == rack) return racks[i].rack_id;
    return 0xffffffffu;
}
static unsigned voice_slot(uintptr_t voice) { return (unsigned)((voice >> 4) * 2654435761u) % MAX_VOICES; }
static uint32_t voice_rack(uintptr_t voice) {
    for (unsigned i = voice_slot(voice), n = 0; n < MAX_VOICES; i = (i + 1) % MAX_VOICES, ++n) {
        if (voices[i].voice == voice) return voices[i].rack_id;
        if (!voices[i].voice) break;
    }
    ++voice_misses;
    return 0xffffffffu;
}
static void remember_voice(uintptr_t voice, uint32_t rack_id, uint32_t index) {
    for (unsigned i = voice_slot(voice), n = 0; n < MAX_VOICES; i = (i + 1) % MAX_VOICES, ++n)
        if (!voices[i].voice || voices[i].voice == voice) { voices[i].voice = voice; voices[i].rack_id = rack_id; voices[i].index = index; return; }
}

static ParamStat *stat_for(uint64_t key) {
    for (unsigned i = (unsigned)((key * 0x9e3779b97f4a7c15ull) >> 54) % MAX_KEYS, n = 0; n < MAX_KEYS; i = (i + 1) % MAX_KEYS, ++n) {
        if (stats[i].count && stats[i].key == key) return &stats[i];
        if (!stats[i].count) { stats[i].key = key; stats[i].min_size = UINT32_MAX; return &stats[i]; }
    }
    return NULL;
}
static void record_param(uint32_t rack_id, const ParamHeader *param) {
    ParamStat *s = stat_for(((uint64_t)rack_id << 32) | param->id);
    if (!s) return;
    ++s->count;
    s->bytes_total += param->size;
    if (param->size < s->min_size) s->min_size = param->size;
    if (param->size > s->max_size) s->max_size = param->size;
    size_t size = param->size < EXAMPLE_BYTES ? param->size : EXAMPLE_BYTES;
    for (uint32_t e = 0; e < s->examples; ++e)
        if (s->example_size[e] == size && !memcmp(s->example[e], param, size)) return;
    if (s->examples < EXAMPLES) {
        uint32_t e = s->examples++;
        memcpy(s->example[e], param, size);
        s->example_size[e] = (uint16_t)size;
        /* Pointer fields: (offset of the pointer, offset of a u32 count or 0, bytes per item). */
        const uint8_t *base = (const uint8_t *)param;
        uint32_t at = 0, count_at = 0, item = 0;
        switch (param->id) {
        case 0x00000001: at = 16; count_at = 12; item = 4; break;  /* matrix levels */
        case 0x10000001: at = 24; count_at = 16; item = 32; break; /* waveform blocks */
        case 0x10000006: at = 16; count_at = 8; item = 16; break;  /* envelope points */
        }
        if (at && at + 8 <= param->size) {
            uintptr_t pointer;
            memcpy(&pointer, base + at, 8);
            uint32_t count = 1;
            if (count_at) memcpy(&count, base + count_at, 4);
            size_t bytes = (size_t)count * item;
            if (bytes > POINTEE_BYTES) bytes = POINTEE_BYTES;
            if (pointer > 0x10000 && bytes) { memcpy(s->pointee[e], (const void *)pointer, bytes); s->pointee_size[e] = (uint16_t)bytes; }
        }
    }
}
static void record_link(uint32_t from, const ParamHeader *param) {
    if (param->id != 5 || param->size < 24) return;
    uint32_t port;
    uintptr_t dest;
    memcpy(&port, (const uint8_t *)param + 8, 4);
    memcpy(&dest, (const uint8_t *)param + 16, 8);
    uint32_t to = dest ? voice_rack(dest) : 0xfffffffeu;
    for (unsigned i = 0; i < link_count; ++i)
        if (links[i].from == from && links[i].to == to && links[i].port == port) { ++links[i].count; return; }
    if (link_count < MAX_LINKS) {
        links[link_count].from = from; links[link_count].to = to; links[link_count].port = port;
        links[link_count++].count = 1;
    }
}

static int compare_keys(const void *a, const void *b) {
    uint64_t x = (*(ParamStat *const *)a)->key, y = (*(ParamStat *const *)b)->key;
    return x < y ? -1 : x > y;
}
static void write_summary(void) {
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s.tmp", trace_path);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "# NGS2 trace (native libSceNgs2.sprx)\n\n## Calls\n");
    for (int i = 0; i < F_COUNT; ++i)
        fprintf(f, "%-34s %10" PRIu64 " calls, %" PRIu64 " errors\n", names[i], calls[i], failures[i]);
    fprintf(f, "voice handles not seen in GetVoiceHandle: %" PRIu64 "\n", voice_misses);
    fprintf(f, "\n## VoiceGetState sizes\n");
    for (int i = 0; i < 8; ++i) if (state_sizes[i][1]) fprintf(f, "size 0x%" PRIx64 ": %" PRIu64 " calls\n", state_sizes[i][0], state_sizes[i][1]);
    fprintf(f, "\n## Render buffers (first render)\n");
    for (uint32_t i = 0; i < first_render_count; ++i)
        fprintf(f, "[%u] size 0x%" PRIx64 " waveform 0x%x channels %u\n", i, first_render[i].size, first_render[i].waveform, first_render[i].channels);
    fprintf(f, "renders: %" PRIu64 "\n", renders);
    fprintf(f, "\n## Racks\n");
    for (unsigned i = 0; i < rack_count; ++i) fprintf(f, "rack %#" PRIxPTR " id 0x%04x\n", racks[i].rack, racks[i].rack_id);
    fprintf(f, "\n## Patches (rack id -> rack id; fffffffe = unpatch)\n");
    for (unsigned i = 0; i < link_count; ++i)
        fprintf(f, "0x%04x port %u -> 0x%04x: %" PRIu64 "\n", links[i].from, links[i].port, links[i].to, links[i].count);
    fprintf(f, "\n## Events\n");
    for (unsigned i = 0; i < event_count; ++i) fprintf(f, "%s\n", events[i]);
    ParamStat *sorted[MAX_KEYS];
    unsigned n = 0;
    for (unsigned i = 0; i < MAX_KEYS; ++i) if (stats[i].count) sorted[n++] = &stats[i];
    qsort(sorted, n, sizeof(*sorted), compare_keys);
    fprintf(f, "\n## VoiceControl params (rack id, param id)\n");
    for (unsigned i = 0; i < n; ++i) {
        ParamStat *s = sorted[i];
        fprintf(f, "\nrack 0x%04x param 0x%08x: %" PRIu64 " uses, size %u..%u\n",
                (uint32_t)(s->key >> 32), (uint32_t)s->key, s->count, s->min_size, s->max_size);
        for (uint32_t e = 0; e < s->examples; ++e) {
            char line[EXAMPLE_BYTES * 3 + 64];
            hex(line, sizeof(line), s->example[e], s->example_size[e]);
            fprintf(f, "  %s\n", line);
            if (s->pointee_size[e]) {
                hex(line, sizeof(line), s->pointee[e], s->pointee_size[e]);
                fprintf(f, "    -> %s\n", line);
            }
        }
    }
    fclose(f);
    rename(tmp, trace_path);
}

static void wav_header(FILE *f, uint32_t channels, uint64_t frames) {
    uint32_t data = (uint32_t)(frames * channels * 4), riff = 36 + data, fmt_size = 16, rate = 48000;
    uint32_t byte_rate = rate * channels * 4;
    uint16_t format = 3, ch = (uint16_t)channels, align = (uint16_t)(channels * 4), bits = 32;
    fseek(f, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
    fwrite(&fmt_size, 4, 1, f); fwrite(&format, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&rate, 4, 1, f);
    fwrite(&byte_rate, 4, 1, f); fwrite(&align, 2, 1, f); fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
    fseek(f, 0, SEEK_END);
}

static void finish(int index, int32_t result) {
    ++calls[index];
    if (result) {
        ++failures[index];
        event("%s -> 0x%08x", names[index], (uint32_t)result);
    }
}

static ABI int32_t t_sys_create(uintptr_t option, uintptr_t allocator, uintptr_t out) {
    int32_t r = ((Fn3)native[F_SYS_CREATE_ALLOC])(option, allocator, out);
    acquire();
    char line[200] = "";
    if (option) hex(line, sizeof(line), (const void *)option, 0x40);
    event("SystemCreateWithAllocator option[0x40]=%s -> %#" PRIxPTR, line, out && !r ? *(uintptr_t *)out : 0);
    finish(F_SYS_CREATE_ALLOC, r);
    release();
    return r;
}
static ABI int32_t t_sys_destroy(uintptr_t system, uintptr_t info, uintptr_t c) {
    int32_t r = ((Fn3)native[F_SYS_DESTROY])(system, info, c);
    acquire(); event("SystemDestroy %#" PRIxPTR, system); finish(F_SYS_DESTROY, r); write_summary(); release();
    return r;
}
static ABI int32_t t_render(uintptr_t system, uintptr_t buffers, uintptr_t count) {
    int32_t r = ((Fn3)native[F_RENDER])(system, buffers, count);
    acquire();
    const RenderBuffer *b = (const RenderBuffer *)buffers;
    if (!renders && b) {
        first_render_count = (uint32_t)count < 8 ? (uint32_t)count : 8;
        memcpy(first_render, b, first_render_count * sizeof(*b));
    }
    ++renders;
    if (wav && b && count && b[0].waveform == 0x18 && b[0].channels) { /* PCM_F32L */
        uint64_t frames = b[0].size / (4u * b[0].channels);
        fwrite(b[0].buffer, 4u * b[0].channels, frames, wav);
        wav_frames += frames;
        if (!(renders % 1000)) { wav_header(wav, b[0].channels, wav_frames); fflush(wav); }
    }
    finish(F_RENDER, r);
    if (!(renders % 1000)) write_summary();
    release();
    return r;
}
static ABI int32_t t_rack_query(uintptr_t rack_id, uintptr_t option, uintptr_t info) {
    int32_t r = ((Fn3)native[F_RACK_QUERY])(rack_id, option, info);
    acquire();
    char line[400] = "";
    if (option) hex(line, sizeof(line), (const void *)option, 0x60);
    event("RackQueryBufferSize id 0x%04x option[0x60]=%s -> size 0x%" PRIx64, (uint32_t)rack_id, line,
          info && !r ? ((uint64_t *)info)[1] : 0);
    finish(F_RACK_QUERY, r);
    release();
    return r;
}
static void rack_created(const char *what, uint32_t rack_id, uintptr_t option, uintptr_t out, int32_t r) {
    char line[400] = "";
    if (option) hex(line, sizeof(line), (const void *)option, 0x60);
    uintptr_t rack = out && !r ? *(uintptr_t *)out : 0;
    event("%s id 0x%04x option[0x60]=%s -> %#" PRIxPTR, what, rack_id, line, rack);
    if (rack && rack_count < 256) { racks[rack_count].rack = rack; racks[rack_count++].rack_id = rack_id; }
}
static ABI int32_t t_rack_create(uintptr_t system, uintptr_t rack_id, uintptr_t option, uintptr_t buffer, uintptr_t out) {
    int32_t r = ((Fn5)native[F_RACK_CREATE])(system, rack_id, option, buffer, out);
    acquire(); rack_created("RackCreate", (uint32_t)rack_id, option, out, r); finish(F_RACK_CREATE, r); release();
    return r;
}
static ABI int32_t t_rack_create_alloc(uintptr_t system, uintptr_t rack_id, uintptr_t option, uintptr_t allocator, uintptr_t out) {
    int32_t r = ((Fn5)native[F_RACK_CREATE_ALLOC])(system, rack_id, option, allocator, out);
    acquire(); rack_created("RackCreateWithAllocator", (uint32_t)rack_id, option, out, r); finish(F_RACK_CREATE_ALLOC, r); release();
    return r;
}
static ABI int32_t t_rack_destroy(uintptr_t rack, uintptr_t info, uintptr_t c) {
    int32_t r = ((Fn3)native[F_RACK_DESTROY])(rack, info, c);
    acquire(); event("RackDestroy id 0x%04x", rack_of(rack)); finish(F_RACK_DESTROY, r); release();
    return r;
}
static ABI int32_t t_voice_handle(uintptr_t rack, uintptr_t index, uintptr_t out) {
    int32_t r = ((Fn3)native[F_VOICE_HANDLE])(rack, index, out);
    acquire();
    if (!r && out) remember_voice(*(uintptr_t *)out, rack_of(rack), (uint32_t)index);
    finish(F_VOICE_HANDLE, r);
    release();
    return r;
}
static ABI int32_t t_voice_control(uintptr_t voice, uintptr_t params, uintptr_t c) {
    acquire();
    uint32_t rack_id = voice_rack(voice);
    const uint8_t *p = (const uint8_t *)params;
    for (int guard = 0; p && guard < 256; ++guard) {
        const ParamHeader *h = (const ParamHeader *)p;
        record_param(rack_id, h);
        record_link(rack_id, h);
        if (!h->next) break;
        p += h->next;
    }
    release();
    int32_t r = ((Fn3)native[F_VOICE_CONTROL])(voice, params, c);
    acquire(); finish(F_VOICE_CONTROL, r); release();
    return r;
}
static ABI int32_t t_voice_state(uintptr_t voice, uintptr_t state, uintptr_t size) {
    int32_t r = ((Fn3)native[F_VOICE_STATE])(voice, state, size);
    acquire();
    for (int i = 0; i < 8; ++i)
        if (!state_sizes[i][1] || state_sizes[i][0] == size) { state_sizes[i][0] = size; ++state_sizes[i][1]; break; }
    finish(F_VOICE_STATE, r);
    release();
    return r;
}
static void *const wrappers[F_COUNT]={
    (void *)t_sys_create, (void *)t_sys_destroy, (void *)t_render, (void *)t_rack_query, (void *)t_rack_create,
    (void *)t_rack_create_alloc, (void *)t_rack_destroy, (void *)t_voice_handle, (void *)t_voice_control, (void *)t_voice_state,
};

/* Called for each eboot import bound to a linked module's export; returns the address to bind. */
uintptr_t runtime_ngs2_trace_bind(const char *nid, uintptr_t address) {
    static int enabled = -1;
    if (enabled < 0) {
        trace_path = getenv("TLG_NGS2_TRACE");
        enabled = trace_path && *trace_path;
        const char *wav_path = getenv("TLG_NGS2_TRACE_WAV");
        if (enabled && wav_path && *wav_path && (wav = fopen(wav_path, "wb"))) wav_header(wav, 8, 0);
    }
    const char *symbol = enabled ? runtime_symbol(nid) : NULL;
    if (!symbol) return address;
    for (int i = 0; i < F_COUNT; ++i)
        if (!strcmp(symbol, names[i])) {
            native[i] = address;
            printf("NGS2 trace: %s wrapped\n", symbol);
            return (uintptr_t)wrappers[i];
        }
    if (!strncmp(symbol, "sceNgs2", 7)) printf("NGS2 trace: %s not traced\n", symbol);
    return address;
}
