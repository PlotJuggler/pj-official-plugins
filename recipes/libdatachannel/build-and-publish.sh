#!/usr/bin/env bash
# Build the libdatachannel conda package and (optionally) publish it to the
# prefix.dev "plotjuggler" channel. libdatachannel is absent from conda-forge.
set -euo pipefail
cd "$(dirname "$0")"
rm -rf src output
git clone --depth 1 --branch v0.24.0 --recurse-submodules --shallow-submodules \
  https://github.com/paullouisageneau/libdatachannel.git src
rattler-build build --recipe recipe.yaml -c conda-forge --output-dir ./output
# Publish (needs PREFIX_API_KEY with write access to the plotjuggler channel):
#   rattler-build upload prefix --channel plotjuggler --skip-existing ./output/**/*.conda
