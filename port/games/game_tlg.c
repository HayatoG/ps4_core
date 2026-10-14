/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The Last Guardian (tlg_pc): hook table for the core (src/runtime_game.h). Copy to the port's
 * port/src/game_tlg.c. Same resolver order as the fixed calls it replaces (runtime.c before
 * ps4_core feat/game-hooks): runtime_tlg.c only. TLG_ is the core's own prefix: no alias.
 */
#include "runtime.h"
#include "runtime_game.h"

static const char *const titles[] = {"CUSA03627", NULL};
static const RuntimeGameResolver resolvers[] = {
    runtime_tlg_resolve,
    NULL,
};
const RuntimeGame runtime_game = {
    .slug = "tlg",
    .name = "The Last Guardian",
    .title_ids = titles,
    .env_prefix = NULL,
    .profile = "profile.ini",
    .resolvers = resolvers,
};
