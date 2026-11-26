# Parquet Data Loader

Imports [Apache Parquet](https://parquet.apache.org/) files into a single topic named after the file; every supported
column becomes a series.

## Supported Types

- Arrow numeric types: bool, int8-int64, uint8-uint64, float, double. Values keep their native type.
- Compression: snappy, lz4 and zstd. Other codecs (e.g. brotli) fail with Arrow's error.

## Time Axis

The time column is the one picked in the dialog (restored by name for the next file), else the auto-detected one (the
first TIMESTAMP column, else a column named `timestamp`, `time`, `t`, `ts`, ...), else the row index at one second per
row. The Row index option still uses a detected timestamp column if the file has one.

Time column types: TIMESTAMP, TIME, DURATION and DATE are unit-converted; integers are read as nanoseconds and floats as
seconds. Timestamps with timezone metadata (e.g. `America/New_York`) are shifted by the zone's UTC offset, as in
PlotJuggler 3.

## Limits

- Rows are sorted by timestamp within each record batch only, so time can be non-monotonic across batches.
