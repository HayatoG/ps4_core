/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Game profile loader (layer 2 of the configuration): one generic reader, no game names. The
 * game's port ships port/profile.ini; the core reads it at the start of main and fills every
 * TLG_* variable the profile sets and the environment does not:
 *
 *     core defaults (compiled) < profile.ini < launcher / player < environment
 *
 * The launchers pass the player's choices as environment variables and tests set them by hand,
 * so "not already in the environment" is exactly "nobody above the profile chose". The GPU
 * settings (tlg.ini, the Insert menu) read the profile's [settings] section before tlg.ini
 * (gpu/shim/tlg_settings.cpp, through TLG_PROFILE_LOADED), so there too the player's file wins.
 *
 * File: lines `key = value`; `#` and `;` at the start of a line or after a blank start a
 * comment; `[section]` headers.
 *   [core] (or no section)  the keys of `keys[]` below (tsc, readbacks, ...);
 *   [env]                   NAME = value: a default for TLG_NAME (any core variable);
 *   [settings]              tlg.ini keys (mouse_sensitivity, smaa, ...): defaults under tlg.ini.
 * Where: TLG_PROFILE=<file> (or <PREFIX>PROFILE through the env alias; "0" or "none" disables);
 * otherwise runtime_game.profile ("profile.ini") in the working directory (run.sh runs from
 * port/), next to the executable, or one level above it (packages: bin/ or out/).
 */
#define _GNU_SOURCE
#include "runtime.h"
#include "runtime_game.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

typedef struct {
    const char *key;   /* profile key */
    const char *env;   /* TLG_<env> */
    int presence;      /* the core only checks that the variable exists: 0 means "leave unset" */
    const char *help;
} ProfileKey;
static const ProfileKey keys[] = {
    {"tsc", "TSC", 0, "host | process | auto (count rdtsc in the eboot)"},
    {"tsc_auto_threshold", "TSC_AUTO_THRESHOLD", 0, "rdtsc sites for tsc=auto to pick host (64)"},
    {"readbacks", "READBACKS", 0, "GPU readbacks: 0 off, 1 relaxed, 2 precise"},
    {"eager_readback", "EAGER_READBACK", 0, "0/1, precise readbacks only"},
    {"write_merge", "WRITE_MERGE", 0, "0/1, precise readbacks only"},
    {"gc_budget_mb", "GC_BUDGET_MB", 0, "texture cache budget in MB (0: driver's)"},
    {"block1d", "BLOCK1D", 0, "0/1: promote BC 1D textures to 2D"},
    {"skip_adjacency", "SKIP_ADJACENCY", 1, "0/1: drop draws with adjacency topology"},
    {"pipeline_binaries", "PIPELINE_BINARIES", 0, "0/1: VK_KHR_pipeline_binary cache"},
    {"mouse_buttons", "MOUSE_BUTTONS", 0, "default mouse buttons: Mouse Left=triangle,..."},
    {"motion_button", "MOTION_BUTTON", 0, "SDL button that toggles the sensor mode"},
    {"stick_deadzone", "STICK_DEADZONE", 0, "analog stick dead zone"},
};

static char loaded_path[1024];

static int env_set(const char *name, const char *value) {
#ifdef _WIN32
    return _putenv_s(name, value);
#else
    return setenv(name, value, 1);
#endif
}

static char *trim(char *text) {
    while (*text == ' ' || *text == '\t') ++text;
    char *end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1])) *--end = 0;
    return text;
}

/* Default for TLG_<env>: set when nothing above the profile chose. */
static void apply_default(const char *env, const char *value, int presence, const char *key) {
    char name[128];
    snprintf(name, sizeof(name), "TLG_%s", env);
    const char *current = getenv(name);
    if (current) {
        printf("Profile: %s=%s ignored, %s=%s set by the launcher or the environment\n", key, value, name, current);
        return;
    }
    if (presence && !strcmp(value, "0")) return;
    env_set(name, value);
    printf("Profile: %s=%s -> %s\n", key, value, name);
}

static int valid_value(const char *key, const char *value) {
    if (!strcmp(key, "tsc")) return !strcmp(value, "host") || !strcmp(value, "process") || !strcmp(value, "auto");
    return 1;
}

