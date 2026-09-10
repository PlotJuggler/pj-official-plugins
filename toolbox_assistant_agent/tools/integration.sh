#!/usr/bin/env bash
# The integration line for the Assistant Agent (ROADMAP.md → How this is verified,
# ARCHITECTURE.md → The integration line): every open change that serves the assistant,
# built and deployed together.
#
#   SDK     : the worktree of the single SDK PR (#184), exported to the local Conan cache
#   PJ4     : a LOCAL, never-pushed branch `integration/assistant` = host-ux + #619, pinned to
#             that export, built with the host tests
#   plugin  : this repo's assistant branch, built against the same export
#   deploy  : a COPY of the plugin directory plus a launcher; never the build tree itself,
#             because overwriting a shared object the running app has mapped crashes it at exit
#
# Idempotent: rerun after any commit on any draft. `--reset` recreates the PJ4 integration
# branch from scratch (drops its local merge commits). `--no-build` only refreshes the trees.
set -euo pipefail

PJ4="${PJ4:-$HOME/Work/PJ4}"
SDK_WT="${SDK_WT:-$HOME/Work/plotjuggler_sdk/.worktrees/playback-viewport}"
PLUGINS="${PLUGINS:-$HOME/Work/pj-official-plugins/.worktrees/assistant}"
DEPLOY="${DEPLOY:-$HOME/Work/assistant-demo/deploy}"
HOST_BRANCH="${HOST_BRANCH:-alvvm/assistant-host-ux}"
QUALIFIED_BRANCH="${QUALIFIED_BRANCH:-fix/dataset-qualified-inputs}"
INT_BRANCH="integration/assistant"
INT_WT="$PJ4/.worktrees/integration"
APP_UNIT="${APP_UNIT:-pj4-assistant-app}"   # the user's running app, if launched as a user unit

RESET=0; BUILD=1
for a in "$@"; do
  case "$a" in
    --reset) RESET=1 ;;
    --no-build) BUILD=0 ;;
    -h|--help) sed -n 2,14p "$0"; exit 0 ;;
    *) echo "integration: unknown flag $a" >&2; exit 2 ;;
  esac
done

say() { printf '\n== %s\n' "$*"; }

# ---------------------------------------------------------------- 0. guards
for d in "$PJ4" "$SDK_WT" "$PLUGINS"; do
  [[ -d "$d/.git" || -f "$d/.git" ]] || { echo "integration: not a git checkout: $d" >&2; exit 1; }
done
if systemctl --user is-active --quiet "$APP_UNIT" 2>/dev/null; then
  echo "integration: the app unit '$APP_UNIT' is running; stop it before rebuilding what it has mapped" >&2
  exit 1
fi

# ---------------------------------------------------------------- 1. SDK export
say "SDK: exporting $SDK_WT ($(git -C "$SDK_WT" rev-parse --short HEAD), VERSION $(cat "$SDK_WT/VERSION"))"
SDK_VERSION="$(tr -d '[:space:]' < "$SDK_WT/VERSION")"
EXPORT_LINE="$(conan export "$SDK_WT" 2>&1 | grep -E 'Exported: plotjuggler_sdk/' | tail -1)"
RREV="${EXPORT_LINE##*#}"; RREV="${RREV%% *}"
[[ "$RREV" =~ ^[0-9a-f]{32}$ ]] || { echo "integration: could not read the exported rrev from: $EXPORT_LINE" >&2; exit 1; }
echo "   plotjuggler_sdk/$SDK_VERSION#$RREV"

# ---------------------------------------------------------------- 2. PJ4 integration worktree
say "PJ4: $INT_BRANCH = $HOST_BRANCH + $QUALIFIED_BRANCH"
if [[ $RESET == 1 && -d "$INT_WT" ]]; then
  git -C "$PJ4" worktree remove --force "$INT_WT"
  git -C "$PJ4" branch -D "$INT_BRANCH" >/dev/null 2>&1 || true
fi
if [[ ! -d "$INT_WT" ]]; then
  git -C "$PJ4" worktree add -b "$INT_BRANCH" "$INT_WT" "$HOST_BRANCH"
  ln -sfn "$(readlink -f "$PJ4/.qt")" "$INT_WT/.qt"   # ABSOLUTE: a relative link resolves inside the worktree
fi
[[ -z "$(git -C "$INT_WT" status --porcelain)" ]] || { echo "integration: $INT_WT is dirty; commit or drop your changes first" >&2; exit 1; }

# host-ux is the base of record: merge it first, then #619; on the Conan pin host-ux wins.
merge_into_integration() {
  local ref="$1"
  if git -C "$INT_WT" merge --no-edit "$ref" >/dev/null 2>&1; then return 0; fi
  local conflicted; conflicted="$(git -C "$INT_WT" diff --name-only --diff-filter=U)"
  local f
  for f in $conflicted; do
    case "$f" in
      conanfile.txt|conan.lock) git -C "$INT_WT" checkout --ours -- "$f"; git -C "$INT_WT" add -- "$f" ;;
      *) echo "integration: real conflict merging $ref in $f — resolve by hand in $INT_WT" >&2; exit 1 ;;
    esac
  done
  git -C "$INT_WT" commit -q --no-edit
}
merge_into_integration "$HOST_BRANCH"
merge_into_integration "$QUALIFIED_BRANCH"

