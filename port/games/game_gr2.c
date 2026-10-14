/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Gravity Rush 2 (gr2_pc): hook table for the core (src/runtime_game.h). Copy to the port's
 * port/src/game_gr2.c. Same order as the fixed calls it replaces: Audio3d first (the old TLG
 * file had Audio3d placeholders), then runtime_tlg.c (NGS2 placeholders), then GR2's own files.
 * No env alias yet (GR2_ is planned; set .env_prefix = "GR2_" when the launcher switches).
 */
#include "runtime.h"
#include "runtime_game.h"

static const char *const titles[] = {"CUSA03694", NULL};
static const RuntimeGameResolver resolvers[] = {
    runtime_gr2_audio3d_resolve,
    runtime_tlg_resolve,
    runtime_gr2_resolve,
    runtime_gr2_np_resolve,
    runtime_gr2_font_resolve,
    runtime_gr2_json_resolve,
    NULL,
};
const RuntimeGame runtime_game = {
    .slug = "gr2",
    .name = "Gravity Rush 2",
    .title_ids = titles,
    .env_prefix = NULL,
    .profile = "profile.ini",
    .resolvers = resolvers,
    .report = runtime_gr2_audio3d_report,
};
