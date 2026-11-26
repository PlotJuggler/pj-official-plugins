# Extension Release Scripts

Scripts for releasing PlotJuggler extensions and submitting them to the extension registry.

## Terminology

| Term | Definition |
|------|------------|
| **extension** | The distributable package (ZIP containing plugin + manifest.json) |
| **plugin** | The compiled binary (.so/.dll/.dylib) containing the C++ class |
| **source_dir** | The source directory containing code and manifest.json |

## File Structure

| File | Purpose |
|------|---------|
| `release_extension.py` | Creates and pushes release tags with validation |
| `submit_to_registry.py` | Submits extensions to registry via PR |
| `build_local_registry.py` | Builds a local `file://` registry from compiled plugins (no release) |
| `release_tools.py` | Library + CLI for validation and packaging |
| `requirements.txt` | Python dependencies |

---

## Release Pipeline Overview

Each official plugin declares `min_sdk_required` in its manifest, independently of the build pin
in `SDK_VERSION`; the minimum must not exceed the build pin. Raise a plugin's minimum only when it
acquires a new mandatory host requirement, including through shared code (policy:
[`CLAUDE.md`](../CLAUDE.md#plugin-sdk-compatibility--required-pr-review)).

`python3 scripts/bump_core_version.py --check` validates the declarations in PR CI; the manifest
validator also checks them before local releases, packaging and registry submission, and published
and local registry entries preserve `min_sdk_required`. This checks declared metadata only, not
source code or runtime compatibility.

```
┌─────────────────────────────────────────────────────────────────────────────────────┐
│                              RELEASE PIPELINE                                        │
├─────────────────────────────────────────────────────────────────────────────────────┤
│                                                                                      │
│  ┌──────────────────┐    ┌──────────────────┐    ┌──────────────────┐    ┌────────┐│
│  │ 1. LOCAL SCRIPT  │    │ 2. BUILD CI      │    │ 3. REGISTRY      │    │4. REG  ││
│  │                  │    │                  │    │    SUBMISSION    │    │   CI   ││
│  │ release_extension│───►│ build-release.yml│───►│                  │───►│        ││
│  │ .py              │    │                  │    │ submit_to_       │    │validate││
│  │                  │    │ (GitHub Actions) │    │ registry.py      │    │registry││
│  └──────────────────┘    └──────────────────┘    └──────────────────┘    └────────┘│
│         │                        │                       │                    │     │
│         ▼                        ▼                       ▼                    ▼     │
│  - Manifest validation    - Build 6 platforms     - Download artifacts  - Schema   │
│  - Semver check           - Version consistency   - Verify checksums      validation│
│  - Tag conflict check     - Package ZIPs          - Build registry entry- Checksum │
│  - Push annotated tag     - Generate checksums    - Create PR             verify   │
│                           - Generate plugin notes                                   │
│                           - Upload to release                                       │
│                           - (Auto-submit if flag)                                   │
└─────────────────────────────────────────────────────────────────────────────────────┘
```

---

## Release Workflows

### Option A: Fully Automatic (Recommended)

Bumps the version, commits, tags and pushes; CI builds all platforms and submits to the registry.

```bash
python3 scripts/release_extension.py csv-loader --bump patch --submit-to-registry   # or --bump minor|major
python3 scripts/release_extension.py csv-loader --version 2.0.0 --submit-to-registry
```

### Option B: Two-Step (Release + Submit Separately)

Verify the CI builds before submitting to the registry.

```bash
# 1. Update version in manifest.json, commit and push
vim data_load_csv/manifest.json
git add data_load_csv/manifest.json
git commit -m "Bump csv-loader to v1.0.6"
git push

# 2. Create the release tag (triggers CI builds)
python3 scripts/release_extension.py csv-loader

# 3. After CI succeeds, submit to the registry
python3 scripts/submit_to_registry.py csv-loader
```

### Option C: Tag-Only (Manifest Already Updated)

Without `--bump` / `--version` the script reads the version from the committed manifest and creates
only the tag (no manifest update, no new commit). Use it after a hotfix bump, for batch releases,
or to re-create tags after cleanup.

```bash
python3 scripts/release_extension.py csv-loader --submit-to-registry
```

### Option D: Fully Manual

```bash
# After committing and pushing the manifest bump:
git tag -a "data_load_csv/v1.0.6" -m '{"extension_id":"csv-loader","version":"1.0.6"}'
git push origin "data_load_csv/v1.0.6"

# After CI completes:
python3 scripts/submit_to_registry.py csv-loader --version 1.0.6
```

The registry entry takes its `version` and `min_sdk_required` from the manifest on disk, so the
script refuses a release whose version is not the checked-out manifest's. To submit an older
release, check out its tag first.

Tag format: `{source_dir}/v{version}` (e.g., `data_load_csv/v1.0.6`)

---

## Pipeline Stages in Detail

### Stage 1: Local Script (`release_extension.py`)

**What it does:**

| Validation | Description |
|------------|-------------|
| Find source directory | Accepts directory name (`data_load_csv`) or extension id (`csv-loader`) |
| Read manifest.json | Extracts version and extension id |
| Validate manifest | Checks required fields: id, name, version, description, author |
| Validate semver | Ensures version follows semantic versioning (X.Y.Z) |
| Check tag conflicts | Verifies tag doesn't exist locally or on remote |
| Verify HEAD alignment | Ensures existing tags point to current HEAD |
| Find GitHub remote | Auto-detects remote pointing to pj-official-plugins |
| Create annotated tag | Embeds JSON metadata (extension_id, version, auto_submit flag) |
| Push tag | Pushes to GitHub to trigger CI |

**Exit conditions:**
- Fails if manifest.json is missing or invalid
- Fails if version is not valid semver
- Fails if tag exists at different commit
- Succeeds if tag already exists at HEAD (idempotent)

---

### Stage 2: Build CI (`build-release.yml`)

Triggered by tags matching `*/v*` pattern.

**Build matrix:**

| Platform | Runner | Architecture |
|----------|--------|--------------|
| Linux x86_64 | ubuntu-22.04 | x86_64 |
| Linux ARM64 | ubuntu-22.04-arm | aarch64 |
| macOS Intel | macos-15-intel | x86_64 |
| macOS Apple Silicon | macos-15 | arm64 |
| Windows x64 | windows-2022 | x64 |
| Windows ARM64 | windows-11-arm | arm64 |

> **Note:** Windows x64 is pinned to `windows-2022`: the `windows-latest` image (VS18 / MSVC 14.5x)
> breaks the libsodium build with `MSB8052` (default toolset 14.44 vs the v145 platform toolset its
> bundled solution selects). Untested with `libsodium/1.0.22`; revert to `windows-latest` once
> confirmed to build there.

**Per-platform steps:**

| Step | Description |
|------|-------------|
| Checkout | Clone repository at tagged commit |
| Install dependencies | For tags, install the tagged plugin's `conanfile.py`; for scheduled/manual full builds, install the root `conanfile.py` |
| Restore Conan cache | Use a cache key scoped by Conan version, platform, plugin, and recipe hash |
| Build | Run `./build.sh source_dir`, which installs the plugin recipe and configures CMake with `-DPJ_BUILD_PLUGIN=source_dir` |
| Test | Run plugin tests via ctest |
| **Verify version** | `release_tools.py verify-version-consistency` |
| Package | `release_tools.py create-distribution-package` |
| Compress | Create ZIP: `{ext_id}-{version}-{os}-{arch}.zip` |
| Generate checksum | SHA256 in `.sha256` file |
| Upload artifact | Store in GitHub Actions artifacts |
| Generate release notes | `release_tools.py generate-release-notes` keeps notes scoped to this plugin |
| Upload to release | Attach ZIP + checksum to GitHub Release, with plugin-scoped notes from the Linux x86_64 job |

**Post-build (if `--submit-to-registry` was used):**

| Step | Description |
|------|-------------|
| Check tag metadata | Read JSON from annotated tag message |
| Submit to registry | Run `submit_to_registry.py --version <tag version>` if `auto_submit_to_registry: true` |

Release notes come from non-merge commits touching only the tagged plugin directory (release
bookkeeping commits omitted), so a CSV release does not inherit other plugins' entries. Pass
`--include-shared` to include shared build, packaging or runtime commits.

One tag per plugin: the source directory before `/v` selects the dependency graph and the CMake
target scope. `data_load_csv/v1.2.0` runs `./build.sh data_load_csv`, which installs
`data_load_csv/conanfile.py` and configures CMake with `-DPJ_BUILD_PLUGIN=data_load_csv`, so
unrelated plugins' dependencies (Arrow, MQTT, ...) are not installed.

---

### Stage 3: Registry Submission (`submit_to_registry.py`)

**What it does:**

| Step | Description |
|------|-------------|
| Find GitHub release | Queries GitHub API for release matching version |
| List release assets | Gets all ZIP artifacts from the release |
| Download assets | Downloads each platform's ZIP |
| Verify checksums | Validates SHA256 matches `.sha256` file |
| Build registry entry | Creates JSON with download URLs and checksums |
| Attach release notes | Copies the plugin-scoped release notes into the registry PR body |
| Fetch registry JSON | Reads the current registry via the GitHub API |
| Update registry.json | Appends the release, then prunes superseded versions (see below) |
| Create PR branch | `update-{extension_id}-{version}` |
| Push and create PR | Opens PR on pj-plugin-registry |

**Several versions per extension.** The registry keeps more than one record per `id`, so a host
on an older SDK still finds a release it can load after a newer release raises
`min_sdk_required`. `update_registry()` appends the new record and `prune_registry()` then keeps
version V for platform P only if V's `min_sdk_required` is strictly lower than that of every
newer version that ships P: one record per SDK floor level, per platform. With an unchanged
floor that is "latest only". A record keeps only the platforms it still serves (an older version
survives for macOS alone when the newer one shipped without macOS assets). Versions compare by
SemVer precedence (`semver_sort_key`), pre-releases included.

A published version is immutable, with one exception: submitting it again may add platforms,
which is what the "Recover a missing platform" dispatch does. The stored record gains the new
platforms and changes in nothing else, so metadata corrected by hand in the registry survives.
A re-submission that adds nothing succeeds without opening a PR. The script refuses one that
would change a stored URL or checksum, and a new version that would be pruned at once. The
registry PR lists every record or platform entry it removes. Nothing else is removed
automatically: pulling a broken release, or publishing the new checksums of a force-recreated tag,
is a manual edit of `registry.json`. The registry's own validator rejects a file that is not pruned
by this rule.

**Registry entry format:**

```json
{
  "id": "csv-loader",
  "name": "CSV Loader",
  "version": "1.0.6",
  "description": "Load CSV files into PlotJuggler",
  "author": "PlotJuggler Team",
  "category": "data_loader",
  "repository": "https://github.com/PlotJuggler/pj-official-plugins",
  "platforms": {
    "linux-x86_64": {
      "url": "https://github.com/.../csv-loader-1.0.6-linux-x86_64.zip",
      "checksum": "sha256:abc123..."
    }
  }
}
```

---

### Stage 4: Registry Validation CI (`validate-registry.yml`)

Triggered on PRs to pj-plugin-registry that modify `registry.json`.

**Validations:**

| Check | Description |
|-------|-------------|
| JSON schema | Valid JSON structure |
| Required fields | All extensions have id, name, version, platforms |
| URL format | All artifact URLs are valid HTTPS |
| Platform coverage | Warns if missing platforms |
| **Checksum verification** | Downloads each artifact and verifies SHA256 |

Checksum verification runs on every PR.

---

## CLI Reference

### `release_extension.py`

```bash
python3 scripts/release_extension.py <source> [options]

Arguments:
  source                   Source directory (data_load_csv) or extension id (csv-loader)

Options:
  --bump TYPE              Bump version automatically:
                             patch: 1.0.5 -> 1.0.6
                             minor: 1.0.5 -> 1.1.0
                             major: 1.0.5 -> 2.0.0
  --version VERSION        Set explicit version (e.g., 2.0.0)
  --submit-to-registry     Submit to registry after CI builds complete
  --dry-run                Show what would be done without making changes
  --remote NAME            Git remote to push to (default: auto-detect GitHub)
  --token TOKEN            GitHub token (or set GITHUB_TOKEN env var)

Examples:
  # Bump patch version and auto-submit
  python3 scripts/release_extension.py csv-loader --bump patch --submit-to-registry

  # Set specific version
  python3 scripts/release_extension.py csv-loader --version 2.0.0 --submit-to-registry

  # Tag-only: manifest already has correct version, just create tag
  python3 scripts/release_extension.py csv-loader --submit-to-registry

  # Preview what would happen
  python3 scripts/release_extension.py csv-loader --bump minor --dry-run
```

### `submit_to_registry.py`

```bash
python3 scripts/submit_to_registry.py <source> [options]

Arguments:
  source                   Source directory or extension id

Options:
  --version VERSION        Submit specific version (default: latest release)
  --list-releases          List available releases and exit
  --dry-run                Show registry entry without creating PR
  --skip-checksum-verify   Skip downloading and verifying checksums (CI use)
  --release-notes FILE     Include plugin-scoped release notes in the registry PR
```

### `release_tools.py` Subcommands

#### verify-version-consistency

Verify tag, manifest, and plugin binary versions match.

```bash
python3 scripts/release_tools.py verify-version-consistency \
    --release-tag "data_load_csv/v1.0.5" \
    --build-dir build/Release \
    --check-manifest
```

#### create-distribution-package

Create distribution folder structure for ZIP compression.

```bash
python3 scripts/release_tools.py create-distribution-package \
    --release-tag "data_load_csv/v1.0.5" \
    --build-dir build/Release \
    --output-dir dist \
    --os-label linux \
    --arch x86_64
```

Outputs ZIP filename to stdout: `csv-loader-1.0.5-linux-x86_64.zip`

#### extract-embedded-manifest

Extract manifest JSON from compiled plugin binary.

```bash
python3 scripts/release_tools.py extract-embedded-manifest \
    build/Release/bin/libcsv_source_plugin.so
```

#### validate-manifest

Validate manifest.json structure and fields.

```bash
python3 scripts/release_tools.py validate-manifest data_load_csv/manifest.json
```

#### validate-distribution-package

Validate a distribution ZIP package.

```bash
python3 scripts/release_tools.py validate-distribution-package \
    csv-loader-1.0.5-linux-x86_64.zip \
    --checksum-file csv-loader-1.0.5-linux-x86_64.zip.sha256
```

#### generate-release-notes

Generate release notes for only the plugin represented by a monorepo tag.

```bash
python3 scripts/release_tools.py generate-release-notes \
    --release-tag "data_load_csv/v1.0.5" \
    --output release-notes.md
```

By default the command diffs against the previous tag for the same source directory. Use `--previous-tag` to override that range when repairing or backfilling a release, and `--include-shared` only when shared build/runtime changes should be called out explicitly.

---

## Local registry (offline testing)

`build_local_registry.py` is the offline counterpart of `submit_to_registry.py`: it packages the
plugin `.so`s from a local build directory into ZIPs and writes a `registry.json` whose platform
URLs are `file://` links to them. The PlotJuggler Marketplace accepts the `file` scheme, so you can
exercise install / update / uninstall end-to-end **without cutting a release**. It shares the
packaging and registry-entry logic of the release path, so the ZIP layout (`<extension_id>/` at the
archive root) matches a real release.

### Steps to reproduce a local registry

```bash
# 1. Build the plugins to serve (produces build/.../bin/*.so).
./build.sh

# 2. Generate the registry (default: all plugins with a manifest.json, host platform only,
#    written to build/local-registry/registry.json).
python3 scripts/build_local_registry.py

# 3. Set the Marketplace registry URL to file:///abs/path/to/build/local-registry/registry.json
#    (the script prints it).
```

Verify the result with the same schema validator the CI uses (skip the URL check since the URLs are
local `file://`):

```bash
python3 /path/to/pj-plugin-registry/scripts/validate_registry.py --skip-urls \
    build/local-registry/registry.json
```

### Selecting plugins and output location

```bash
# Specific plugins (source dir or extension id), custom output path
python3 scripts/build_local_registry.py csv-loader data_load_parquet \
    --output /tmp/registry.json
```

### Multiple platforms

By default only the host platform is packaged (`--host-platform`, default `linux-x86_64`) from
`--build-dir`. Add other platforms (e.g. a Windows build from another machine) with the repeatable
`--platform NAME=BUILDDIR` flag:

```bash
python3 scripts/build_local_registry.py --platform windows-x86_64="$WINDOWS_BUILD_DIR"
```

A platform whose build directory is missing or that has no matching binary is skipped with a
warning; coverage against the full platform set is reported at the end.

### CLI reference

```bash
python3 scripts/build_local_registry.py [plugins...] [options]

Arguments:
  plugins                  Source dirs or extension ids to include (default: all with a manifest.json)

Options:
  --build-dir PATH         Host build directory to package binaries from (default: build)
  --host-platform NAME     Registry platform key for --build-dir (default: linux-x86_64)
  --platform NAME=BUILDDIR Additional platform from another build dir (repeatable)
  --output PATH            registry.json path (default: <build-dir>/local-registry/registry.json)
  --zip-dir PATH           Directory for the ZIP artifacts (default: <output-dir>/artifacts)
  --keep-staging           Keep the intermediate staging tree instead of deleting it
```

---

## Prerequisites

```bash
pip install -r scripts/requirements.txt
```

Required:
- Python 3.10+
- GitPython for `release_extension.py` (`pip install GitPython`)
- GitHub CLI (`gh`) - authenticated with appropriate permissions

---

## Troubleshooting

### "No plugin found with extension id"

The tool searches for plugins by loading each `.so/.dll/.dylib` and checking its embedded manifest.

- Verify the plugin was compiled
- Check that the extension id in `manifest.json` matches what's compiled into the plugin
- Use `extract-embedded-manifest` to inspect what's in the plugin

### "Version mismatch"

The version in the tag, manifest.json, or plugin don't match.

- Update `manifest.json` with the correct version
- Recompile if the plugin has wrong version
- Delete and recreate the tag if it points to wrong commit:
  ```bash
  git tag -d source_dir/v1.0.5
  git push origin :refs/tags/source_dir/v1.0.5
  ```

### "Checksum mismatch"

The extension ZIP doesn't match its `.sha256` file.

- Re-download the file
- Check for corruption during transfer
- Verify the checksum file corresponds to this exact ZIP

### "Tag already exists at different commit"

You probably forgot to update the version before creating a new release.

- Update the version in `manifest.json` to a new value
- Or delete the existing tag if it was created by mistake

### "Registry PR validation failed"

Check the PR's CI logs. Common issues:

- Missing artifacts for some platforms (wait for all CI builds)
- Checksum mismatch (artifacts were rebuilt, registry has old checksums)
- Invalid JSON structure in registry entry
