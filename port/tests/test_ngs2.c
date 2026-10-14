/* Offline tests for the built-in NGS2 mixer (src/runtime_ngs2.c), driven through the same
 * exports the game imports, with synthetic waveforms: PCM sine at odd rates, hand-built
 * VAG/HEVAG frames, envelopes, loops, kick events, user FX, render order and two outputs.
 * Parameter blocks copied from the native trace (docs/NGS2_LEVANTAMENTO.md) check parsing. */
#define _GNU_SOURCE
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "runtime.h"

/* runtime_ngs2.c resolves through runtime_lookup; the tests call it by symbol instead. */
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *nid) { (void)table; (void)count; (void)nid; return 0; }

typedef struct { void *buffer; uint64_t size; uint32_t waveform, channels; } RenderBuffer;
typedef struct { void *host_buffer; uint64_t host_size; uintptr_t reserved[5]; uintptr_t user_data; } BufferInfo;
typedef struct { uint32_t data_offset, data_size, repeats, skip, samples, reserved; uint64_t user_data; } Block;
typedef struct { uint32_t curve, duration; float height; } EnvPoint;
typedef struct { float **channels; uintptr_t user0, user1, user2; uint32_t flags, num_channels, num_grain_samples, sample_rate; } UserFxContext;

static ABI int32_t (*system_create)(const void *, const void *, uintptr_t *);
static ABI int32_t (*system_render)(uintptr_t, const RenderBuffer *, uint32_t);
static ABI int32_t (*rack_create)(uintptr_t, uint32_t, const void *, const void *, uintptr_t *);
static ABI int32_t (*rack_create_alloc)(uintptr_t, uint32_t, const void *, const void *, uintptr_t *);
static ABI int32_t (*query_buffer)(uint32_t, const void *, BufferInfo *);
static ABI int32_t (*voice_handle)(uintptr_t, uint32_t, uintptr_t *);
static ABI int32_t (*voice_control)(uintptr_t, const void *);
static ABI int32_t (*voice_state)(uintptr_t, void *, uint64_t);
static ABI int32_t (*system_destroy)(uintptr_t, BufferInfo *);

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { ++failures; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define GRAIN 256
static float out8[GRAIN * 8];
static float out1[GRAIN];

/* ---- Parameter helpers ---- */
static void param(uintptr_t voice, uint32_t id, const void *body, uint16_t body_size) {
    uint8_t block[256] = {0};
    uint16_t size = (uint16_t)(8 + body_size);
    memcpy(block, &size, 2);
    memcpy(block + 4, &id, 4);
    memcpy(block + 8, body, body_size);
    CHECK(voice_control(voice, block) == 0, "VoiceControl 0x%08x", id);
}
static void u32s(uintptr_t voice, uint32_t id, uint32_t a, uint32_t b) { uint32_t v[2] = {a, b}; param(voice, id, v, 8); }
static void event(uintptr_t voice, uint32_t e) { param(voice, 6, &e, 4); }
static void patch(uintptr_t voice, uint32_t port, uintptr_t dest) {
    struct { uint32_t port, input; uintptr_t dest; } p = {port, 0, dest};
    param(voice, 5, &p, sizeof(p));
}
static void matrix(uintptr_t voice, uint32_t m, const float *levels, uint32_t count) {
    struct { uint32_t matrix, count; const float *levels; } p = {m, count, levels};
    param(voice, 1, &p, sizeof(p));
}
static void port_volume(uintptr_t voice, uint32_t port, float level) { struct { uint32_t port; float level; } p = {port, level}; param(voice, 2, &p, 8); }
static void setup_sampler(uintptr_t voice, uint32_t wave, uint32_t channels, uint32_t rate, uint32_t config) {
    uint32_t p[8] = {wave, channels, rate, config, 0, 0, 0, 0};
    param(voice, 0x10000000, p, 32);
}
static void add_blocks(uintptr_t voice, const void *data, const Block *blocks, uint32_t count) {
    struct { const void *data; uint32_t flags, count; const Block *blocks; } p = {data, 1, count, blocks};
    param(voice, 0x10000001, &p, sizeof(p));
}
static void pitch(uintptr_t voice, float ratio) { float p[2] = {ratio, 0}; param(voice, 0x10000005, p, 8); }
static void envelope(uintptr_t voice, const EnvPoint *points, uint32_t forward, uint32_t release) {
    struct { uint32_t forward, release; const EnvPoint *points; } p = {forward, release, points};
    param(voice, 0x10000006, &p, sizeof(p));
}
static uint32_t flags_of(uintptr_t voice) {
    uint8_t state[0x30];
    memset(state, 0xcc, sizeof(state));
    CHECK(voice_state(voice, state, 0x30) == 0, "GetState");
    uint32_t f;
    memcpy(&f, state, 4);
    return f;
}
static uint64_t decoded_of(uintptr_t voice) {
    uint8_t state[0x30];
    voice_state(voice, state, 0x30);
    uint64_t n;
    memcpy(&n, state + 0x10, 8);
    return n;
}

/* ---- Graph ---- */
typedef struct { uintptr_t system, sampler, submixer, mastering, master_voice, sub_voice; } Graph;
static Graph graph_new(uint32_t mastering_voices) {
    Graph g;
    uint8_t sys_option[0x40] = {0};
    uint64_t size = 0x40; uint32_t max_grain = 512, grain = GRAIN, rate = 48000;
    memcpy(sys_option, &size, 8); memcpy(sys_option + 0x1c, &max_grain, 4); memcpy(sys_option + 0x20, &grain, 4); memcpy(sys_option + 0x24, &rate, 4);
    CHECK(system_create(sys_option, NULL, &g.system) == 0, "system");
    uint8_t option[0xa8] = {0};
    uint32_t voices = 8, matrices = 1, ports = 8;
    memcpy(option + 0x20, &voices, 4); memcpy(option + 0x28, &matrices, 4); memcpy(option + 0x2c, &ports, 4);
    CHECK(rack_create(g.system, 0x1000, option, NULL, &g.sampler) == 0, "sampler rack");
    BufferInfo info;
    CHECK(query_buffer(0x2000, option, &info) == 0 && info.host_size > 0, "query");
    voices = 1;
    memcpy(option + 0x20, &voices, 4);
    CHECK(rack_create(g.system, 0x2000, option, &info, &g.submixer) == 0, "submixer rack");
    voices = mastering_voices;
    memcpy(option + 0x20, &voices, 4);
    CHECK(rack_create_alloc(g.system, 0x3000, option, NULL, &g.mastering) == 0, "mastering rack");
    CHECK(voice_handle(g.mastering, 0, &g.master_voice) == 0, "mastering voice");
    CHECK(g.master_voice == g.mastering, "voice 0 handle is the rack handle");
    CHECK(voice_handle(g.submixer, 0, &g.sub_voice) == 0, "submixer voice");
    u32s(g.sub_voice, 0x20000000, 8, 0);
    event(g.sub_voice, 0);
    patch(g.sub_voice, 0, g.mastering);
    u32s(g.master_voice, 0x30000000, 8, 0);
    struct { uint32_t enable; float threshold; } limiter = {1, 1.0f};
    param(g.master_voice, 0x30000003, &limiter, 8);
    float gain[2] = {1.0f, 1.0f};
    param(g.master_voice, 0x30000004, gain, 8);
    event(g.master_voice, 0);
    return g;
}
static void render(const Graph *g) {
    RenderBuffer b[2] = {{out8, sizeof(out8), 0x18, 8}, {out1, sizeof(out1), 0x18, 1}};
    CHECK(system_render(g->system, b, 2) == 0, "render");
}
/* Renders `count` grains, collecting channel `ch` of the main output. */
static void capture(const Graph *g, float *dst, uint32_t count, uint32_t ch) {
    for (uint32_t r = 0; r < count; ++r) {
        render(g);
        for (uint32_t i = 0; i < GRAIN; ++i) dst[r * GRAIN + i] = out8[i * 8 + ch];
    }
}
static double rms(const float *x, uint32_t n) { double s = 0; for (uint32_t i = 0; i < n; ++i) s += (double)x[i] * x[i]; return sqrt(s / n); }
static double frequency(const float *x, uint32_t n) {
    uint32_t crossings = 0, first = 0, last = 0;
    for (uint32_t i = 1; i < n; ++i)
        if (x[i - 1] < 0 && x[i] >= 0) { if (!crossings) first = i; last = i; ++crossings; }
    return crossings > 1 ? 48000.0 * (crossings - 1) / (last - first) : 0.0;
}
static uint32_t active_length(const float *x, uint32_t n) { uint32_t last = 0; for (uint32_t i = 0; i < n; ++i) if (fabsf(x[i]) > 1e-4f) last = i + 1; return last; }

static const float mono_front[8] = {0.5f, 0.5f, 0, 0, 0, 0, 0, 0};

/* PCM sine at 32 kHz through sampler -> submixer (patched to its rack handle) -> mastering. */
static void test_pcm_sine(void) {
    Graph g = graph_new(1);
    uintptr_t v;
    voice_handle(g.sampler, 1, &v);
    CHECK(flags_of(v) == 0, "fresh voice is free (0x%x)", flags_of(v));
    static int16_t pcm[32000];
    for (int i = 0; i < 32000; ++i) pcm[i] = (int16_t)lrint(0.5 * 32767 * sin(2 * M_PI * 1000.0 * i / 32000.0));
    Block b = {0, sizeof(pcm), 0, 0, 32000, 0, 0x1234};
    setup_sampler(v, 0x12, 1, 32000, 0);
    CHECK(flags_of(v) == 1, "set up voice is INUSE (0x%x)", flags_of(v));
    add_blocks(v, pcm, &b, 1);
    matrix(v, 0, mono_front, 8);
    u32s(v, 3, 0, 0);
    patch(v, 0, g.submixer);
    event(v, 0);
    CHECK(flags_of(v) == 3, "playing voice INUSE|PLAYING (0x%x)", flags_of(v));
    static float ch0[GRAIN * 200], ch1[GRAIN * 200], ch2[GRAIN * 200];
    for (uint32_t r = 0; r < 200; ++r) {
        render(&g);
        for (uint32_t i = 0; i < GRAIN; ++i) { ch0[r * GRAIN + i] = out8[i * 8]; ch1[r * GRAIN + i] = out8[i * 8 + 1]; ch2[r * GRAIN + i] = out8[i * 8 + 2]; }
        if (r == 100) CHECK(flags_of(v) == 3, "still playing at render 100");
    }
    double level = rms(ch0 + 4800, 38400), f = frequency(ch0 + 4800, 38400);
    printf("pcm sine 32k: rms %.4f (want %.4f) freq %.1f Hz, length %u samples, decoded %llu\n", level, 0.25 / sqrt(2), f,
           active_length(ch0, GRAIN * 200), (unsigned long long)decoded_of(v));
    CHECK(fabs(level - 0.25 / sqrt(2)) < 0.005, "level");
    CHECK(fabs(f - 1000.0) < 2.0, "frequency");
    CHECK(rms(ch1 + 4800, 38400) > 0.17, "matrix FR");
    CHECK(rms(ch2, GRAIN * 200) == 0.0, "matrix FC silent");
    uint32_t length = active_length(ch0, GRAIN * 200);
    CHECK(length > 47900 && length <= 48100, "1 s at 32 kHz lasts 48000 output samples (%u)", length);
    CHECK(flags_of(v) == 0, "finished voice drops INUSE (0x%x)", flags_of(v));
    CHECK(decoded_of(v) == 0 || decoded_of(v) == 32000, "decoded samples");
    system_destroy(g.system, NULL);
}

/* Pitch 2.0 on a 44.1 kHz source and a 96 kHz source at pitch 1. */
static void test_pitch_rates(void) {
    static const struct { uint32_t rate; float pitch; double expect_f; } cases[] = {{44100, 2.0f, 880.0}, {96000, 1.0f, 440.0}, {22050, 1.0f, 440.0}, {37800, 0.5f, 220.0}};
    for (unsigned c = 0; c < sizeof(cases) / sizeof(*cases); ++c) {
        Graph g = graph_new(1);
        uintptr_t v;
        voice_handle(g.sampler, 3, &v);
        uint32_t n = cases[c].rate;  /* one second of source */
        int16_t *pcm = malloc(n * 2);
        for (uint32_t i = 0; i < n; ++i) pcm[i] = (int16_t)lrint(0.5 * 32767 * sin(2 * M_PI * 440.0 * i / cases[c].rate));
        Block b = {0, n * 2, 0, 0, n, 0, 0};
        setup_sampler(v, 0x12, 1, cases[c].rate, 0);
        add_blocks(v, pcm, &b, 1);
        pitch(v, cases[c].pitch);
        matrix(v, 0, mono_front, 8);
        u32s(v, 3, 0, 0);
        patch(v, 0, g.sub_voice);
        event(v, 0);
        static float ch0[GRAIN * 450];
        capture(&g, ch0, 450, 0);
        double f = frequency(ch0 + 2400, 19200);
        uint32_t length = active_length(ch0, GRAIN * 450), want = (uint32_t)(48000.0 / cases[c].pitch);
        printf("rate %u pitch %.1f: %.1f Hz (want %.0f), length %u (want %u)\n", cases[c].rate, cases[c].pitch, f, cases[c].expect_f, length, want);
        CHECK(fabs(f - cases[c].expect_f) < 2.0, "pitch frequency");
        CHECK(length + 200 > want && length < want + 200, "pitch duration");
        free(pcm);
        system_destroy(g.system, NULL);
    }
}

/* VAG: frames with predictor 0 carry the nibbles directly (value = nibble * 4096 >> shift);
 * predictor 1 (0.9375) and an HEVAG index (> 4) exercise the history. Stereo is frame-interleaved. */
static void test_vag(void) {
    Graph g = graph_new(1);
    uintptr_t v;
    voice_handle(g.sampler, 2, &v);
    enum { FRAMES = 2000 };
    static uint8_t data[FRAMES * 32];
    int nibble_at = 0;
    for (int f = 0; f < FRAMES; ++f)
        for (int c = 0; c < 2; ++c) {
            uint8_t *frame = data + (f * 2 + c) * 16;
            frame[0] = 0x00 | 2;   /* predictor 0, shift 2 */
            frame[1] = 0;
            for (int i = 0; i < 28; ++i) {
                int k = nibble_at + i;
                int n = (int)lrint(7.0 * sin(2 * M_PI * 1500.0 * k / 48000.0)) * (c ? -1 : 1);
                uint8_t nib = (uint8_t)(n & 0xf);
                if (i & 1) frame[2 + i / 2] |= (uint8_t)(nib << 4); else frame[2 + i / 2] = nib;
            }
            if (c) nibble_at += 28;
        }
    uint32_t samples = FRAMES * 28;
    Block b = {0, sizeof(data), 0, 0, samples, 0, 0};
    setup_sampler(v, 0x1c, 2, 48000, 0);
    add_blocks(v, data, &b, 1);
    const float stereo[16] = {1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}; /* out-major: FL<-L, FR<-R */
    matrix(v, 0, stereo, 16);
    u32s(v, 3, 0, 0);
    patch(v, 0, g.submixer);
    event(v, 0);
    static float l[GRAIN * 240], r[GRAIN * 240];
    for (uint32_t k = 0; k < 240; ++k) {
        render(&g);
        for (uint32_t i = 0; i < GRAIN; ++i) { l[k * GRAIN + i] = out8[i * 8]; r[k * GRAIN + i] = out8[i * 8 + 1]; }
    }
    double fl = frequency(l + 1000, 40000), level = rms(l + 1000, 40000);
    double expect = 7.0 * 4096.0 / 4.0 / 32768.0 / sqrt(2.0);
    double corr = 0;
    for (int i = 1000; i < 41000; ++i) corr += (double)l[i] * r[i];
    printf("vag stereo: %.1f Hz rms %.4f (want ~%.4f), L/R correlation %.3f, length %u (want %u)\n", fl, level, expect, corr,
           active_length(l, GRAIN * 240), samples);
    CHECK(fabs(fl - 1500.0) < 3.0, "vag frequency");
    CHECK(fabs(level - expect) < 0.01, "vag level");
    CHECK(corr < 0, "stereo channels decoded separately (inverted R)");
    CHECK(flags_of(v) == 0, "vag voice finished");
    uint32_t length = active_length(l, GRAIN * 240);
    CHECK(length + 64 > samples && length < samples + 64, "vag duration");

    /* Predictor 1: one frame with an impulse then zeros decays by 0.9375 per sample. */
    uintptr_t w;
    voice_handle(g.sampler, 4, &w);
    static uint8_t impulse[16 * 4];
    memset(impulse, 0, sizeof(impulse));
    impulse[0] = 0x10 | 0;     /* predictor 1, shift 0 */
    impulse[2] = 0x04;         /* first sample 4 * 4096 */
    impulse[16] = 0x10; impulse[32] = 0x10; impulse[48] = 0x10;
    Block bi = {0, sizeof(impulse), 0, 0, 112, 0, 0};
    setup_sampler(w, 0x1c, 1, 48000, 0);
    add_blocks(w, impulse, &bi, 1);
    const float only_fc[8] = {0, 0, 1, 0, 0, 0, 0, 0};
    matrix(w, 0, only_fc, 8);
    u32s(w, 3, 0, 0);
    patch(w, 0, g.submixer);
    event(w, 0);
    render(&g);
    /* Past the 64-sample start fade: x[k] = 0.5 * 0.9375^k. */
    float x0 = out8[70 * 8 + 2], x10 = out8[80 * 8 + 2];
    printf("vag predictor 1: x70 %.6f (want %.6f) x80/x70 %.4f (want %.4f)\n", x0, 0.5 * pow(0.9375, 70), x10 / x0, pow(0.9375, 10));
    CHECK(fabs(x0 - 0.5 * pow(0.9375, 70)) < 0.0005, "impulse level");
    CHECK(fabs(x10 / x0 - pow(0.9375, 10)) < 0.02, "predictor decay");
    system_destroy(g.system, NULL);
}

/* Envelope: 100 ms linear attack, release 100 ms; the voice ends when the release does. */
static void test_envelope_and_events(void) {
    Graph g = graph_new(1);
    uintptr_t v;
    voice_handle(g.sampler, 0, &v);
    CHECK(v == g.sampler, "sampler voice 0 is the rack handle");
    voice_handle(g.sampler, 5, &v);
    static int16_t pcm[480];
    for (int i = 0; i < 480; ++i) pcm[i] = (int16_t)lrint(0.5 * 32767 * sin(2 * M_PI * 1000.0 * i / 48000.0));
    Block loop = {0, sizeof(pcm), UINT32_MAX, 0, 480, 0, 0};   /* endless loop */
    setup_sampler(v, 0x12, 1, 48000, 0);
    add_blocks(v, pcm, &loop, 1);
    EnvPoint points[3] = {{0, 4800, 1.0f}, {0, 480000, 1.0f}, {0, 4800, 0.0f}};
    envelope(v, points, 2, 1);
    matrix(v, 0, mono_front, 8);
    u32s(v, 3, 0, 0);
    patch(v, 0, g.submixer);
    event(v, 0);
    static float ch0[GRAIN * 200];
    capture(&g, ch0, 100, 0);
    double early = rms(ch0, 960), mid = rms(ch0 + 3840, 960), full = rms(ch0 + 9600, 4800);
    printf("envelope: attack rms %.4f -> %.4f -> %.4f\n", early, mid, full);
    CHECK(early < mid && mid < full && fabs(full - 0.25 / sqrt(2)) < 0.01, "attack ramp");
    CHECK(flags_of(v) == 3, "looping voice keeps playing");
    event(v, 1);   /* RELEASE */
    CHECK(flags_of(v) == 3, "releasing voice still in use");
    capture(&g, ch0, 30, 0);
    uint32_t tail = active_length(ch0, GRAIN * 30);
    printf("release tail %u samples (want ~4800), flags 0x%x\n", tail, flags_of(v));
    CHECK(tail > 4700 && tail < 4900, "release length");
    CHECK(flags_of(v) == 0, "voice ends after release");

    /* STOP is immediate, PAUSE/RESUME hold the position. */
    setup_sampler(v, 0x12, 1, 48000, 0);
    add_blocks(v, pcm, &loop, 1);
    envelope(v, NULL, 0, 0);
    event(v, 0);
    render(&g);
    event(v, 4);
    CHECK(flags_of(v) == 7, "paused flags (0x%x)", flags_of(v));
    render(&g);
    CHECK(out8[100 * 8] == 0.0f, "paused voice silent");
    event(v, 5);
    render(&g);
    CHECK(rms(out8, GRAIN * 8) > 0.01, "resumed");
    event(v, 2);
    render(&g);   /* STOP fades out over 64 samples in the next grain, then frees the voice */
    CHECK(flags_of(v) == 0, "STOP frees the voice");
    CHECK(out8[100 * 8] == 0.0f && out8[10 * 8] != 0.0f, "stop fade then silence");
    render(&g);
    CHECK(rms(out8, GRAIN * 8) == 0.0, "stopped voice silent");
    event(v, 0);
    CHECK(flags_of(v) == 0, "PLAY without setup does nothing");
    system_destroy(g.system, NULL);
}

/* Block repeats, skip samples and a two-block stream with a callback-free handoff. */
static void test_blocks(void) {
    Graph g = graph_new(1);
    uintptr_t v;
    voice_handle(g.sampler, 6, &v);
    static int16_t pcm[1100];
    for (int i = 0; i < 1100; ++i) pcm[i] = 16384;
    Block b[2] = {{0, sizeof(pcm), 2, 100, 1000, 0, 7}, {0, 400, 0, 0, 200, 0, 8}};
    setup_sampler(v, 0x12, 1, 48000, 0);
    add_blocks(v, pcm, b, 2);
    const float fc[8] = {0, 0, 1, 0, 0, 0, 0, 0};
    matrix(v, 0, fc, 8);
    u32s(v, 3, 0, 0);
    patch(v, 0, g.submixer);
    event(v, 0);
    static float ch[GRAIN * 20];
    capture(&g, ch, 20, 2);
    uint32_t length = active_length(ch, GRAIN * 20);
    printf("blocks: 3 x 1000 + 200 samples -> %u, decoded %llu\n", length, (unsigned long long)decoded_of(v));
    CHECK(length >= 3199 && length <= 3202, "repeats and second block");
    CHECK(flags_of(v) == 0, "queue drained ends the voice");
    system_destroy(g.system, NULL);
}

/* Submixer user FX (a host function standing in for the game's) and render order: B is
 * created before A but A feeds B, so A must run first or B would lag a grain. */
static int fx_calls;
static ABI int32_t half_fx(UserFxContext *c) {
    ++fx_calls;
    for (uint32_t ch = 0; ch < c->num_channels; ++ch) for (uint32_t i = 0; i < c->num_grain_samples; ++i) c->channels[ch][i] *= (float)c->user0 / 100.0f;
    return 0;
}
static void test_fx_and_order(void) {
    Graph g = graph_new(2);
    uint8_t option[0x98] = {0};
    uint32_t one = 1;
    memcpy(option + 0x20, &one, 4); memcpy(option + 0x28, &one, 4); memcpy(option + 0x2c, &one, 4);
    uintptr_t b, a;
    rack_create(g.system, 0x2000, option, NULL, &b);
    rack_create(g.system, 0x2000, option, NULL, &a);
    u32s(a, 0x20000000, 8, 0); event(a, 0);
    u32s(b, 0x20000000, 8, 0); event(b, 0);
    patch(g.sub_voice, 0, a);   /* sampler -> submixer -> A -> B -> mastering */
    patch(a, 0, b);
    patch(b, 0, g.mastering);
    struct { uintptr_t handler, u0, u1, u2; } fx = {(uintptr_t)half_fx, 50, 0, 0};
    param(b, 0x20000004, &fx, sizeof(fx));
    /* Second mastering voice -> render buffer 1 (mono), fed by a send from the submixer. */
    uintptr_t m1;
    voice_handle(g.mastering, 1, &m1);
    u32s(m1, 0x30000000, 1, 0);
    u32s(m1, 0x30000005, 1, 0);
    event(m1, 0);
    patch(g.sub_voice, 1, m1);
    port_volume(g.sub_voice, 1, 0.5f);
    uintptr_t v;
    voice_handle(g.sampler, 7, &v);
    static int16_t pcm[48000];
    for (int i = 0; i < 48000; ++i) pcm[i] = 16384;
    Block blk = {0, sizeof(pcm), 0, 0, 48000, 0, 0};
    setup_sampler(v, 0x12, 1, 48000, 0);
    add_blocks(v, pcm, &blk, 1);
    const float fl[8] = {1, 0, 0, 0, 0, 0, 0, 0};
    matrix(v, 0, fl, 8);
    u32s(v, 3, 0, 0);
    patch(v, 0, g.submixer);
    event(v, 0);
    render(&g);
    printf("fx/order: first grain out %.4f (want 0.25), fx calls %d, pad output %.4f (want 0.25)\n", out8[100 * 8], fx_calls, out1[100]);
    CHECK(fabsf(out8[100 * 8] - 0.25f) < 0.002f, "chain processed in dependency order with user FX");
    CHECK(fx_calls == 1, "user FX called once per render");
    CHECK(fabsf(out1[100] - 0.25f) < 0.002f, "second output buffer (mastering OUTPUT 1)");
    system_destroy(g.system, NULL);
}

/* Parameter blocks copied from the TLG trace (layouts the console module accepted). */
static void test_trace_blocks(void) {
    Graph g = graph_new(1);
    static const char *const blocks[] = {
        "28000000 04000020 00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000000",
        "28000000 02000020 00000000 3970b03c d0ec6f3f 00000000 4260e53b 4c37893d 0000803f 00000000",
        "30000000 06000020 00000000 00000000 06000000 00000000 00001643 08d6513f 8718363e 00000000 00000000 00000000",
        "10000000 05000010 a4cf753f 00000000",
        "30000000 0a000010 01000000 00000000 08000000 00000000 00002f44 5fce363f 0080993e 00000000 00000000 00000000",
        "88000000 001d0040 00000000 0000803f 0000803f cccc4c3e 0000fa44 08000000 0000803f 00004843 00000000 0000803f 00000000 0000803f 00000000 0000803f 00000000 0000803f 00000000 0000803f 00000000 0000803f 00000000 0000803f 00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000000 01000000",
        "60000000 01000120 0000803f 00000000 00000000 59ffffff 02000000 00004040 1f856b3e 50fbffff 0000003e dc050000 4c37893d 0000c842 0000be42 aee64046 00000000 0000803f 00000000 0000803f 00000000 0000803f 00000000 0000803f",
        "18000000 00000120 08000000 08000000 00000000 00000000",
        "10000000 99000010 00000000 00000000",
    };
    uintptr_t v;
    voice_handle(g.sampler, 1, &v);
    for (unsigned i = 0; i < sizeof(blocks) / sizeof(*blocks); ++i) {
        uint8_t raw[256];
        unsigned n = 0;
        for (const char *p = blocks[i]; *p && n < sizeof(raw);) {
            if (*p == ' ') { ++p; continue; }
            unsigned byte;
            sscanf(p, "%2x", &byte);
            raw[n++] = (uint8_t)byte;
            p += 2;
        }
        uint32_t id;
        memcpy(&id, raw + 4, 4);
        uintptr_t target = (id >> 28) == 1 ? v : g.sub_voice;
        CHECK(voice_control(target, raw) == 0, "trace block %u", i);
    }
    render(&g);
    system_destroy(g.system, NULL);
}

/* ATRAC9 setup with the trace's config word: a garbage block must end the voice, not hang. */
static void test_atrac9_setup(void) {
    Graph g = graph_new(1);
    uintptr_t v;
    voice_handle(g.sampler, 1, &v);
    static uint8_t junk[0x3000];
    for (unsigned i = 0; i < sizeof(junk); ++i) junk[i] = (uint8_t)(i * 37 + 11);
    Block b = {0, sizeof(junk), 0, 0x100, 0x1000, 0, 0};
    setup_sampler(v, 0x40, 4, 48000, 0xfe7a17f0);
    add_blocks(v, junk, &b, 1);
    patch(v, 0, g.submixer);
    event(v, 0);
    for (int i = 0; i < 200 && flags_of(v); ++i) render(&g);
    printf("atrac9 garbage block: flags 0x%x after decoding\n", flags_of(v));
    CHECK(flags_of(v) == 0, "atrac9 voice ends");
    system_destroy(g.system, NULL);
}


/* ---- Effects ---- */
/* One second of a float sine (amplitude `amp`) looping forever on sampler voice 1, front left. */
static float tone_pcm[48000];
static uintptr_t start_tone(const Graph *g, double freq, float amp, const float *levels, uintptr_t dest) {
    for (int i = 0; i < 48000; ++i) tone_pcm[i] = amp * (float)sin(2 * M_PI * freq * i / 48000.0);
    uintptr_t v;
    voice_handle(g->sampler, 1, &v);
    Block b = {0, sizeof(tone_pcm), UINT32_MAX, 0, 48000, 0, 0};
    setup_sampler(v, 0x18, 1, 48000, 0);
    add_blocks(v, tone_pcm, &b, 1);
    matrix(v, 0, levels, 8);
    u32s(v, 3, 0, 0);
    patch(v, 0, dest);
    event(v, 0);
    return v;
}
static double level_after(const Graph *g, uint32_t warm, uint32_t measure, uint32_t ch, float *peak) {
    static float buf[GRAIN * 400];
    capture(g, buf, warm, ch);
    capture(g, buf, measure, ch);
    if (peak) { *peak = 0; for (uint32_t i = 0; i < measure * GRAIN; ++i) if (fabsf(buf[i]) > *peak) *peak = fabsf(buf[i]); }
    return rms(buf, measure * GRAIN);
}
static void filter_param(uintptr_t voice, uint32_t id, uint32_t index, uint32_t type, float fc, float q, float level) {
    struct { uint32_t index, location, type, mask; float fc, q, level, r0, r1, r2; } f = {index, 0, type, 0, fc, q, level, 0, 0, 0};
    param(voice, id, &f, sizeof(f));
}
static const float front_left[8] = {1, 0, 0, 0, 0, 0, 0, 0};
static double db(double x) { return 20 * log10(x + 1e-12); }

static void test_filters(void) {
    static const struct { uint32_t type; float fc, q, level; double freq, want_db, tol; const char *what; } cases[] = {
        {1, 1000, 0.7071f, 1, 100, 0.0, 0.3, "LPF passband"}, {1, 1000, 0.7071f, 1, 8000, -37.8, 0.5, "LPF stopband"},
        {2, 1000, 0.7071f, 1, 8000, 0.0, 0.3, "HPF passband"}, {2, 1000, 0.7071f, 1, 125, -36.1, 1.5, "HPF stopband"},
        {8, 1000, 1.0f, 0.25f, 1000, -12.04, 0.3, "peaking EQ centre"}, {6, 150, 0.7071f, 0.178f, 20, -14.9, 0.8, "low shelf"},
        {7, 4000, 0.7071f, 0.5f, 18000, -6.0, 0.6, "high shelf"}, {5, 1000, 2.0f, 1, 1000, -40.0, 20.0, "notch"},
        {0, 1000, 0.5f, 1, 1000, 0.0, 0.01, "type 0 bypass"},
    };
    for (unsigned c = 0; c < sizeof(cases) / sizeof(*cases); ++c) {
        Graph g = graph_new(1);
        start_tone(&g, cases[c].freq, 0.5f, front_left, g.submixer);
        double ref = level_after(&g, 20, 100, 0, NULL);
        filter_param(g.sub_voice, 0x20000006, 0, cases[c].type, cases[c].fc, cases[c].q, cases[c].level);
        double got = db(level_after(&g, 40, 100, 0, NULL) / ref);
        printf("filter %-18s %6.0f Hz: %7.2f dB (want %6.2f)\n", cases[c].what, cases[c].freq, got, cases[c].want_db);
        CHECK(cases[c].type == 5 ? got < cases[c].want_db + cases[c].tol : fabs(got - cases[c].want_db) <= cases[c].tol, "filter %s", cases[c].what);
        system_destroy(g.system, NULL);
    }
    /* Sampler filter (0x1000000a) at location 0, and a per-port filter (location 2 = port 0). */
    Graph g = graph_new(1);
    uintptr_t v = start_tone(&g, 8000, 0.5f, front_left, g.submixer);
    double ref = level_after(&g, 20, 100, 0, NULL);
    filter_param(v, 0x1000000a, 1, 1, 1000, 0.7071f, 1);
    double got = db(level_after(&g, 40, 100, 0, NULL) / ref);
    struct { uint32_t index, location, type, mask; float fc, q, level, r0, r1, r2; } f = {1, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    param(v, 0x1000000a, &f, sizeof(f));
    f.type = 1; f.location = 2; f.fc = 1000; f.q = 0.7071f; f.level = 1;
    param(v, 0x1000000a, &f, sizeof(f));
    double got_port = db(level_after(&g, 40, 100, 0, NULL) / ref);
    printf("sampler filter: %.1f dB, port filter: %.1f dB (want ~-36)\n", got, got_port);
    CHECK(got < -34 && got_port < -34, "sampler and port filters");
    system_destroy(g.system, NULL);
}

static void test_dynamics(void) {
    /* Compressor: thr 0.1, slope 0.25, attack 1 ms, release 10 ms: a 0.8 peak settles at 0.275. */
    Graph g = graph_new(1);
    start_tone(&g, 200, 0.8f, front_left, g.submixer);
    struct { uint32_t flags; float thr, ratio, knee, attack, release, level; uint32_t reserved; } c = {0, 0.1f, 0.25f, 0, 0.001f, 0.01f, 1, 0};
    param(g.sub_voice, 0x20000002, &c, sizeof(c));
    float peak;
    level_after(&g, 40, 100, 0, &peak);
    printf("compressor: steady peak %.3f (want ~0.275)\n", peak);
    CHECK(peak > 0.25f && peak < 0.31f, "compressor curve");
    c.flags = 1;   /* bit 0 = bypass */
    param(g.sub_voice, 0x20000002, &c, sizeof(c));
    level_after(&g, 5, 100, 0, &peak);
    CHECK(fabsf(peak - 0.8f) < 0.01f, "compressor bypass (%.3f)", peak);
    system_destroy(g.system, NULL);

    /* Mastering limiter at 1.0 on a 2.0 sine: settles at the threshold. */
    g = graph_new(1);
    start_tone(&g, 200, 2.0f, front_left, g.submixer);
    static float buf[GRAIN * 200];
    capture(&g, buf, 200, 0);
    float early = 0, late = 0;
    for (int i = 0; i < GRAIN * 4; ++i) if (fabsf(buf[i]) > early) early = fabsf(buf[i]);
    for (int i = GRAIN * 20; i < GRAIN * 200; ++i) if (fabsf(buf[i]) > late) late = fabsf(buf[i]);
    /* The console limiter has no look-ahead: block (32-sample) peak detection with 1 ms attack
     * and 10 ms release smoothing lets peaks pass a few percent over the threshold. */
    double steady = rms(buf + GRAIN * 20, GRAIN * 180);
    printf("limiter: first 20 ms peak %.3f, steady peak %.3f, steady rms %.3f (threshold 1.0)\n", early, late, steady);
    CHECK(late <= 1.08f && late > 0.95f && fabs(steady - 1 / sqrt(2)) < 0.05, "limiter holds the threshold");
    system_destroy(g.system, NULL);

    /* Mastering GAIN (full band 0.5) and the LFE low-pass (120 Hz) on channel 3. */
    g = graph_new(1);
    const float lfe_only[8] = {0, 0, 0, 1, 0, 0, 0, 0};
    float gains[2] = {0.5f, 1.0f};
    param(g.master_voice, 0x30000004, gains, 8);
    start_tone(&g, 30, 0.5f, lfe_only, g.submixer);
    double lfe30 = level_after(&g, 40, 100, 3, NULL);
    start_tone(&g, 480, 0.5f, lfe_only, g.submixer);
    double lfe480 = level_after(&g, 40, 100, 3, NULL);
    start_tone(&g, 1000, 0.5f, front_left, g.submixer);
    double fl = level_after(&g, 40, 100, 0, NULL);
    printf("mastering: LFE 30 Hz %.2f dB, 480 Hz %.1f dB; FL with gain 0.5: %.2f dB\n", db(lfe30 / (0.5 / sqrt(2))),
           db(lfe480 / (0.5 / sqrt(2))), db(fl / (0.5 / sqrt(2))));
    CHECK(fabs(db(lfe30 / (0.5 / sqrt(2)))) < 0.6, "LFE passband");
    CHECK(db(lfe480 / (0.5 / sqrt(2))) < -40, "LFE stopband");
    CHECK(fabs(db(fl / (0.5 / sqrt(2))) + 6.02) < 0.1, "mastering gain");
    system_destroy(g.system, NULL);
}

static void i3dl2(uintptr_t voice, float wet, float dry, int32_t room, int32_t room_hf, uint32_t pattern, float decay, float hf_ratio,
                  int32_t reflections, float refl_delay, int32_t reverb, float rev_delay, float diffusion, float density, float hf_ref) {
    struct { float wet, dry; int32_t room, room_hf; uint32_t pattern; float decay, ratio; int32_t refl; float refl_delay; int32_t rev;
             float rev_delay, diffusion, density, hf_ref; uint32_t reserved[8]; } p = {wet, dry, room, room_hf, pattern, decay, hf_ratio,
             reflections, refl_delay, reverb, rev_delay, diffusion, density, hf_ref, {0}};
    param(voice, 0x20010001, &p, sizeof(p));
}
static void test_reverb(void) {
    for (int pass = 0; pass < 2; ++pass) {
        float decay = pass ? 2.5f : 1.0f;
        Graph g = graph_new(1);
        uint8_t option[0x88] = {0};
        uintptr_t rv;
        CHECK(rack_create(g.system, 0x2001, option, NULL, &rv) == 0, "reverb rack");
        u32s(rv, 0x20010000, 8, 8);
        i3dl2(rv, 1, 0, 0, 0, 0, decay, 1.0f, -10000, 0.01f, 0, 0.01f, 100, 100, 5000);
        event(rv, 0);
        patch(rv, 0, g.mastering);
        uintptr_t v;
        voice_handle(g.sampler, 2, &v);
        static float click[480];
        memset(click, 0, sizeof(click));
        click[100] = 1.0f;
        Block b = {0, sizeof(click), 0, 0, 480, 0, 0};
        setup_sampler(v, 0x18, 1, 48000, 0);
        add_blocks(v, click, &b, 1);
        matrix(v, 0, front_left, 8);
        u32s(v, 3, 0, 0);
        patch(v, 0, rv);
        event(v, 0);
        enum { RENDERS = 600 };
        static float ch[8][GRAIN * RENDERS];
        for (uint32_t r = 0; r < RENDERS; ++r) {
            render(&g);
            for (uint32_t i = 0; i < GRAIN; ++i) for (int c = 0; c < 8; ++c) ch[c][r * GRAIN + i] = out8[i * 8 + c];
        }
        /* Energy decay of FL between 0.2 s and 0.8 s * decay. */
        uint32_t w = 4800, a = (uint32_t)(0.2 * 48000 * decay), bb = (uint32_t)(0.8 * 48000 * decay);
        double ea = 0, eb = 0;
        for (uint32_t i = a; i < a + w; ++i) ea += ch[0][i] * ch[0][i];
        for (uint32_t i = bb; i < bb + w; ++i) eb += ch[0][i] * ch[0][i];
        double slope = 10 * log10(eb / ea) / ((double)(bb - a) / 48000.0), t60 = -60.0 / slope;
        double lfe = 0, fc = 0;
        for (uint32_t i = 0; i < GRAIN * RENDERS; ++i) { lfe += fabs(ch[3][i]); fc += fabs(ch[2][i]); }
        printf("reverb decayTime %.1f s: measured T60 %.2f s, LFE sum %.3g, FC sum %.3g\n", decay, t60, lfe, fc);
        CHECK(fabs(t60 - decay) < 0.25 * decay, "reverb decay time");
        CHECK(lfe == 0.0 && fc > 0.0, "reverb channels");
        system_destroy(g.system, NULL);
    }
}

static void test_custom(void) {
    Graph g = graph_new(1);
    uint8_t option[0x4d8] = {0};
    uintptr_t cr;
    CHECK(rack_create(g.system, 0x4002, option, NULL, &cr) == 0, "custom rack");
    u32s(cr, 0x40020000, 8, 8);
    event(cr, 0);
    patch(cr, 0, g.mastering);
    /* Delay: one tap at 100 ms, wet only. */
    struct { float dry, wet, gain, feedback, lowpass; uint32_t taps; float tap[16]; float in_gain[8]; uint32_t mask, flags; } d;
    memset(&d, 0, sizeof(d));
    d.dry = 0; d.wet = 1; d.gain = 1; d.feedback = 0; d.lowpass = 24000; d.taps = 1; d.tap[0] = 1.0f; d.tap[1] = 100.0f;
    _Static_assert(sizeof(d) == 128, "delay param body");
    param(cr, 0x40001d00, &d, sizeof(d));
    uintptr_t v;
    voice_handle(g.sampler, 2, &v);
    static float click[480];
    memset(click, 0, sizeof(click));
    click[100] = 1.0f;
    Block b = {0, sizeof(click), 0, 0, 480, 0, 0};
    setup_sampler(v, 0x18, 1, 48000, 0);
    add_blocks(v, click, &b, 1);
    matrix(v, 0, front_left, 8);
    u32s(v, 3, 0, 0);
    patch(v, 0, cr);
    event(v, 0);
    static float fl[GRAIN * 40];
    capture(&g, fl, 40, 0);
    uint32_t at = 0;
    for (uint32_t i = 0; i < GRAIN * 40; ++i) if (fabsf(fl[i]) > 0.5f) { at = i; break; }
    printf("delay: click at 100 -> %u (want %u)\n", at, 100 + 4800);
    CHECK(at == 4900, "delay tap time");

    /* Chorus: one phase, 1 ms, no depth, wet only: a pure delay of D + 1 samples. */
    struct { uint32_t pad, phases, mask; float gain, delay, rate, depth, feedback, wet, dry; } c = {0, 1, 0, 1, 1.0f, 1.0f, 0, 0, 1, 0};
    param(cr, 0x40001c00, &c, sizeof(c));
    d.dry = 1; d.wet = 0;   /* delay now passes its input through */
    param(cr, 0x40001d00, &d, sizeof(d));
    setup_sampler(v, 0x18, 1, 48000, 0);
    add_blocks(v, click, &b, 1);
    event(v, 0);
    capture(&g, fl, 4, 0);
    at = 0;
    for (uint32_t i = 0; i < GRAIN * 4; ++i) if (fabsf(fl[i]) > 0.5f) { at = i; break; }
    printf("chorus: click at 100 -> %u (want %u)\n", at, 100 + 49);
    CHECK(at == 149, "chorus delay");
    system_destroy(g.system, NULL);
}

static void test_envelope_curves(void) {
    /* Curve 3 (S-curve) is halfway at the middle of its duration; curve 1 starts slowly. */
    static const struct { uint32_t curve; double want; } cases[] = {{3, 0.5}, {1, (pow(10, 0.5) - 1) / 9}, {2, log10(5.5)}, {5, sin(M_PI / 4)}};
    for (unsigned k = 0; k < sizeof(cases) / sizeof(*cases); ++k) {
        Graph g = graph_new(1);
        uintptr_t v;
        voice_handle(g.sampler, 1, &v);
        static float dc[48000];
        for (int i = 0; i < 48000; ++i) dc[i] = 0.5f;
        Block b = {0, sizeof(dc), 0, 0, 48000, 0, 0};
        setup_sampler(v, 0x18, 1, 48000, 0);
        add_blocks(v, dc, &b, 1);
        EnvPoint points[2] = {{cases[k].curve, 9600, 1.0f}, {0, 480000, 1.0f}};
        envelope(v, points, 2, 0);
        matrix(v, 0, front_left, 8);
        u32s(v, 3, 0, 0);
        patch(v, 0, g.submixer);
        event(v, 0);
        static float buf[GRAIN * 40];
        capture(&g, buf, 40, 0);
        double mid = buf[4800] / 0.5;
        printf("envelope curve %u: %.3f at the middle (want %.3f)\n", cases[k].curve, mid, cases[k].want);
        CHECK(fabs(mid - cases[k].want) < 0.01, "envelope curve %u", cases[k].curve);
        system_destroy(g.system, NULL);
    }
}

/* Render cost of a TLG-sized graph: 40 submixers (2 filters + compressor), 6 I3DL2 reverbs,
 * 6 custom delays, 96 playing voices with 4 sends each and filters. */
static void test_perf(void) {
    Graph g = graph_new(1);
    uint8_t option[0x4d8] = {0};
    uint32_t voices = 256;
    memcpy(option + 0x20, &voices, 4);
    uintptr_t sampler;
    rack_create(g.system, 0x1000, option, NULL, &sampler);
    voices = 1;
    memcpy(option + 0x20, &voices, 4);
    uintptr_t subs[40], revs[6], dels[6];
    struct { uint32_t flags; float thr, ratio, knee, attack, release, level; uint32_t reserved; } comp = {0, 0.1f, 0.5f, 0, 0.01f, 0.1f, 1, 0};
    for (int i = 0; i < 40; ++i) {
        rack_create(g.system, 0x2000, option, NULL, &subs[i]);
        u32s(subs[i], 0x20000000, 8, 0); event(subs[i], 0);
        filter_param(subs[i], 0x20000006, 0, 6, 150, 0.8f, 0.2f);
        filter_param(subs[i], 0x20000006, 1, 8, 700, 1.5f, 0.8f);
        param(subs[i], 0x20000002, &comp, sizeof(comp));
        patch(subs[i], 0, i ? subs[i / 2] : g.mastering);
    }
    for (int i = 0; i < 6; ++i) {
        rack_create(g.system, 0x2001, option, NULL, &revs[i]);
        u32s(revs[i], 0x20010000, 8, 8);
        i3dl2(revs[i], 1, 0, 0, -167, 2, 3.0f, 0.23f, -1200, 0.125f, 1500, 0.0168f, 100, 95, 3086);
        event(revs[i], 0); patch(revs[i], 0, subs[0]);
        rack_create(g.system, 0x4002, option, NULL, &dels[i]);
        u32s(dels[i], 0x40020000, 8, 8); event(dels[i], 0); patch(dels[i], 0, revs[i]);
        struct { float dry, wet, gain, feedback, lowpass; uint32_t taps; float tap[16]; float in_gain[8]; uint32_t mask, flags; } d;
        memset(&d, 0, sizeof(d));
        d.wet = 1; d.gain = 1; d.feedback = 0.2f; d.lowpass = 2000; d.taps = 8; d.tap[0] = 1; d.tap[1] = 200; d.flags = 1;
        for (int t = 1; t < 8; ++t) d.tap[2 * t + 1] = 1;
        param(dels[i], 0x40001d00, &d, sizeof(d));
    }
    static float noise[48000];
    for (int i = 0; i < 48000; ++i) noise[i] = (float)((i * 7919 % 2001) - 1000) / 4000.0f;
    Block b = {0, sizeof(noise), UINT32_MAX, 0, 48000, 0, 0};
    const float levels[8] = {0.5f, 0.5f, 0.2f, 0.1f, 0.3f, 0.3f, 0.1f, 0.1f};
    for (uint32_t i = 0; i < 96; ++i) {
        uintptr_t v;
        voice_handle(sampler, i, &v);
        setup_sampler(v, 0x18, 1, 44100 + i * 37, 0);
        add_blocks(v, noise, &b, 1);
        matrix(v, 0, levels, 8);
        for (uint32_t p = 0; p < 4; ++p) { u32s(v, 3, p, 0); patch(v, p, p ? (p == 3 ? dels[i % 6] : revs[(i + p) % 6]) : subs[10 + i % 30]); }
        filter_param(v, 0x1000000a, 0, 8, 700, 0.7f, 0.3f);
        filter_param(v, 0x1000000a, 1, 7, 10000, 0.5f, 0.66f);
        pitch(v, 0.97f);
        event(v, 0);
    }
    for (int r = 0; r < 20; ++r) render(&g);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    enum { RENDERS = 400 };
    for (int r = 0; r < RENDERS; ++r) render(&g);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ms = ((double)(t1.tv_sec - t0.tv_sec) * 1e3 + (double)(t1.tv_nsec - t0.tv_nsec) / 1e6) / RENDERS;
    printf("perf: %.3f ms per 256-frame render (%.1f%% of real time, one core)\n", ms, ms / (256.0 / 48.0) * 100.0);
    CHECK(ms < 256.0 / 48.0 * 0.5, "render cost under half of real time");
    system_destroy(g.system, NULL);
}

int main(void) {
#define BIND(var, name) var = (__typeof__(var))runtime_ngs2_function(name); if (!var) { printf("missing %s\n", name); return 1; }
    BIND(system_create, "sceNgs2SystemCreateWithAllocator");
    BIND(system_render, "sceNgs2SystemRender");
    BIND(rack_create, "sceNgs2RackCreate");
    BIND(rack_create_alloc, "sceNgs2RackCreateWithAllocator");
    BIND(query_buffer, "sceNgs2RackQueryBufferSize");
    BIND(voice_handle, "sceNgs2RackGetVoiceHandle");
    BIND(voice_control, "sceNgs2VoiceControl");
    BIND(voice_state, "sceNgs2VoiceGetState");
    BIND(system_destroy, "sceNgs2SystemDestroy");
    test_pcm_sine();
    test_pitch_rates();
    test_vag();
    test_envelope_and_events();
    test_blocks();
    test_fx_and_order();
    test_trace_blocks();
    test_atrac9_setup();
    test_filters();
    test_dynamics();
    test_reverb();
    test_custom();
    test_envelope_curves();
    test_perf();
    printf(failures ? "ngs2-test: %d failures\n" : "ngs2-test: all passed\n", failures);
    return failures != 0;
}
