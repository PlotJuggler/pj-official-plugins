# Changelog — toolbox_transform_editor

One entry per released version (newest first). Introduced at the version
below; for earlier releases see the git history of `toolbox_transform_editor/`.

## Unreleased

- Show on-demand JSON reports in the main preview pane; disable scalar Create while previewing this recipe kind.
- Poll on-demand previews once per GUI tick without sleeping. Cancel and release pending previews when the script changes, expires, or the editor closes.

## [1.1.0] - Unreleased

### Added
- Single Function tab: GLOBAL code starting with `-- pj-kind: on_demand`
  previews the FUNCTION body as an on-demand recipe (once, at the playhead;
  report JSON instead of a chart). Create/Modify not yet wired for this kind.

## [1.0.4] - 2026-08-04
