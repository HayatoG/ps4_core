/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * runtime_game.h: the per-game hook table (DRAFT, NOT WIRED).
 *
 * Nothing includes this header yet and nothing in the core calls through it. It records the
 * design so the next ports can stop editing runtime.c/probe.c. See README.md ("Como um port vai
 * usar o núcleo" / "How a port will consume the core") and docs/KNOWN_ISSUES.md.
 *
 * Today (what this replaces):
 *   - runtime.c runtime_resolve() calls runtime_gr2_audio3d_resolve, runtime_tlg_resolve,
 *     runtime_gr2_resolve, runtime_gr2_np_resolve, runtime_gr2_font_resolve,
 *     runtime_gr2_json_resolve and runtime_infamous_resolve by name, so every port has to carry
 *     every game's runtime_<slug>*.c (gr2_pc carries runtime_tlg.c; infamous_pc carries all of
 *     them) and a new game edits runtime.c and runtime.h;
 *   - runtime_report() calls runtime_gr2_audio3d_report();
 *   - probe.c installs God of War III (CUSA01623 v01.02) cheats and stat hooks by checking the
 *     title ID inline (install_cheats, install_stat_multipliers; cheat/damage/actor/orb_hook.h);
 *   - every game reads its settings through TLG_* environment names.
 *
 * Plan:
 *   - the core defines the struct below and calls it; each port defines exactly one
 *     `const RuntimeGame runtime_game` in port/src/game_<slug>.c, next to its runtime_<slug>*.c;
 *   - a core-only build (tests, CI of ps4_core) links port/src/game_none.c, whose table is all
 *     NULL, so runtime-test and sema-test link without any game file;
 *   - runtime_resolve(): exact libc/kernel glue in runtime.c, then the core system modules (in
 *     today's order), then runtime_game.resolvers[] in the game's order, then tlggpu_resolve();
 *   - a function that two games need moves from runtime_<slug>*.c into a core runtime_*.c (first
 *     candidates, from docs/LIBRARIES.md: the event flags and the module/unwind queries in
 *     runtime_tlg.c, the ErrorDialog in runtime_gr2.c);
 *   - the env prefix and the profile file feed the generic configuration loader (core_config.c,
 *     also a plan): core defaults < game profile.ini < player ini; environment over all, for
 *     testing only.
 *
 * Migration, one port at a time (each in its own PR, tested on its game, under the lock):
 *   1. add game_<slug>.c with a table that points at the existing resolvers;
 *   2. switch runtime.c/runtime.h to the table (one core change, synced to every port);
 *   3. drop the other games' runtime_*.c from the port.
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
 * relocating, before the entry point). */
typedef struct RuntimeGameImage {
    const char *title_id;   /* "CUSA00223", from sce_sys/param.sfo */
    const char *app_ver;    /* "01.07" (the update's param.sfo when there is one) */
    unsigned char *base;    /* mapped eboot image */
    uint64_t size;          /* image bytes */
    /* Whether [offset, offset + length) is inside a mapped segment of the image. */
    int (*mapped)(uint64_t offset, uint64_t length);
    /* Executable host memory for trampolines (one page per call; never freed). */
    void *(*allocate_code)(size_t size);
} RuntimeGameImage;

typedef struct RuntimeGame {
    /* Identity. slug: "tlg", "gr2", "infamous" (file names, ~/.config/<slug>-launcher).
     * name: window title / Alt+Tab ("inFAMOUS Second Son"); TLG_APP_NAME still overrides. */
    const char *slug;
    const char *name;
    /* Title IDs this table serves, NULL-terminated ({"CUSA00223", NULL}). The loader warns when
     * the mounted game is not in the list (it still runs: regions share the code). */
    const char *const *title_ids;

    /* Environment prefix without the underscore: "TLG", "GR2", "ISS" (PORT_ENV_PREFIX in
     * port/packaging/port.env). The configuration loader looks up <prefix>_X, then TLG_X during
     * the transition (logging the old name once). */
    const char *env_prefix;
    /* Game profile (defaults of layer 2), relative to the executable: "profile.ini". NULL: none.
     * <prefix>_PROFILE overrides the path. */
    const char *profile;

    /* Game-specific system functions, tried in this order after the core modules and before the
     * GPU layer; NULL-terminated. GR2 puts runtime_gr2_audio3d_resolve first (TLG's Audio3d
     * entries are placeholders). */
    const RuntimeGameResolver *resolvers;

    /* Optional hooks; NULL when unused. */
    /* After the configuration is loaded and runtime_init() ran, before the image is loaded. */
    void (*init)(void);
    /* Eboot patches in code (cheats, stat multipliers, FPS hooks that need a trampoline). Data
     * patches stay in <slug>_patches.py / patches/<name>.xml. Returns the number of patches applied. */
    int (*patch_image)(const RuntimeGameImage *image);
    /* Extra lines for the end-of-run report (runtime_report); e.g. runtime_gr2_audio3d_report. */
    void (*report)(void);
} RuntimeGame;

/* Defined by the port in port/src/game_<slug>.c (core-only builds: port/src/game_none.c). */
extern const RuntimeGame runtime_game;

/* Example (port/src/game_infamous.c, not compiled anywhere yet):
 *
 *   #include "runtime.h"
 *   #include "runtime_game.h"
 *   static const char *const titles[]={"CUSA00223",NULL};
 *   static const RuntimeGameResolver resolvers[]={
 *       runtime_infamous_resolve,
 *       runtime_gr2_resolve,      // until ErrorDialog and the content queries move to the core
 *       runtime_tlg_resolve,      // until the event flags move to the core
 *       NULL,
 *   };
 *   const RuntimeGame runtime_game={
 *       .slug="infamous", .name="inFAMOUS Second Son", .title_ids=titles,
 *       .env_prefix="ISS", .profile="profile.ini", .resolvers=resolvers,
 *   };
 *
 * And the core side (runtime.c), replacing the fixed calls:
 *
 *   for (const RuntimeGameResolver *r=runtime_game.resolvers; r && *r; ++r) {
 *       uintptr_t found=(*r)(name);
 *       if (found) return found;
 *   }
 *   return tlggpu_resolve(name);
 */

#ifdef __cplusplus
}
#endif

#endif /* RUNTIME_GAME_H */
