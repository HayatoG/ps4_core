# SPDX-License-Identifier: GPL-2.0-or-later
function(tlg_perf_build_revision source_dir output)
    find_package(Git QUIET)
    set(revision "unknown")
    if(GIT_FOUND)
        execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse --short=12 HEAD
                        WORKING_DIRECTORY "${source_dir}"
                        OUTPUT_VARIABLE revision OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        if(NOT revision MATCHES "^[0-9a-f]+$")
            set(revision "unknown")
        endif()
        execute_process(COMMAND "${GIT_EXECUTABLE}" diff --quiet HEAD --
                        WORKING_DIRECTORY "${source_dir}" RESULT_VARIABLE dirty ERROR_QUIET)
        # 1 means changes; any other non-zero code is git failing (e.g. not a repository).
        if(dirty EQUAL 1)
            string(APPEND revision "-dirty")
        endif()
    endif()
    set(${output} "${revision}" PARENT_SCOPE)
endfunction()
