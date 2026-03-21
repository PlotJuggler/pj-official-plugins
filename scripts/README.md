# Extension Release Scripts

Scripts for releasing PlotJuggler extensions and submitting them to the extension registry.

## Terminology

| Term | Definition |
|------|------------|
| **extension** | The distributable package (ZIP containing plugin + manifest.json) |
| **plugin** | The compiled binary (.so/.dll/.dylib) containing the C++ class |
| **source_dir** | The source directory containing code and manifest.json |

## Overview

This folder contains tools for the complete extension release workflow:

```
┌─────────────────────┐     ┌─────────────────────┐     ┌─────────────────────┐
│  1. Update version  │     │  2. Create release  │     │  3. Submit to       │
│     in manifest     │────►│     tag + push      │────►│     registry        │
│                     │     │                     │     │                     │
│  manifest.json      │     │  release_plugin.py  │     │  submit_to_registry │
└─────────────────────┘     └─────────────────────┘     └─────────────────────┘
                                    │                           │
                                    ▼                           ▼
                             CI builds extension         PR on pj-plugin-registry
                             artifacts and creates
                             GitHub release
```

## File Structure

| File | Purpose |
|------|---------|
| `release_plugin.py` | Main script: creates and pushes release tags |
| `submit_to_registry.py` | Main script: submits extensions to registry |
| `release_tools.py` | Library + CLI: validation and packaging utilities |
| `requirements.txt` | Python dependencies |

---

## Main Scripts

### `release_plugin.py`

Creates and pushes a release tag to trigger CI builds.

```bash
# By source directory name
python3 scripts/release_plugin.py data_load_csv

# By extension id (manifest id)
python3 scripts/release_plugin.py csv-loader

# Dry run (show what would be done)
python3 scripts/release_plugin.py csv-loader --dry-run

# Specify remote
python3 scripts/release_plugin.py csv-loader --remote plotjuggler
```

### `submit_to_registry.py`

Submits an extension release to the pj-plugin-registry by creating a PR.

```bash
# Submit latest release
python3 scripts/submit_to_registry.py csv-loader

# Submit specific version
python3 scripts/submit_to_registry.py csv-loader --version 1.0.5

# List available releases
python3 scripts/submit_to_registry.py csv-loader --list-releases

# Dry run (show registry entry without creating PR)
python3 scripts/submit_to_registry.py csv-loader --dry-run
```

---

## Release Tools CLI

`release_tools.py` provides both a Python library and a CLI with subcommands for validation and packaging tasks. These are used by CI workflows and can also be used for debugging.

### verify-version-consistency

**Purpose:** Verify that the git tag, manifest.json, and compiled plugin all have matching versions.

**Rationale:** Before publishing a release, we must ensure version consistency across all sources. A mismatch could mean the wrong code was tagged, the manifest wasn't updated, or the plugin was compiled from the wrong source.

**Usage:**
```bash
# Using release tag (recommended for CI)
python3 scripts/release_tools.py verify-version-consistency \
    --release-tag "data_load_csv/v1.0.5" \
    --build-dir build/Release \
    --check-manifest

# Using source directory name directly
python3 scripts/release_tools.py verify-version-consistency data_load_csv

# Full verification with explicit expected version
python3 scripts/release_tools.py verify-version-consistency data_load_csv \
    --build-dir build/Release \
    --expected-version 1.0.5 \
    --check-manifest
```

**Output:**
```
Source directory: data_load_csv
Extension id:     csv-loader
Manifest version: 1.0.5
Expected version: 1.0.5

Searching for plugin binary with id 'csv-loader' in build/Release...
Found plugin: build/Release/bin/libcsv_source_plugin.so
Plugin version:   1.0.5

OK: All versions match

PASSED: All checks successful
```

**Used by:** CI workflow after building, before packaging.

---

### create-distribution-package

**Purpose:** Create the distribution folder structure (plugin + manifest.json) ready for ZIP compression.

