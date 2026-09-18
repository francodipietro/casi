# SPDX-License-Identifier: AGPL-3.0-only
cmake_minimum_required(VERSION 3.20)

project(casi
    VERSION 0.1.0
    DESCRIPTION "Coding Assistant Session Interchange"
    HOMEPAGE_URL "https://github.com/francodipietro/casi"
    LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
# gnu11 rather than strict c11: gives POSIX + BSD declarations (strdup, mkdir,
# realpath, DT_DIR) uniformly on Linux and macOS without per-platform feature
# macros. MSVC ignores the flag.
set(CMAKE_C_EXTENSIONS ON)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(CASI_WERROR          "Treat compiler warnings as errors"        OFF)
option(CASI_SANITIZE        "Build with ASan + UBSan"                  OFF)
option(CASI_VENDOR_LIBGIT2  "Build libgit2 from source and link it in" OFF)
option(CASI_BUILD_TESTS     "Build the test suite"                     ON)

# ---------------------------------------------------------------- libgit2 ---
# Preferred path: system libgit2 via pkg-config (brew / apt / pacman).
# Fallback path (-DCASI_VENDOR_LIBGIT2=ON): pinned source build, used for the
# static release binaries and for the Windows CI job.
#
# The floor is 1.7: that is what the code actually needs, and it is what
# Debian/Ubuntu ship.
#
# The security caveat is narrower than a blanket floor would suggest. The
# OpenSSH exec transport arrived in libgit2 1.8.0 and had an arbitrary command
# execution bug fixed in 1.9.2, so exactly the [1.8.0, 1.9.2) window is
# dangerous. Anything older has no exec transport at all and cannot be
# affected. Rejecting only that window keeps casi buildable on stock distro
# packages without waving the vulnerability through.
set(CASI_LIBGIT2_MIN_VERSION 1.7.0)

option(CASI_ALLOW_UNSAFE_LIBGIT2
    "Build against a libgit2 whose OpenSSH exec transport is unpatched" OFF)

if(CASI_VENDOR_LIBGIT2)
    include(FetchContent)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(BUILD_TESTS       OFF CACHE BOOL "" FORCE)
    set(BUILD_CLI         OFF CACHE BOOL "" FORCE)
    set(USE_SSH           "exec" CACHE STRING "" FORCE)
    FetchContent_Declare(libgit2
        GIT_REPOSITORY https://github.com/libgit2/libgit2.git
        GIT_TAG        v1.9.7
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(libgit2)
    # libgit2's packaged target does not export its include directory, so
    # attach it here rather than making every consumer know the source layout.
    #
    # BUILD_INTERFACE is required, not decorative: FetchContent puts the source
    # under the build tree, and CMake rejects a bare INTERFACE include path
    # there for any target reachable from an install() rule -- that path would
    # not exist for someone consuming an exported target.
    target_include_directories(libgit2package
        INTERFACE $<BUILD_INTERFACE:${libgit2_SOURCE_DIR}/include>)
    add_library(casi::libgit2 ALIAS libgit2package)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBGIT2 REQUIRED IMPORTED_TARGET
        libgit2>=${CASI_LIBGIT2_MIN_VERSION})

    if(LIBGIT2_VERSION VERSION_GREATER_EQUAL 1.8.0 AND
       LIBGIT2_VERSION VERSION_LESS 1.9.2 AND
       NOT CASI_ALLOW_UNSAFE_LIBGIT2)
        message(FATAL_ERROR
            "libgit2 ${LIBGIT2_VERSION} may carry an unpatched arbitrary "
            "command execution bug in its OpenSSH exec transport, fixed in "
            "1.9.2. Upgrade, build with -DCASI_VENDOR_LIBGIT2=ON, or override "
            "with -DCASI_ALLOW_UNSAFE_LIBGIT2=ON if you know this build uses "
            "libssh2 instead.")
    endif()

    if(LIBGIT2_VERSION VERSION_LESS 1.9.0)
        message(STATUS
            "libgit2 ${LIBGIT2_VERSION} predates git_libgit2_feature_backend(); "
            "the SSH backend will be reported as libssh2, the only one it has")
    endif()

    add_library(casi::libgit2 ALIAS PkgConfig::LIBGIT2)
endif()

# ------------------------------------------------------------- core library --
# Everything except main() lives here so the tests can link it directly.
add_library(casi_core STATIC
    src/casi.c
    src/util/error.c
    src/util/buf.c
    src/util/str.c
    src/util/fs.c
    src/util/log.c
    src/util/paths.c
    src/util/config.c
    src/util/jsonl.c
    src/path/encoding.c
    src/path/roots.c
    src/core/chunk.c
    src/core/index.c
    src/core/repo.c
    src/core/shared_config.c
    src/core/store.c
    src/core/sync.c
    src/core/ctx.c
    src/provider/provider.c
    src/provider/claude_code.c
)

target_include_directories(casi_core
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_BINARY_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(casi_core PUBLIC casi::libgit2)

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/include/casi/version.h
    @ONLY)

# ------------------------------------------------------------------ binary ---
add_executable(casi
    src/main.c
    src/cmd/cmd_version.c
    src/cmd/cmd_help.c
    src/cmd/cmd_config.c
    src/cmd/cmd_doctor.c
    src/cmd/cmd_conflicts.c
    src/cmd/cmd_init.c
    src/cmd/cmd_status.c
    src/cmd/cmd_push.c
    src/cmd/cmd_pull.c
    src/cmd/cmd_sync.c
    src/cmd/cmd_exclude.c
    src/cmd/sync_ops.c
)
target_include_directories(casi PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(casi PRIVATE casi_core)

# ----------------------------------------------------------------- warnings --
if(MSVC)
    set(CASI_WARN_FLAGS /W4)
    if(CASI_WERROR)
        list(APPEND CASI_WARN_FLAGS /WX)
    endif()
else()
    set(CASI_WARN_FLAGS
        -Wall -Wextra -Wshadow -Wpointer-arith -Wwrite-strings
        -Wstrict-prototypes -Wmissing-prototypes -Wvla)
    if(CASI_WERROR)
        list(APPEND CASI_WARN_FLAGS -Werror)
    endif()
endif()
target_compile_options(casi_core PRIVATE ${CASI_WARN_FLAGS})
target_compile_options(casi      PRIVATE ${CASI_WARN_FLAGS})

target_compile_definitions(casi_core PRIVATE _FILE_OFFSET_BITS=64)

if(CASI_SANITIZE)
    if(MSVC)
        message(WARNING "CASI_SANITIZE is not supported with MSVC; ignoring")
    else()
        set(CASI_SAN_FLAGS -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_compile_options(casi_core PUBLIC ${CASI_SAN_FLAGS})
        target_link_options(casi_core    PUBLIC ${CASI_SAN_FLAGS})
    endif()
endif()

# ------------------------------------------------------------------- tests ---
if(CASI_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ----------------------------------------------------------------- install ---
include(GNUInstallDirs)
install(TARGETS casi RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
