// SPDX-License-Identifier: GPL-2.0-or-later
// tlg: while a ScopedAssertThrows lives on this thread, a failed ASSERT/UNREACHABLE throws
// std::runtime_error instead of stopping the port. The first-run shader compilation uses it: a
// record the translator cannot handle fails alone instead of ending the game's start (the
// shader then compiles in game as before, where the same assertion would stop it).
#pragma once

namespace Common {
extern thread_local int assert_throws;

struct ScopedAssertThrows {
    ScopedAssertThrows() {
        ++assert_throws;
    }
    ~ScopedAssertThrows() {
        --assert_throws;
    }
    ScopedAssertThrows(const ScopedAssertThrows&) = delete;
    ScopedAssertThrows& operator=(const ScopedAssertThrows&) = delete;
};
} // namespace Common
