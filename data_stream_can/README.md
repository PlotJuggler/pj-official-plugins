# CAN Bus Streamer

Streams one live CAN / CAN FD interface into PlotJuggler through
[Qt Serial Bus](https://doc.qt.io/qt-6/qtcanbus-backends.html) and decodes the
frames to timeseries with a `.dbc` file or an ARUS-style CSV dictionary. Signal
decoding is the shared `pj_can_dbc` library
([`dbc_parser_cpp`](https://github.com/LinuxDevon/dbc_parser_cpp), MIT), the same
decoder as the candump, MF4 and BLF loaders, so a live stream and a recorded
`candump` capture of the same bus produce the same topics.

This is the one non-widget plugin that links Qt (Core + SerialBus). It ships
`Qt6SerialBus`, `Qt6SerialPort` and the `canbus/` backends next to the plugin
library, because PlotJuggler's own Qt has none of them.

## Backends

The backend list in the dialog is whatever Qt finds in the bundled `canbus/`
folder. Hardware backends also need the vendor's driver installed on the
machine: PCAN-Basic, Vector XL Driver Library, SYS TEC USB-CANmodul driver,
Tiny CAN API, or the adapter's J2534 PassThru DLL.

| Backend key | Hardware | Linux | Windows |
|-------------|----------|:-----:|:-------:|
| `socketcan` | Any SocketCAN interface (`can0`, `vcan0`, `slcan0`, ...) | yes | — |
| `peakcan` | PEAK-System PCAN adapters (PCAN-Basic) | yes | yes |
| `vectorcan` | Vector adapters (XL Driver Library) | — | yes |
| `systeccan` | SYS TEC USB-CANmodul | — | yes |
| `tinycan` | MHS Tiny-CAN | yes | yes |
| `passthrucan` | SAE J2534 PassThru adapters | yes | yes |
| `virtualcan` | Qt's software bus over TCP, no hardware (see Limitations) | yes | yes |

A missing vendor driver shows up twice: in the dialog's status line when the
backend's interfaces are listed (Qt's error, e.g. `peakcan: Cannot load library
pcanbasic`), and as the start error of the stream (`<backend>/<interface>: ...`).

## Features

- Pick backend, interface (detected, or typed), bitrate and, with CAN FD on, the
  data-phase bitrate (2, 4, 5 or 8 Mbit/s). "Keep device setting" means:
  - **SocketCAN**: the interface's configured rate. A SocketCAN interface is
    configured outside PlotJuggler (`ip link set can0 type can bitrate 500000`,
    with the interface down, as root / `CAP_NET_ADMIN`), so "Keep device
    setting" is the only choice there: any other bitrate or data bitrate fails
    the start with that hint. (Qt would only log its failure to apply the rate
    and stream at the interface's own rate.)
  - **Vendor backends** (PEAK, Vector, ...): Qt opens the adapter with the
    backend's default rate (PEAK: 500 kbit/s), not the rate a previous
    application left. Choose the bus's rate explicitly there. A rate the adapter
    rejects, while configuring or while connecting, fails the start.
- **Dictionaries**: any number of `.dbc` files and ARUS-style CSV dictionaries
  (`ID,bitIn,bitFin,Signed,Power,Scale,Offset,Name`, see
  [data_load_candump](../data_load_candump/README.md#arus-csv-dictionary)), all
  applied to the one interface. A file that fails to load is a warning, not a
  failure: the stream starts and the raw fallback still works.
- **Raw fallback** (on by default): frames no dictionary decodes are published
  as raw bytes.
- Acquisition runs on its own thread; frames are handed over to PlotJuggler's
  poll and appended directly (no message parser in between).
- A lost adapter or bus reports an error and stops the stream; frames received
  before it are kept. How it is detected depends on the backend:
  - **SocketCAN**: the interface going down or disappearing (`ip link set down`,
    a USB adapter unplugged), checked every 500 ms. Qt's backend itself reports
    neither.
  - **VirtualCAN**: the loss of its TCP connection to the server.
  - **Vendor backends**: only when the driver raises errors: a `ConnectionError`,
    ten consecutive read errors with no frame in between, or the device leaving
    the connected state. A driver that goes silent without an error is not
    detected.
- Stopping the stream reports a summary: frames decoded, message topics
  created, unmatched / undecodable / no-dictionary frames, error frames and
  remote frames.

## Topic naming

Identical to the [candump loader](../data_load_candump/README.md):

- Decoded: one topic per message, `CAN/<interface>/<Message>`, one field per
  signal. A signal with a DBC `VAL_` table also gets a `<signal>_label` text
  field.
- Raw: `CAN/<interface>/0x<ID>` (upper-case hex, no padding), fields
  `byte0..byteN`.

`/` and whitespace in the interface name become `_`. Error frames and remote
(RTR) frames are counted, not published.

## Timestamps

| `timestamps` | Behavior |
|--------------|----------|
| `device` (default, "Adapter clock") | The adapter's own frame time. An absolute stamp (SocketCAN kernel time) is used as is; a relative clock (PEAK, Vector count from driver start) is anchored to wall-clock time at the first frame and re-anchored after a backwards jump of more than 1 s (replug, driver restart). A backend that stamps nothing gets the arrival time. |
| `host` ("Arrival time") | When PlotJuggler received the frame. For adapters whose clock drifts. |

## Configuration

```json
{
  "backend": "peakcan",
  "interface": "usb0",
  "bitrate": 500000,
  "can_fd": false,
  "data_bitrate": 0,
  "timestamps": "device",
  "dictionaries": ["/path/to/powertrain.dbc", "/path/to/dictionary.csv"],
  "raw_unassigned": true
}
```

Every key is optional; a missing or wrongly-typed one keeps its default.
`bitrate` / `data_bitrate`: bit/s, `0` keeps the device's setting
(`data_bitrate` applies only with `can_fd`; both must be `0` on `socketcan`). Dictionary paths are stored as
given: a stream has no data file for them to be relative to. A saved backend not
installed on this machine is kept in the list, so the layout survives a round
trip through another computer.

## Live test without hardware

On Linux, a virtual SocketCAN interface and `can-utils`:

```bash
sudo modprobe vcan
sudo ip link add dev vcan0 type vcan
sudo ip link set up vcan0
```

Start the stream on backend `socketcan`, interface `vcan0`, then:

```bash
cansend vcan0 123#DEADBEEF      # one frame -> CAN/vcan0/0x123 (raw) or the DBC message
cangen vcan0 -g 5               # random frames every 5 ms
sudo ip link set down vcan0     # while streaming: "interface went down" within 0.5 s, the stream stops
```

On any OS, the `virtualcan` backend needs no setup: start a stream on it, and
any other Qt application that opens a `virtualcan` device sends to it.

## Testing

Load the plugin from `build/data_stream_can/Release/dist/` (or the release zip),
not from the build tree: the plugin looks for the Qt Serial Bus libraries only
next to itself (RUNPATH `$ORIGIN` on Linux), so the build-tree copy cannot load
on its own.

```bash
./build.sh data_stream_can
ctest --test-dir build/data_stream_can/Release --output-on-failure
```

The tests drive the reader and the built plugin end to end over VirtualCAN.
They must run serially (they are `RUN_SERIAL`): VirtualCAN listens on fixed TCP
ports.

## Limitations

- **Qt 6.12 hosts only.** The plugin is built against the Qt in `QT_VERSION` and
  loads only into a PlotJuggler built with the same Qt major.minor. There is no
  version check for this family (widgets have one): a mismatch fails at load
  time.
- One interface per source. Add another source for a second bus.
- Receive only: nothing is sent on the bus.
- A standard (11-bit) and an extended (29-bit) frame with the same numeric ID
  share the raw topic `CAN/<interface>/0x<ID>`, as in the candump loader. A
  follow-up will tell them apart.
- Multiplexed signals are not decoded, and neither is a CAN FD payload longer
  than 8 bytes (both `dbc_parser_cpp` limitations, shared with the file
  loaders). Such a CAN FD frame is counted undecodable and still reaches the
  raw fallback.
- No macOS build (Linux x86_64 and Windows x64 only, where CI installs the
  official Qt).
- **VirtualCAN**: its server is one process-wide Qt object, bound to the thread
  of the first opener (the plugin starts it on PlotJuggler's main thread) and
  listening on the fixed TCP port 35468. A second process (another PlotJuggler,
  a test run) that wants to serve VirtualCAN at the same time cannot; its
  devices connect to the first process's server instead.
