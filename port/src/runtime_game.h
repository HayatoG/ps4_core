/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * runtime_game.h: the per-game hook table. The core never names a game: runtime.c, probe.c and
 * the profile loader (runtime_profile.c) reach the game only through `runtime_game`.
 *
 * Each port defines exactly one `const RuntimeGame runtime_game` in port/src/game_<slug>.c, next
 * to its runtime_<slug>*.c. build.sh (and gpu/CMakeLists.txt on Windows) compiles:
 *   - PORT_GAME=<slug> (environment or CMake cache) -> src/game_<slug>.c;
 *   - otherwise PORT_SLUG from packaging/port.env, when src/game_<PORT_SLUG>.c exists;
 *   - otherwise the only src/game_*.c besides game_none.c;
 *   - otherwise src/game_none.c (all NULL: core-only build, unit tests, ps4_core CI).
 * Ready-made tables for the current games are in port/games/ (copy one into the port's src/).
 *
 * The core uses it, in this order:
 *   - probe.c main(): env_prefix (when the build has no -DPORT_ENV_PREFIX), then the profile
 *     (runtime_profile_load: core defaults < profile.ini < launcher/player < environment);
 *   - init() right after runtime_start(), before any import is resolved;
 *   - runtime_resolve(): exact libc/kernel glue in runtime.c, the core system modules, then
 *     resolvers[] in the game's order, then tlggpu_resolve();
 *   - patch_image() after the import relocations and the data patches, before the image is
 *     protected;
 *   - report() inside runtime_report(), among the core modules' lines.
 */
#ifndef RUNTIME_GAME_H
#define RUNTIME_GAME_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Resolve one scoped import ("NID#library"; libScePosix already folded into libkernel) to a host
 * function, or 0 to let the next resolver try. Same contract as runtime_<module>_resolve. */
typedef uintptr_t (*RuntimeGameResolver)(const char *scoped_nid);

/* What the loader knows about the executable when it patches it (probe.c, after mapping and
 * relocating, before the image is protected and the entry point runs). */
typedef struct RuntimeGameImage {
    const char *title_id;   /* "CUSA00223", from sce_sys/param.sfo ("" without --app0) */
    const char *app0;       /* the game folder (--app0), or NULL */
    unsigned char *base;    /* mapped image (eboot and linked modules) */
    uint64_t size;          /* image bytes */
} RuntimeGameImage;

typedef struct RuntimeGame {
    /* Identity. slug: "tlg", "gr2", "infamous" (file names, ~/.config/<slug>-launcher).
     * name: for the log ("inFAMOUS Second Son"). */
    const char *slug;
    const char *name;
    /* Title IDs this table serves, NULL-terminated ({"CUSA00223", NULL}). The loader warns when
     * the mounted game is not in the list (it still runs: regions share the code). NULL: any. */
    const char *const *title_ids;

    /* Environment prefix with the underscore, as PORT_ENV_PREFIX: "ISS_", "GR2_". Each <prefix>X
     * is copied to TLG_X at start unless TLG_X is set. NULL or "TLG_": no alias. A build with
     * -DPORT_ENV_PREFIX (packaging/port.env) overrides this. */
    const char *env_prefix;
    /* Game profile (defaults of layer 2) looked up in the working directory, then next to the
     * executable and one level up: "profile.ini". NULL: none (TLG_PROFILE still names one). */
    const char *profile;

    /* Game-specific system functions, tried in this order after the core modules and before the
     * GPU layer; NULL-terminated. NULL: none. */
    const RuntimeGameResolver *resolvers;

    /* Optional hooks; NULL when unused. */
    /* Right after runtime_start(), before the image is loaded. */
    void (*init)(void);
    /* Eboot patches in code (cheats, FPS hooks that need a trampoline). Data patches stay in
     * <slug>_patches.py / patches/<name>.xml. Returns the number of patches applied. */
    int (*patch_image)(const RuntimeGameImage *image);
    /* Extra lines for the end-of-run report. */
    void (*report)(void);
} RuntimeGame;

/* Defined by the port in port/src/game_<slug>.c (core-only builds: port/src/game_none.c). */
extern const RuntimeGame runtime_game;

#ifdef __cplusplus
}
#endif

#endif /* RUNTIME_GAME_H */
