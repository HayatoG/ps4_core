/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * inFAMOUS Second Son (infamous_pc / iss_pc): hook table for the core (src/runtime_game.h). Copy
 * to the port's port/src/game_infamous.c. Same order as the fixed calls it replaces (the port
 * carries GR2's and TLG's files: HTTP/NP from runtime_gr2_np.c, NGS2 placeholders from
 * runtime_tlg.c). The ISS_ alias comes from here, so port.env needs no PORT_ENV_PREFIX.
 */
#include "runtime.h"
#include "runtime_game.h"

static const char *const titles[] = {"CUSA00223", NULL};
static const RuntimeGameResolver resolvers[] = {
    runtime_gr2_audio3d_resolve,
    runtime_tlg_resolve,
    runtime_gr2_resolve,
    runtime_gr2_np_resolve,
    runtime_gr2_font_resolve,
    runtime_gr2_json_resolve,
    runtime_infamous_resolve,
    NULL,
};
const RuntimeGame runtime_game = {
    .slug = "infamous",
    .name = "inFAMOUS Second Son",
    .title_ids = titles,
    .env_prefix = "ISS_",
    .profile = "profile.ini",
    .resolvers = resolvers,
    .report = runtime_gr2_audio3d_report,
};
