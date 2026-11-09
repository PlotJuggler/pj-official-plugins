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

A missing vendor driver is reported when the stream starts, as Qt's error
prefixed with `<backend>/<interface>`, not when the dialog opens.

## Features

- Pick backend, interface (detected, or typed), bitrate and, with CAN FD on, the
  data-phase bitrate. "Keep device setting" leaves the adapter's configuration
  alone (SocketCAN interfaces are configured with `ip link`, not by the
  application).
- **Dictionaries**: any number of `.dbc` files and ARUS-style CSV dictionaries
  (`ID,bitIn,bitFin,Signed,Power,Scale,Offset,Name`, see
  [data_load_candump](../data_load_candump/README.md#arus-csv-dictionary)), all
  applied to the one interface. A file that fails to load is a warning, not a
  failure: the stream starts and the raw fallback still works.
- **Raw fallback** (on by default): frames no dictionary decodes are published
  as raw bytes.
- Acquisition runs on its own thread; frames are handed over to PlotJuggler's
  poll and appended directly (no message parser in between).
- A lost adapter or bus (`ConnectionError`, ten consecutive read errors with no
  frame in between, or the device leaving the connected state) reports an error
  and stops the stream. Frames received before it are kept.
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
  "backend": "socketcan",
  "interface": "can0",
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
(`data_bitrate` applies only with `can_fd`). Dictionary paths are stored as
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
sudo ip link set down vcan0     # while streaming: error message, the stream stops
```

On any OS, the `virtualcan` backend needs no setup: start a stream on it, and
any other Qt application that opens a `virtualcan` device sends to it.

## Testing

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
