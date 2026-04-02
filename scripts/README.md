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
| `release_tools.py` | Library + CLI for validation and packaging |
| `requirements.txt` | Python dependencies |

---

## Release Pipeline Overview

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
│                           - Upload to release                                       │
│                           - (Auto-submit if flag)                                   │
└─────────────────────────────────────────────────────────────────────────────────────┘
```

---

## Release Workflows

Choose the automation level that fits your needs:

### Option A: Fully Automatic (Recommended)

One command does everything: bumps version, commits, creates tag, CI builds all platforms, and automatically submits to the registry.

```bash
# Single command: bump version, commit, tag, push, auto-submit
python3 scripts/release_extension.py csv-loader --bump patch --submit-to-registry

# Or bump minor/major version
python3 scripts/release_extension.py csv-loader --bump minor --submit-to-registry
python3 scripts/release_extension.py csv-loader --bump major --submit-to-registry

# Or set explicit version
python3 scripts/release_extension.py csv-loader --version 2.0.0 --submit-to-registry

# Done! The script will:
#   - Update manifest.json with new version
#   - Commit the change
#   - Create annotated tag
#   - Push to GitHub (triggers CI)
#   - CI builds all 6 platforms
#   - CI creates GitHub release with artifacts
#   - CI automatically creates PR to registry
```

### Option B: Two-Step (Release + Submit Separately)

Useful when you want to verify CI builds before submitting to the registry.

```bash
# 1. Update version in manifest.json
vim data_load_csv/manifest.json

# 2. Commit and push
git add data_load_csv/manifest.json
git commit -m "Bump csv-loader to v1.0.6"
git push

# 3. Create release tag (triggers CI builds)
python3 scripts/release_extension.py csv-loader

# 4. Wait for CI to complete, verify builds succeeded

# 5. Submit to registry manually
python3 scripts/submit_to_registry.py csv-loader
```

### Option C: Fully Manual (Create Tags by Hand)

Maximum control. Useful for debugging or special releases.

```bash
# 1. Update version in manifest.json
vim data_load_csv/manifest.json

# 2. Commit and push
git add data_load_csv/manifest.json
git commit -m "Bump csv-loader to v1.0.6"
git push

# 3. Create and push tag manually
git tag -a "data_load_csv/v1.0.6" -m '{"extension_id":"csv-loader","version":"1.0.6"}'
git push origin "data_load_csv/v1.0.6"

# 4. Wait for CI to complete

# 5. Submit to registry
python3 scripts/submit_to_registry.py csv-loader --version 1.0.6
```

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
| macOS Apple Silicon | macos-14 | arm64 |
| Windows x64 | windows-latest | x64 |
| Windows ARM64 | windows-11-arm | arm64 |

**Per-platform steps:**

| Step | Description |
|------|-------------|
| Checkout | Clone repository at tagged commit |
| Install dependencies | Conan packages, cmake, ninja |
| Build | Compile only the tagged plugin (`-DPJ_BUILD_PLUGIN=source_dir`) |
| Test | Run plugin tests via ctest |
| **Verify version** | `release_tools.py verify-version-consistency` |
| Package | `release_tools.py create-distribution-package` |
| Compress | Create ZIP: `{ext_id}-{version}-{os}-{arch}.zip` |
| Generate checksum | SHA256 in `.sha256` file |
| Upload artifact | Store in GitHub Actions artifacts |
| Upload to release | Attach ZIP + checksum to GitHub Release |

**Post-build (if `--submit-to-registry` was used):**

| Step | Description |
|------|-------------|
| Check tag metadata | Read JSON from annotated tag message |
| Submit to registry | Run `submit_to_registry.py` if `auto_submit_to_registry: true` |

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
| Clone registry repo | Clones pj-plugin-registry |
| Update registry.json | Adds or updates extension entry |
| Create PR branch | `registry/{extension_id}-{version}` |
| Push and create PR | Opens PR on pj-plugin-registry |

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
  "artifacts": [
    {
      "platform": "linux-x86_64",
      "url": "https://github.com/.../csv-loader-1.0.6-linux-x86_64.zip",
      "sha256": "abc123..."
    },
    ...
  ]
}
```

---

### Stage 4: Registry Validation CI (`validate-registry.yml`)

Triggered on PRs to pj-plugin-registry that modify `registry.json`.

**Validations:**

| Check | Description |
|-------|-------------|
| JSON schema | Valid JSON structure |
| Required fields | All extensions have id, name, version, artifacts |
| URL format | All artifact URLs are valid HTTPS |
| Platform coverage | Warns if missing platforms |
| **Checksum verification** | Downloads each artifact and verifies SHA256 |

The checksum verification runs on every PR, ensuring artifacts haven't been tampered with or corrupted.

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

---

## Prerequisites

```bash
pip install -r scripts/requirements.txt
```

Required:
- Python 3.10+
- GitPython (`pip install GitPython`)
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
