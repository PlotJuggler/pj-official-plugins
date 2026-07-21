# data_stream_lsl

Streaming DataSource plugin for [Lab Streaming Layer (LSL)](https://labstreaminglayer.org/).

## What it does

Discovers LSL streams on the local network and ingests the selected ones into
PlotJuggler in real time. Numeric streams become numeric plot series (in their
native type); string "Markers" streams become string series for event
annotations.

## Usage

1. Start PlotJuggler, choose **Lab Streaming Layer** under Streaming, click Start.
2. The dialog lists discovered streams (Name / Type / Channels / Rate / Source ID),
   refreshed once per second. Select one or more (or **Select All**).
3. Choose a **Timestamp Source**:
   - **Synchronized** (default): LSL sample stamps are time-corrected and mapped to
     absolute epoch time, so multiple streams share one aligned timeline.
   - **Raw LSL timestamp**: forwards the raw stamp (sender clock).
   - **Receiver clock**: timestamps samples on arrival.
4. Click OK.

## Testing

`test_scripts/lsl_publisher.py` (needs `pip install pylsl`) publishes a 4-channel
numeric EEG stream and a string marker stream.

## Dependencies

- `liblsl/1.16.2` (Conan), linked statically.

## Known limitations (first version)

- No reconnection: a stream that is offline at start or drops mid-session is
  skipped (re-open the source to pick it up again).
- Two streams with the identical `name` are selected together.
- Each poll drains an inlet's full backlog (bounded by liblsl's 360 s buffer);
  after a long stall one stream's catch-up can briefly delay the others. A
  per-poll sample cap is a planned follow-up.