static int read_profile(const char *path) {
    FILE *file = fopen(path, "r");
    if (!file) return 0;
    snprintf(loaded_path, sizeof(loaded_path), "%s", path);
    env_set("TLG_PROFILE_LOADED", path); /* for the GPU settings ([settings], tlg_settings.cpp) */
    printf("Profile: %s (%s)\n", path, runtime_game.name ? runtime_game.name : "?");
    char line[1024], section[32] = "core";
    unsigned number = 0;
    while (fgets(line, sizeof(line), file)) {
        ++number;
        for (char *c = line; *c; ++c) /* comments: '#' or ';' at the start or after a blank */
            if ((*c == '#' || *c == ';') && (c == line || c[-1] == ' ' || c[-1] == '\t')) { *c = 0; break; }
        char *text = trim(line);
        if (!*text) continue;
        if (*text == '[') {
            char *close = strchr(text, ']');
            if (!close) { printf("Profile: %s:%u: bad section\n", path, number); continue; }
            *close = 0;
            snprintf(section, sizeof(section), "%s", trim(text + 1));
            continue;
        }
        char *equals = strchr(text, '=');
        if (!equals) { printf("Profile: %s:%u: expected key = value\n", path, number); continue; }
        *equals = 0;
        char *key = trim(text), *value = trim(equals + 1);
        if (!strcmp(section, "settings")) continue; /* gpu/shim/tlg_settings.cpp */
        if (!strcmp(section, "env")) {
            const char *bare = strncmp(key, "TLG_", 4) ? key : key + 4;
            if (*bare && strlen(bare) < 100) apply_default(bare, value, 0, key);
            continue;
        }
        if (strcmp(section, "core")) { printf("Profile: %s:%u: unknown section [%s]\n", path, number, section); continue; }
        const ProfileKey *known = NULL;
        for (size_t i = 0; i < sizeof(keys) / sizeof(*keys); ++i)
            if (!strcmp(key, keys[i].key)) known = &keys[i];
        if (!known) { printf("Profile: %s:%u: unknown key '%s'\n", path, number, key); continue; }
        if (!valid_value(key, value)) { printf("Profile: %s:%u: bad value %s=%s (%s)\n", path, number, key, value, known->help); continue; }
        apply_default(known->env, value, known->presence, key);
    }
    fclose(file);
    return 1;
}

/* Directory of the running executable (with the trailing separator), or "" when unknown. */
static void executable_dir(char *out, size_t size) {
    out[0] = 0;
#ifdef _WIN32
    DWORD n = GetModuleFileNameA(NULL, out, (DWORD)size);
    if (!n || n >= size) { out[0] = 0; return; }
#else
    ssize_t n = readlink("/proc/self/exe", out, size - 1);
    if (n <= 0) { out[0] = 0; return; }
    out[n] = 0;
#endif
    char *slash = strrchr(out, '/');
#ifdef _WIN32
    char *back = strrchr(out, '\\');
    if (!slash || (back && back > slash)) slash = back;
#endif
    if (slash) slash[1] = 0; else out[0] = 0;
}

void runtime_profile_load(void) {
    const char *explicit_path = getenv("TLG_PROFILE");
    if (explicit_path) {
        if (!*explicit_path || !strcmp(explicit_path, "0") || !strcmp(explicit_path, "none")) {
            puts("Profile: disabled (TLG_PROFILE)");
            return;
        }
        if (!read_profile(explicit_path)) printf("Profile: cannot open %s (TLG_PROFILE)\n", explicit_path);
        return;
    }
    const char *name = runtime_game.profile;
    if (!name || !*name) return;
    if (read_profile(name)) return;
    char dir[1024], path[2048];
    executable_dir(dir, sizeof(dir));
    if (!dir[0]) return;
    snprintf(path, sizeof(path), "%s%s", dir, name);
    if (read_profile(path)) return;
    snprintf(path, sizeof(path), "%s../%s", dir, name);
    read_profile(path);
}

const char *runtime_profile_path(void) { return loaded_path[0] ? loaded_path : NULL; }

/* rdtsc is 0F 31. A raw byte search also finds 0F 31 inside other instructions, mostly a 0F
 * (jump displacement, two-byte opcode tail) before `xor r32, r32` (31 C0..FF). Real rdtsc is
 * followed by an instruction that stores or shifts eax/edx (89 ..., 48 C1 E2 20, REX prefixes), so
 * a following register-register ModRM byte (C0..FF) is not counted unless it is a VEX prefix or a
 * jump/call opcode. Measured on TLG, GR2 and inFAMOUS: 2-4% over objdump's count of rdtsc,
 * against 20-80% for the raw count. */
size_t runtime_count_rdtsc(const unsigned char *code, size_t size, size_t *raw) {
    size_t sites = 0, all = 0;
    for (size_t i = 0; i + 1 < size; ++i) {
        if (code[i] != 0x0f || code[i + 1] != 0x31) continue;
        ++all;
        const unsigned next = i + 2 < size ? code[i + 2] : 0;
        if (next < 0xc0 || next == 0xc4 || next == 0xc5 || next == 0xe8 || next == 0xe9 || next == 0xeb) ++sites;
    }
    if (raw) *raw = all;
    return sites;
}

/* TLG_TSC=auto: a game that executes rdtsc itself must read the same clock as
 * sceKernelReadTsc, so many sites mean the host rdtsc (TLG, GR2 and inFAMOUS all have hundreds);
 * a game that only uses the kernel calls can take the process clock (steady on hosts whose TSC
 * differs between cores). */
void runtime_profile_tsc_decide(size_t sites, size_t raw, size_t text_bytes) {
    const char *mode = getenv("TLG_TSC");
    if (!mode || strcmp(mode, "auto")) return;
    const char *limit = getenv("TLG_TSC_AUTO_THRESHOLD");
    const size_t threshold = limit && *limit ? (size_t)strtoull(limit, NULL, 10) : 64;
    const int host = sites >= threshold;
    env_set("TLG_TSC", host ? "host" : "process");
    printf("TSC: auto: %zu rdtsc sites in the eboot code (%zu raw 0F 31 in %.1f MB), threshold %zu -> %s\n",
           sites, raw, (double)text_bytes / (1024.0 * 1024.0), threshold,
           host ? "host rdtsc (measured frequency)" : "process clock (1 GHz)");
}
