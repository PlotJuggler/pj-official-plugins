# Plugin Release Scripts

Scripts for releasing plugins and submitting them to the PlotJuggler plugin registry.

## Workflow Overview

```
┌─────────────────────┐     ┌─────────────────────┐     ┌─────────────────────┐
│  1. Update version  │     │  2. Create release  │     │  3. Submit to       │
│     in manifest     │────►│     tag + push      │────►│     registry        │
│                     │     │                     │     │                     │
│  manifest.json      │     │  release_plugin.py  │     │  submit_to_registry │
└─────────────────────┘     └─────────────────────┘     └─────────────────────┘
                                     │                           │
                                     ▼                           ▼
                              CI builds artifacts         PR on pj-plugin-registry
                              and creates GitHub
                              release
```

## Prerequisites

```bash
pip install -r scripts/requirements.txt
```

Required tools:
- Python 3.10+
- GitPython (`pip install GitPython`)
- GitHub CLI (`gh`) - authenticated with appropriate permissions

## Scripts

### `release_plugin.py`

Creates and pushes a release tag to trigger CI builds.

#### Usage

```bash
# By directory name
python3 scripts/release_plugin.py data_load_csv

# By manifest id
python3 scripts/release_plugin.py csv-loader

# Dry run (show what would be done)
python3 scripts/release_plugin.py csv-loader --dry-run

# Specify remote
python3 scripts/release_plugin.py csv-loader --remote plotjuggler

# With GitHub token
python3 scripts/release_plugin.py csv-loader --token ghp_xxxx
# Or via environment variable
export GITHUB_TOKEN=ghp_xxxx
python3 scripts/release_plugin.py csv-loader
```

#### Features

| Feature | Description |
|---------|-------------|
| Plugin lookup | Accepts directory name (`data_load_csv`) or manifest id (`csv-loader`) |
| Version from manifest | Reads version from `manifest.json` |
| Tag format | `{plugin_dir}/v{version}` (e.g., `data_load_csv/v1.0.5`) |
| Remote auto-detection | Finds GitHub remote matching `pj-official-plugins` |
| Local tag check | Verifies tag doesn't already exist at different commit |
| Remote tag check | Verifies tag doesn't already exist on remote |
| Token authentication | Supports `--token` or `GITHUB_TOKEN` env var for HTTPS push |
| Dry run | `--dry-run` shows actions without executing |

#### Options

| Option | Description |
|--------|-------------|
| `plugin` | Plugin directory or manifest id (required) |
| `--dry-run` | Show what would be done without making changes |
| `--remote NAME` | Git remote to push to (default: auto-detect) |
| `--token TOKEN` | GitHub token for authentication |

---

### `submit_to_registry.py`

Submits a plugin release to the pj-plugin-registry by creating a PR.

#### Usage

```bash
# Submit latest release
python3 scripts/submit_to_registry.py csv-loader

# Submit specific version
python3 scripts/submit_to_registry.py csv-loader --version 1.0.5

# List available releases
python3 scripts/submit_to_registry.py csv-loader --list-releases

# Dry run (show registry entry without creating PR)
python3 scripts/submit_to_registry.py csv-loader --dry-run

# Skip checksum verification (faster, less safe)
python3 scripts/submit_to_registry.py csv-loader --skip-checksum-verify

# Use different releases repository
python3 scripts/submit_to_registry.py csv-loader --releases-repo myorg/my-plugins
```

#### Features

| Feature | Description |
|---------|-------------|
| Plugin lookup | Accepts directory name or manifest id |
| Version selection | Latest from GitHub or specify with `--version` |
| Release listing | `--list-releases` shows all available versions |
| Asset discovery | Finds platform artifacts from GitHub release |
| Checksum verification | Downloads and verifies SHA256 checksums |
| Platform normalization | Maps CI arch names to registry format (`aarch64` → `arm64`) |
| Registry entry generation | Builds complete entry from manifest + assets |
| PR creation | Creates branch and PR directly on registry repo |
| Dry run | Shows generated registry entry without creating PR |

#### Options

| Option | Description |
|--------|-------------|
| `plugin` | Plugin directory or manifest id (required) |
| `--version`, `-v` | Version to submit (default: latest from GitHub) |
| `--list-releases`, `-l` | List available releases and exit |
| `--dry-run` | Print registry entry without creating PR |
| `--skip-checksum-verify` | Skip downloading and verifying checksums |
| `--releases-repo` | GitHub repo for fetching releases |

#### Supported Platforms

| Platform Key | Description |
|--------------|-------------|
| `linux-x86_64` | Linux 64-bit Intel/AMD |
| `linux-arm64` | Linux 64-bit ARM |
| `macos-x86_64` | macOS Intel |
| `macos-arm64` | macOS Apple Silicon |
| `windows-x86_64` | Windows 64-bit Intel/AMD |
| `windows-arm64` | Windows 64-bit ARM |

## Complete Example

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
#   Plugin: data_load_csv
#   Version: 1.0.6
#   Tag: data_load_csv/v1.0.6
#   ...
#   ✓ Release tag 'data_load_csv/v1.0.6' created and pushed!

# 4. Wait for CI to build artifacts (check GitHub Actions)

# 5. Submit to registry
python3 scripts/submit_to_registry.py csv-loader
# Output:
#   Plugin: data_load_csv
#   ...
#   ✓ PR created: https://github.com/PlotJuggler/pj-plugin-registry/pull/123
```

## Registry Entry Format

The generated registry entry follows this format:

```json
{
  "id": "csv-loader",
  "name": "CSV Loader",
  "version": "1.0.6",
  "description": "Load CSV files into PlotJuggler",
  "author": "PlotJuggler",
  "category": "data-loader",
  "platforms": {
    "linux-x86_64": {
      "url": "https://github.com/.../csv-loader-1.0.6-linux-x86_64.zip",
      "checksum": "sha256:abc123..."
    }
  }
}
```

Note: Additional fields like `plugins` and `changelog` can be added manually to `manifest.json` if needed - they will be copied to the registry entry.

## Troubleshooting

### "No GitHub remote found"

The script couldn't find a remote URL matching `github.com/.../pj-official-plugins`. Use `--remote` to specify:

```bash
python3 scripts/release_plugin.py csv-loader --remote origin
```

### "Error pushing tag: 403"

You don't have write access to the repository. Either:
1. Request collaborator access from the repo owner
2. Use a GitHub token with `repo` scope: `--token ghp_xxxx`

### "Release not found on GitHub"

CI hasn't finished building yet. Check GitHub Actions and wait for completion.

### "Checksum mismatch"

The downloaded asset doesn't match its `.sha256` file. This could indicate:
- Corrupted upload
- Man-in-the-middle attack
- CI issue

Re-run CI or investigate the release.
