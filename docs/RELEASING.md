# Releasing casi

This document describes the release path implemented by
`.github/workflows/release.yml`. It is intentionally tag-driven: pull requests
and ordinary pushes cannot create a GitHub Release.

## Prepare a release

1. Update `project(VERSION ...)` in `CMakeLists.txt` and the user-visible
   release notes.
2. Make the repository public before the first release. The workflow
   deliberately stops for a private repository rather than publishing an
   unattested release.
3. Merge the release PR after its local independent review and required CI pass.
4. From that merge commit, create and push an annotated tag named
   `v<project-version>`, for example `v0.1.0`.

The release workflow rejects a tag whose version does not exactly match
`CMakeLists.txt`. It builds from the tag, rather than from an uploaded
developer artifact.

## Produced assets

The workflow publishes these assets and a `checksums.txt` file in the GitHub
Release:

- `casi_<version>_linux_x86_64.tar.gz` — fully static musl binary.
- `casi_<version>_linux_aarch64.tar.gz` — fully static musl binary.
- `casi_<version>_darwin_universal.tar.gz` — an ad-hoc-signed universal
  (`x86_64` + `arm64`) binary.
- A system-linked Debian package produced by CPack.

Linux prebuilt jobs use `CASI_FULLY_STATIC=ON`; they fail if the final binary
has a dynamic dependency. macOS uses static third-party libraries and retains
only operating-system frameworks and libraries. The Debian package instead
uses the distribution's libgit2 and libsodium packages, with
`dpkg-shlibdeps` calculating its runtime dependencies.

Before publishing, the workflow generates SHA-256 checksums and creates a
GitHub build-provenance attestation for every asset. A user can verify a
download with:

```sh
sha256sum -c checksums.txt
gh attestation verify <asset> --owner francodipietro
```

## External package repositories and public release

The Homebrew formula belongs in the `francodipietro/tap` repository and the
AUR `PKGBUILD` belongs in its own package repository; neither is mirrored in
this source tree. Both must pin the immutable release URL and the checksum
from `checksums.txt`, so they are updated only after the release has produced
those values.

Making this repository public, creating the tag/release, or changing either
external package repository is an intentional owner action. The workflow and
the source-tree packaging are ready first; do not make those external changes
as a side effect of merging a pull request.
