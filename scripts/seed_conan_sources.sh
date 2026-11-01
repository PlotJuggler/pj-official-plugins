#!/usr/bin/env bash
# Point Conan's source download cache at .github/conan/source-backup. Anything not
# seeded there still comes from the recipe's own URLs.
set -euo pipefail

cache_dir="${RUNNER_TEMP:-/tmp}/conan-source-cache"
mkdir -p "${cache_dir}/s"
find .github/conan/source-backup -maxdepth 1 -type f ! -name README.md -exec cp -f {} "${cache_dir}/s/" \;
conan_home="$(conan config home)"
mkdir -p "${conan_home}"
echo "core.sources:download_cache=${cache_dir}" >> "${conan_home}/global.conf"