**Rationale:** The plugin filename doesn't match the extension id (e.g., `libcsv_source_plugin.so` vs `csv-loader`). This tool finds the correct plugin by loading each `.so/.dll/.dylib` and checking its embedded manifest id, eliminating the need for hardcoded filename mappings.

**Usage:**
```bash
# Using release tag (recommended for CI)
python3 scripts/release_tools.py create-distribution-package \
    --release-tag "data_load_csv/v1.0.5" \
    --build-dir build/Release \
    --output-dir dist \
    --os-label linux \
    --arch x86_64

# Using source directory name directly
python3 scripts/release_tools.py create-distribution-package data_load_csv \
    --build-dir build/Release \
    --output-dir dist \
    --version 1.0.5 \
    --os-label linux \
    --arch x86_64
```

**Output (stderr):**
```
Searching for plugin with id 'csv-loader' in build/Release...
Found plugin: build/Release/bin/libcsv_source_plugin.so
Copied plugin to: dist/csv-loader/libcsv_source_plugin.so
Copied manifest to: dist/csv-loader/manifest.json
```

**Output (stdout):** The extension ZIP filename for CI to capture:
```
csv-loader-1.0.5-linux-x86_64.zip
```

**Used by:** CI workflow for packaging extension artifacts.

---

### extract-embedded-manifest

**Purpose:** Extract and display the JSON manifest embedded inside a compiled plugin.

**Rationale:** Plugin binaries have their manifest compiled in via the `PJ_DATA_SOURCE_PLUGIN` or `PJ_MESSAGE_PARSER_PLUGIN` macros. This tool loads the plugin via ctypes and extracts that embedded manifest, useful for debugging version issues or verifying what was actually compiled.

**Usage:**
```bash
python3 scripts/release_tools.py extract-embedded-manifest build/Release/bin/libcsv_source_plugin.so
```

**Output:**
```json
{
  "id": "csv-loader",
  "name": "CSV Loader",
  "version": "1.0.5",
  "description": "Load CSV files into PlotJuggler",
  "author": "PlotJuggler Team",
  "category": "data_loader"
}
```

**Used by:** Developers for debugging.

---

### validate-manifest

**Purpose:** Validate that a manifest.json file has correct structure and required fields.

**Rationale:** Catch manifest errors early before they cause build or release failures. Validates required fields (id, name, version), semver format, and valid category values.

**Usage:**
```bash
python3 scripts/release_tools.py validate-manifest data_load_csv/manifest.json
```

**Output (success):**
```
OK: data_load_csv/manifest.json is valid
  id:      csv-loader
  name:    CSV Loader
  version: 1.0.5
```

**Output (error):**
```
Validation errors in data_load_csv/manifest.json:
  - Missing required field: version
  - Invalid category: invalid_category (valid: ['data_loader', 'data_stream', ...])
```

**Used by:** `release_plugin.py` before creating tags, CI for validation.

---

### validate-distribution-package

**Purpose:** Comprehensively validate a distribution ZIP package (extension).

**Rationale:** Before publishing or installing an extension, verify its integrity:
1. **Filename format** - Ensures the ZIP follows naming convention
2. **SHA256 checksum** - Verifies file integrity (if checksum file provided)
3. **Contents** - Confirms ZIP contains both plugin and manifest.json
4. **Manifest consistency** - The plugin's embedded manifest must match the included manifest.json
5. **Filename vs content** - Version and extension ID in filename must match manifest

**Usage:**
```bash
# Basic validation
python3 scripts/release_tools.py validate-distribution-package \
    csv-loader-1.0.5-linux-x86_64.zip

# With checksum verification
python3 scripts/release_tools.py validate-distribution-package \
    csv-loader-1.0.5-linux-x86_64.zip \
    --checksum-file csv-loader-1.0.5-linux-x86_64.zip.sha256
```

