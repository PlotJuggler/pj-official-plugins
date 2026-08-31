# Mosaico toolbox

Browse and download sequences from a Mosaico server (Arrow Flight), and re-import
them from a saved layout.

## Layout re-import

Every Download is recorded into a local cache artifact (`<identity>.pjmosaico`)
and promoted to a file-backed source, so a saved layout embeds a **source
descriptor** — server origin, sequence, topics, time window; never a credential.
Re-opening the layout restores instantly from the cache on the same machine, and
re-downloads the exact request on a machine that lacks the artifact (after the
host's confirmation, unless the origin is trusted).

An explicit Download always fetches fresh data: it re-materializes the cache
artifact when nothing is using it; an artifact in use by an open dataset is
reused only for a bounded time window (an open-ended window may have grown).

## Settings and environment

| Key / variable | Meaning |
|---|---|
| `mosaico/cache_directory` (panel field) | Cache root. Empty = `MOSAICO_CACHE_DIR`, else `$XDG_CACHE_HOME/mosaico/sessions` / `~/.cache/mosaico/sessions` (Linux, macOS) or `%LOCALAPPDATA%\mosaico\sessions` (Windows). |
| `mosaico/cache_max_gb` (advanced, no UI) | Cache budget in GiB, default 20; `<= 0` = unlimited. **Best-effort**: artifacts leased by open datasets are never evicted, so the budget binds only what is evictable; over-target is reported when the panel opens. |
| `MOSAICO_TRUSTED_ORIGINS` | Origins a layout may re-download from headlessly, e.g. `grpc+tls://host:6726` (comma, semicolon or whitespace separated). Anything else needs confirmation. |
| `MOSAICO_URL`, `MOSAICO_API_KEY` | Headless credentials: the key is released only to the exact origin of `MOSAICO_URL`. |
| `MOSAICO_IMPORT_MAX_BYTES`, `MOSAICO_IMPORT_MAX_SECONDS` | Per-machine ceilings for a headless import (defaults 32 GiB, 1 h). |

A cache artifact whose body fails to replay is quarantined as `<artifact>.corrupt`
so the next layout open re-downloads it.
