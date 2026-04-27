# Conan recipe overrides

Local Conan recipes that take precedence over Conan Center while a fix lands
upstream. `build.sh` exports every `conan_overrides/<pkg>/all/` directory it
finds before invoking `conan install`, so the local revision wins resolution.

Each override should:

1. Stay as close as possible to the upstream Conan Center recipe — copy the
   file verbatim, then apply the minimum diff needed.
2. Document the diff and the upstream tracking link in a header comment at
   the top of `conanfile.py`.
3. Be removed once the upstream recipe carries the fix.

## Active overrides

### mpdecimal/2.5.1

Adds Windows ARM64 (`armv8`) support so plugins that pull `cpython` build on
`windows-11-arm` runners. Upstream rejects MSVC + non-x86 in `validate()` and
the `_build_msvc()` machine dict has no `armv8` entry. mpdecimal's source
already supports ARM64 via the `ansi64` C path (see upstream `vcbuild_arm64.bat`
in mpdecimal 4.0.1) — only the recipe needs adjustment.

Track upstream: <https://github.com/conan-io/conan-center-index/blob/master/recipes/mpdecimal/all/conanfile.py>
