# Plugin-owned display localization (work in progress)

The host sets `PLOTJUGGLER_UI_LANGUAGE` to `en_US`, `zh_CN`, or `zh_TW` before
loading plugin images. Unknown/unset values use English. The decision is cached
at plugin initialization; changing it requires restarting the host. This affects
presentation only, never the numeric locale, data/configuration keys, user data,
file paths, prompts, or Assistant canonical stored messages. Some MCAP/Protobuf
error strings returned through the SDK are presentation diagnostics and are
localized; consumers must not parse their English wording as stable identifiers.

Each module owns `locales/zh_CN.json` and `zh_TW.json`. The shared EmbedUi helper
embeds three XML resources, with no Qt dependency in plugins. The English XML is
preserved byte-for-byte. Chinese variants alter only matching string properties
identified by UI path, widget, property and English source. `notr` strings are
not changed. Unknown/new source text stays English. Translations originate from
the stable localization catalogs at 133b9cf4c39067203e1ec1b5bd6d0d421c506c33.

## Current coverage and limits

- Catalogs: 16 modules, 24 UI resources, 549 exact occurrences, 62 templates and
  one guarded value. These counts describe catalog data, **not runtime coverage**.
- The current upstream snapshot has **285 matching static UI string occurrences**,
  not 549 applied translations. The other catalog entries include dynamic values
  and source text that changed upstream.
- The generator currently consumes exact static UI entries only. Templates and
  guarded runtime values are retained for review, not applied blindly to user
  values. Dynamic widget updates can still replace translated static labels
  with English until their owning call sites are adapted.
- Five modules have fixed runtime prose changes: CSV, MCAP, Protobuf, Assistant
  Agent and WebRTC. CSV changes deliberately exclude the separate UTF-8 path
  fix (#337). Canonical warning detail is retained alongside display text.
- Transform Editor has been substantially refactored upstream. Its former
  native patch is not applied; its help UI currently has no matching exact
  static string. Mosaico's deferred downstream native patch is not applied.
- No APBIN, marketplace names/descriptions, registry overrides, release
  packaging, or SDK version changes are part of this work.
- This currently wires language selection for native plugin images only. WASM
  instance-environment propagation is unverified; no WASM Chinese-support claim
  is made. SDK pin checks pass, but SDK feature-floor compatibility has not been
  validated against a complete SDK surface table.
- Full plugin builds, Windows execution and interactive host acceptance are
  still pending. This is not a claim of stable feature parity or a release.

## Focused checks

`python scripts/tests/test_localized_ui.py` exercises key isolation, `notr`,
placeholder rejection, all 24 source UI structures, catalog key parity and
compiled UTF-8 XML selection/fallback. It currently uses a local `c++` compiler.
`display_language_test.cpp` tests fixed prose, language fallback and preservation
of canonical data; optional `PJ_TEST_MCAP` and `PJ_TEST_ASSISTANT` build fixtures
against the actual modified helper/session headers. No compressed MCAP import
or complete plugin binary is exercised by those fixtures.
