# Third-party notices — CAN Bus Streamer

This package ships the following Qt libraries, unmodified, as separate shared libraries next to the
plugin. They are licensed under the GNU Lesser General Public License version 3 (LGPLv3), which
supplements the GNU General Public License version 3 (GPLv3). Both texts are included in this
package:

- [`licenses/LGPL-3.0.txt`](licenses/LGPL-3.0.txt) (also at https://www.gnu.org/licenses/lgpl-3.0.txt)
- [`licenses/GPL-3.0.txt`](licenses/GPL-3.0.txt) (also at https://www.gnu.org/licenses/gpl-3.0.txt)

| Component | Files |
|-----------|-------|
| Qt Serial Bus | `Qt6SerialBus` library, and the CAN backends under `canbus/` |
| Qt Serial Port | `Qt6SerialPort` library |

Copyright (C) The Qt Company Ltd. and other contributors.

The complete source code of these libraries is available at
https://download.qt.io/official_releases/qt/ (the version matching the `Qt6SerialBus` file shipped
here). You may replace the shipped libraries with your own build of the same Qt version: the plugin
loads them by file name from its own folder.