**Output:**
```
Validating: csv-loader-1.0.5-linux-x86_64.zip

Filename parsing:
  Extension: csv-loader
  Version:   1.0.5
  Platform:  linux-x86_64
  OK: Filename format valid

Checksum verification:
  Expected: a1b2c3d4e5f6...
  Actual:   a1b2c3d4e5f6...
  OK: SHA256 matches

Extension contents:
  Plugin:   csv-loader/libcsv_source_plugin.so
  Manifest: csv-loader/manifest.json

Manifest consistency:
  File manifest:   id=csv-loader, version=1.0.5
  Plugin manifest: id=csv-loader, version=1.0.5
  OK: Manifests match

Filename vs content:
  Filename version:      1.0.5
  Manifest version:      1.0.5
  Filename extension id: csv-loader
  Manifest id:           csv-loader
  OK: Filename matches content

PASSED: All validations successful
```

**Used by:** CI after packaging, users before installing downloaded extensions.

---

## Complete Release Example

```bash
# 1. Update version in manifest
vim data_load_csv/manifest.json
# Change "version": "1.0.5" to "1.0.6"

# 2. Commit the version bump
git add data_load_csv/manifest.json
git commit -m "Bump csv-loader to v1.0.6"
git push

# 3. Create and push release tag
python3 scripts/release_plugin.py csv-loader
# Output:
#   Source: data_load_csv
#   Version: 1.0.6
#   Tag: data_load_csv/v1.0.6
#   ✓ Release tag 'data_load_csv/v1.0.6' created and pushed!

# 4. Wait for CI to build extension artifacts (check GitHub Actions)

# 5. (Optional) Download and verify extension locally
python3 scripts/release_tools.py validate-distribution-package \
    csv-loader-1.0.6-linux-x86_64.zip \
    --checksum-file csv-loader-1.0.6-linux-x86_64.zip.sha256

# 6. Submit to registry
python3 scripts/submit_to_registry.py csv-loader
# Output:
#   ✓ PR created: https://github.com/PlotJuggler/pj-plugin-registry/pull/123
```

---

## CI Integration

The CI workflow (`.github/workflows/build-release.yml`) uses these tools. The `--release-tag` argument extracts both the source directory name and version from the tag, eliminating the need for bash string parsing:

```yaml
# After building, verify version consistency
- name: Verify plugin version
  if: startsWith(github.ref, 'refs/tags/')
  run: |
    python3 scripts/release_tools.py verify-version-consistency \
      --release-tag "${{ github.ref_name }}" \
      --build-dir build/Release \
      --check-manifest

# Create distribution package (extension)
- name: Package artifacts
  if: startsWith(github.ref, 'refs/tags/')
  run: |
    ZIP_NAME=$(python3 scripts/release_tools.py create-distribution-package \
      --release-tag "${{ github.ref_name }}" \
      --build-dir build/Release \
      --output-dir dist \
      --os-label "${{ matrix.os_label }}" \
      --arch "${{ matrix.arch }}")
    echo "ZIP_NAME=${ZIP_NAME}" >> $GITHUB_ENV
```

The `--release-tag` argument accepts tags in the format `{source_dir}/v{version}` (e.g., `data_load_csv/v1.0.5`) and internally extracts:
- Source directory name: `data_load_csv`
- Version: `1.0.5`

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

The tool searches for plugins by loading each `.so/.dll/.dylib` and checking its embedded manifest. If no match is found:
- Verify the plugin was compiled
- Check that the extension id in `manifest.json` matches what's compiled into the plugin
- Use `extract-embedded-manifest` to inspect what's in the plugin

### "Version mismatch"

The version in the tag, manifest.json, or plugin don't match:
- Update `manifest.json` with the correct version
- Recompile if the plugin has wrong version
- Delete and recreate the tag if it points to wrong commit

### "Checksum mismatch"

The extension ZIP file doesn't match its `.sha256` file:
- Re-download the file
- Check for corruption during transfer
- Verify the checksum file corresponds to this exact ZIP
