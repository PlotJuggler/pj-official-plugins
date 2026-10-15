# Basic Widgets (`widget_basic`)

Four dockable widgets for live values, in one Widget-family library (`basic_widgets`):

| Type | Input | Settings |
|---|---|---|
| `gauge` | `value` | `min`, `max`, `unit`, `red_from`, `mode` (`arc` or `bar`) |
| `numeric_indicator` | `value` | `decimals`, `unit`, `label` |
| `traffic_light` | `value` | `green_below`, `red_from` |
| `value_table` | `rows` (any number of series) | `decimals` |

The user wires series to inputs in the host. Settings are declared in `manifest.json`; the host
draws the form and sends the full settings object to `loadConfig` on every change. A key that is
missing takes its manifest default, and a key of the wrong type is rejected.

Notes:

- `value_table` labels its rows by slot ("Series 1", "Series 2", ...): the host does not give a
  widget the name of the series behind a slot.
- Chrome (text, tracks, backgrounds) follows the palette. The green, amber and red of the status
  colours are fixed so they mean the same in a light and a dark theme.
- The widgets read nothing from the host while painting: `onUpdate` copies the values and
  schedules a repaint.

## Building

This is the only plugin that links Qt, and it must be built against the official Qt that matches
the host's major.minor (6.12), not a distribution or Conan package:

```bash
export QT_ROOT_DIR=/path/to/Qt/6.12.0/gcc_64
./build.sh widget_basic
ctest --test-dir build/widget_basic/Release
```

`QT_ROOT_DIR` is what `jurplel/install-qt-action` sets in CI. The library is built on Ubuntu 22.04
(Linux x86_64) and with MSVC 14.44 (Windows x64) only; the aggregate build skips it unless
`-DPJ_BUILD_WIDGETS=ON`. Tests run on the offscreen platform and load the library through
`PJ::WidgetLibrary`, as the host does.

The post-build gate checks the export allowlist and that no `STB_GNU_UNIQUE` symbol leaks. Qt is
allowed here and forbidden in every other plugin of the repository.
