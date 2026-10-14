/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Core-only build: no game hooks (unit tests, ps4_core CI). A port replaces this file with its
 * own src/game_<slug>.c (see runtime_game.h and port/games/). */
#include "runtime_game.h"

const RuntimeGame runtime_game = {
    .slug = "none",
    .name = "ps4_core",
};