# Pin to the export above (the lock names an rrev, so it must be rewritten, not just the version).
sed -i -E "s|^plotjuggler_sdk/[0-9.]+$|plotjuggler_sdk/$SDK_VERSION|" "$INT_WT/conanfile.txt"
sed -i -E "s|\"plotjuggler_sdk/[0-9.]+#[0-9a-f]+\"|\"plotjuggler_sdk/$SDK_VERSION#$RREV\"|" "$INT_WT/conan.lock"
if [[ -n "$(git -C "$INT_WT" status --porcelain)" ]]; then
  git -C "$INT_WT" -c commit.gpgsign=false commit -qam "integration: pin plotjuggler_sdk/$SDK_VERSION#$RREV (local line, never pushed)"
fi
echo "   $(git -C "$INT_WT" log --oneline -1)"

[[ $BUILD == 1 ]] || { say "trees refreshed (--no-build)"; exit 0; }

# ---------------------------------------------------------------- 3. PJ4 build + tests
say "PJ4: configure + build + ctest"
source "$INT_WT/versions.env"
QT_DIR="$INT_WT/.qt/${PJ_QT_VERSION}/gcc_64"
B="$INT_WT/build"
"$INT_WT/scripts/configure_conan_remote.sh" >/dev/null
conan install "$INT_WT" --output-folder="$B" --build=missing \
  --lockfile="$INT_WT/conan.lock" --lockfile-partial \
  -s build_type=RelWithDebInfo -s compiler.cppstd=20 -r plotjuggler-conan -r conancenter >/dev/null
cmake -S "$INT_WT" -B "$B" \
  -DCMAKE_TOOLCHAIN_FILE="$B/conan_toolchain.cmake" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_PREFIX_PATH="$QT_DIR" \
  -DPJ_VERSION="${PJ_VERSION:-${PJ_APP_VERSION}}" \
  -DPJ_INSTALLATION=source \
  -UCMAKE_C_COMPILER_LAUNCHER -UCMAKE_CXX_COMPILER_LAUNCHER \
  -DFETCHCONTENT_SOURCE_DIR_PLOTJUGGLER_SDK="$SDK_WT" >/dev/null
cmake --build "$B" -j "$(nproc)"
(cd "$B" && ctest --output-on-failure -j8)

# ---------------------------------------------------------------- 4. plugin build + tests
say "plugin: build + ctest against plotjuggler_sdk/$SDK_VERSION"
[[ "$(tr -d '[:space:]' < "$PLUGINS/SDK_VERSION")" == "$SDK_VERSION" ]] || {
  echo "integration: $PLUGINS/SDK_VERSION says $(cat "$PLUGINS/SDK_VERSION") but the SDK worktree is $SDK_VERSION" >&2; exit 1; }
(cd "$PLUGINS" && ./build.sh toolbox_assistant_agent >/dev/null)
(cd "$PLUGINS/build/toolbox_assistant_agent/Release" && ctest --output-on-failure -j8)
# build/all is what --plugin-dir points at (it carries the loaders); build.sh <plugin> does not refresh it.
PLUGIN_SO="$PLUGINS/build/toolbox_assistant_agent/Release/bin/libtoolbox_assistant_agent_plugin.so"
ALL_BIN="$PLUGINS/build/all/Release/bin"
[[ -d "$ALL_BIN" ]] || { echo "integration: $ALL_BIN missing — run ./build.sh (all plugins) once in $PLUGINS" >&2; exit 1; }
cp "$PLUGIN_SO" "$ALL_BIN/.new.so" && mv -f "$ALL_BIN/.new.so" "$ALL_BIN/$(basename "$PLUGIN_SO")"

# ---------------------------------------------------------------- 5. deploy (a copy, never the build tree)
say "deploy → $DEPLOY"
mkdir -p "$DEPLOY"
rm -rf "$DEPLOY/plugins.new" && cp -a "$ALL_BIN" "$DEPLOY/plugins.new"
rm -rf "$DEPLOY/plugins.old"; [[ -d "$DEPLOY/plugins" ]] && mv "$DEPLOY/plugins" "$DEPLOY/plugins.old"
mv "$DEPLOY/plugins.new" "$DEPLOY/plugins"; rm -rf "$DEPLOY/plugins.old"
cat > "$DEPLOY/run.sh" <<EOF
#!/usr/bin/env bash
# Launch the integration host with the deployed plugins. Extra args go to plotjuggler4
# (e.g. --layout ~/Work/assistant-demo/nissan_assistant.pj4.xml).
export LD_LIBRARY_PATH="$QT_DIR/lib\${LD_LIBRARY_PATH:+:\$LD_LIBRARY_PATH}"
exec "$B/pj_app/plotjuggler4" --plugin-dir "$DEPLOY/plugins" "\$@"
EOF
chmod +x "$DEPLOY/run.sh"
cat > "$DEPLOY/STATE" <<EOF
built     $(date -u +%Y-%m-%dT%H:%M:%SZ)
sdk       $(git -C "$SDK_WT" rev-parse --short HEAD)  plotjuggler_sdk/$SDK_VERSION#$RREV
pj4       $(git -C "$INT_WT" rev-parse --short HEAD)  ($INT_BRANCH = $HOST_BRANCH $(git -C "$PJ4" rev-parse --short "$HOST_BRANCH") + $QUALIFIED_BRANCH $(git -C "$PJ4" rev-parse --short "$QUALIFIED_BRANCH"))
plugin    $(git -C "$PLUGINS" rev-parse --short HEAD)
EOF
cat "$DEPLOY/STATE"
echo; echo "integration: OK — run: $DEPLOY/run.sh --layout ~/Work/assistant-demo/nissan_assistant.pj4.xml"
