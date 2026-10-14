/* Game profile loader (src/runtime_profile.c): precedence and the TLG_TSC=auto rdtsc count. */
#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "runtime.h"

static void write_file(const char *path, const char *text) {
    FILE *f = fopen(path, "w");
    assert(f);
    fputs(text, f);
    fclose(f);
}
static int env_is(const char *name, const char *value) {
    const char *v = getenv(name);
    return value ? v && !strcmp(v, value) : !v;
}

int main(void) {
    char dir[] = "/tmp/profile-test-XXXXXX";
    assert(mkdtemp(dir));
    char path[256];
    snprintf(path, sizeof(path), "%s/profile.ini", dir);
    write_file(path,
        "# game defaults\n"
        "tsc = auto\n"
        "readbacks = 2   ; precise\n"
        "eager_readback=1\n"
        "write_merge = 1\n"
        "gc_budget_mb = 3500\n"
        "block1d = 0\n"
        "skip_adjacency = 0\n"
        "mouse_buttons = Mouse Left=square,Mouse Right=l2\n"
        "bogus = 1\n"
        "[env]\n"
        "PIPELINE_BINARIES = 0\n"
        "KEYS = cross=;,square=# ; a comment\n"
        "TLG_DRAW_PIPE = 1\n"
        "[settings]\n"
        "mouse_sensitivity = 2.5\n");
    /* The launcher (environment) already chose these: the profile must not override them. */
    setenv("TLG_WRITE_MERGE", "0", 1);
    setenv("TLG_DRAW_PIPE", "0", 1);
    unsetenv("TLG_READBACKS"); unsetenv("TLG_TSC"); unsetenv("TLG_SKIP_ADJACENCY"); unsetenv("TLG_MOUSE_SENSITIVITY");
    setenv("TLG_PROFILE", path, 1);
    runtime_profile_load();
    assert(runtime_profile_path() && !strcmp(runtime_profile_path(), path) && env_is("TLG_PROFILE_LOADED", path));
    assert(env_is("TLG_TSC", "auto") && env_is("TLG_READBACKS", "2") && env_is("TLG_EAGER_READBACK", "1"));
    assert(env_is("TLG_WRITE_MERGE", "0") && env_is("TLG_DRAW_PIPE", "0"));  /* environment wins */
    assert(env_is("TLG_GC_BUDGET_MB", "3500") && env_is("TLG_BLOCK1D", "0"));
    assert(env_is("TLG_SKIP_ADJACENCY", NULL));  /* presence flag: 0 leaves it unset */
    assert(env_is("TLG_MOUSE_BUTTONS", "Mouse Left=square,Mouse Right=l2"));
    assert(env_is("TLG_PIPELINE_BINARIES", "0") && env_is("TLG_KEYS", "cross=;,square=#"));
    assert(env_is("TLG_BOGUS", NULL) && env_is("TLG_MOUSE_SENSITIVITY", NULL)); /* [settings]: GPU side */

    /* rdtsc count: 0F 31 then a store of eax counts; 0F 31 C0 (… xor eax,eax) does not. */
    const unsigned char code[] = {
        0x0f, 0x31, 0x89, 0x01,                   /* rdtsc; mov [rcx],eax */
        0x0f, 0x31, 0x48, 0xc1, 0xe2, 0x20,       /* rdtsc; shl rdx,32 */
        0x74, 0x0f, 0x31, 0xc0,                   /* je ...; xor eax,eax (false hit) */
        0x0f, 0x31, 0xeb, 0x00,                   /* rdtsc; jmp */
        0x0f, 0x31,                               /* rdtsc at the end */
    };
    size_t raw = 0;
    assert(runtime_count_rdtsc(code, sizeof(code), &raw) == 4 && raw == 5);
    assert(runtime_count_rdtsc(code, 1, &raw) == 0 && raw == 0);
    /* tsc=auto: many sites -> host; few -> process; an explicit mode is left alone. */
    runtime_profile_tsc_decide(391, 505, 13391288);
    assert(env_is("TLG_TSC", "host"));
    setenv("TLG_TSC", "auto", 1);
    runtime_profile_tsc_decide(3, 20, 1 << 20);
    assert(env_is("TLG_TSC", "process"));
    setenv("TLG_TSC", "auto", 1); setenv("TLG_TSC_AUTO_THRESHOLD", "2", 1);
    runtime_profile_tsc_decide(3, 20, 1 << 20);
    assert(env_is("TLG_TSC", "host"));
    setenv("TLG_TSC", "process", 1);
    runtime_profile_tsc_decide(1000, 1000, 1 << 20);
    assert(env_is("TLG_TSC", "process"));

    /* "none" disables the profile. */
    unsetenv("TLG_GC_BUDGET_MB");
    setenv("TLG_PROFILE", "none", 1);
    runtime_profile_load();
    assert(env_is("TLG_GC_BUDGET_MB", NULL));
    unlink(path);
    rmdir(dir);
    puts("PASS: game profile");
    return 0;
}
