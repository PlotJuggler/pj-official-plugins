# Conan source backup

Source tarballs that have vanished upstream. `scripts/seed_conan_sources.sh` copies them into
Conan's source download cache, so a build from source is served locally. Conan checks each file
against the sha256 pinned in the recipe's `conandata.yml`, so a corrupted or tampered file is never
consumed.

Each source is two files: `<sha256>` (the tarball) and `<sha256>.json` (Conan's metadata sidecar).

| sha256 | Source | Why |
|---|---|---|
| `15417edc8e12…` | `mpdecimal-2.5.0.tar.gz` | bytereef.org removed it, and cpython requires exactly this version on macOS (its recipe: "mpdecimal > 2.5.0 on MacOS causes the _decimal module to not be importable") |
