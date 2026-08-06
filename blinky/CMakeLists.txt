# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(bento_blinky)

# The app version lives in ./VERSION; Zephyr turns it into
# <zephyr/app_version.h>. APP_BUILD_VERSION in that header comes from
# `git describe` run in this directory, but the generated header is only
# rebuilt when its declared dependencies change. Without this the git
# description goes stale as soon as HEAD moves, so depend on the git index.
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/VERSION)
  find_package(Git QUIET)
  if(GIT_FOUND)
    # --path-format=absolute avoids the relative-path ambiguity of a bare
    # `--git-path` (which resolves against the CWD, not the repo root).
    execute_process(
      COMMAND ${GIT_EXECUTABLE} rev-parse --path-format=absolute --git-path index
      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
      OUTPUT_VARIABLE app_git_index
      OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET
      RESULT_VARIABLE app_git_index_rc
    )
    # Only add the dependency if the index actually exists: ninja treats a
    # missing dependency with no rule to build it as a hard error.
    if(app_git_index_rc EQUAL 0 AND EXISTS ${app_git_index})
      set_property(TARGET app_version_h PROPERTY APP_VERSION_DEPENDS ${app_git_index})
    endif()
  endif()
endif()

target_sources(app PRIVATE src/main.c)
target_sources_ifdef(CONFIG_SHELL app PRIVATE src/blinky_shell.c)
