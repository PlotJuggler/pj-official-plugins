# CSV Data Loader

Imports CSV files with automatic delimiter detection, type inference, and
flexible timestamp handling.

## Supported Formats

- CSV with comma, semicolon, tab, or space delimiters (auto-detected)
- Quoted fields with proper escape handling
- Mixed numeric and string columns

## Timestamp Modes

- **Row index** — synthetic timestamps from row numbers
- **Single column** — select a column containing timestamps (auto-detects
  epoch seconds/millis/micros/nanos, ISO 8601, and custom date formats)
- **Combined date + time** — merge two adjacent date-only and time-only columns

## Configuration

The dialog allows selecting delimiter, timestamp mode, and time column.
Custom date format strings use Qt-style codes (`yyyy-MM-dd hh:mm:ss`).

## Known Limitations

- DateTimeHelp dialog not yet available (requires SDK sub-dialog support)
- Column selection history not preserved across sessions
