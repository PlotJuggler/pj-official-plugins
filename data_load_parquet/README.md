# Parquet Data Loader

Imports [Apache Parquet](https://parquet.apache.org/) files, extracting
numeric columns with optional timestamp detection.

## Supported Types

- All Arrow numeric types: bool, int8-int64, uint8-uint64, float, double
- Arrow TIMESTAMP with automatic unit conversion (second/milli/micro/nano)
- Native type preservation via ValueRef (no precision loss from double cast)

## Timestamp Modes

- **Row index** — synthetic timestamps from row numbers
- **Column selection** — pick a column as the time axis
- **Auto-detection** — heuristic search for columns named `timestamp`, `time`, `ts`, etc.
- Optional custom date format parsing

## Known Limitations

- Timezone offset handling not yet supported (non-UTC timestamps used as-is)
- Rows not sorted by timestamp within batches (may produce non-monotonic time)
- Column selection history not preserved across sessions
