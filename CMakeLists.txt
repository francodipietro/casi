# SPDX-License-Identifier: AGPL-3.0-only
cmake_minimum_required(VERSION 3.20)

project(casi
    VERSION 0.2.0
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
option(CASI_FULLY_STATIC    "Link a Linux artifact without shared libraries" OFF)

if(CASI_FULLY_STATIC)
    if(APPLE)
        message(FATAL_ERROR "CASI_FULLY_STATIC is only supported for Linux release artifacts")
    endif()
    # This is intentionally a separate switch from CASI_VENDOR_LIBGIT2:
    # macOS release binaries vendor third-party libraries but retain system
    # frameworks, whereas Linux prebuilt archives are completely static musl.
    set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")
    string(APPEND CMAKE_EXE_LINKER_FLAGS " -static")
endif()

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

# -------------------------------------------------------------- libsodium ---
# Phase 4 encrypts remote content with the portable XChaCha20-Poly1305 AEAD
# implementation from libsodium.  1.0.12 introduced that construction.
find_package(PkgConfig REQUIRED)
if(CASI_VENDOR_LIBGIT2)
    # `--preset static` is the prebuilt-release path.  Ask pkg-config for the
    # static archive here too; otherwise Homebrew (and many Linux distros)
    # silently selects libsodium's shared library and turns a supposedly
    # self-contained artifact into one with a local Cellar path.
    set(CASI_PKG_CONFIG_ARGN_SAVE "${PKG_CONFIG_ARGN}")
    list(APPEND PKG_CONFIG_ARGN --static)
    pkg_check_modules(SODIUM REQUIRED libsodium>=1.0.12)
    set(PKG_CONFIG_ARGN "${CASI_PKG_CONFIG_ARGN_SAVE}")

    # FindPkgConfig resolves `-lsodium` to the shared library on platforms
    # that provide both variants. Select the archive explicitly, retaining
    # pkg-config's static transitive flags (for example `-pthread`).
    find_file(CASI_SODIUM_STATIC_LIBRARY NAMES libsodium.a
              PATHS ${SODIUM_STATIC_LIBRARY_DIRS} NO_DEFAULT_PATH)
    if(NOT CASI_SODIUM_STATIC_LIBRARY)
        message(FATAL_ERROR
            "CASI_VENDOR_LIBGIT2=ON requires a static libsodium archive; "
            "install the libsodium development/static package")
    endif()
    set(CASI_SODIUM_STATIC_TRANSITIVE ${SODIUM_STATIC_LIBRARIES})
    list(REMOVE_ITEM CASI_SODIUM_STATIC_TRANSITIVE sodium)
    add_library(casi::sodium INTERFACE IMPORTED)
    set_target_properties(casi::sodium PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${SODIUM_STATIC_INCLUDE_DIRS}"
        INTERFACE_LINK_LIBRARIES
            "${CASI_SODIUM_STATIC_LIBRARY};${CASI_SODIUM_STATIC_TRANSITIVE}"
        INTERFACE_LINK_OPTIONS "${SODIUM_STATIC_LDFLAGS_OTHER}")
else()
    pkg_check_modules(SODIUM REQUIRED IMPORTED_TARGET libsodium>=1.0.12)
    add_library(casi::sodium ALIAS PkgConfig::SODIUM)
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
    src/core/crypto.c
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
target_link_libraries(casi_core PUBLIC casi::libgit2 casi::sodium)

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
    src/cmd/cmd_gc.c
    src/cmd/cmd_diagnose.c
    src/cmd/cmd_migrate.c
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

# CPack covers the system-linked packages.  Prebuilt artifacts use the
# `static` preset instead: it vendors libgit2, while the release workflow also
# bundles the libsodium dependency rather than asking an end user to install
# build tooling or development headers.
set(CPACK_PACKAGE_NAME "casi")
set(CPACK_PACKAGE_VENDOR "francodipietro")
set(CPACK_PACKAGE_CONTACT "https://github.com/francodipietro/casi/issues")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Coding Assistant Session Interchange")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/francodipietro/casi")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_PACKAGE_FILE_NAME
    "${CPACK_PACKAGE_NAME}-${PROJECT_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
set(CPACK_GENERATOR "TGZ")

if(UNIX AND NOT APPLE)
    list(APPEND CPACK_GENERATOR DEB)
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
    set(CPACK_DEBIAN_PACKAGE_SECTION "utils")
    set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
endif()

include(CPack)
