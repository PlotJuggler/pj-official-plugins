# CSV Data Loader

Imports CSV files with automatic delimiter detection, type inference, and
flexible timestamp handling.

## Supported Formats

- CSV with comma, semicolon, tab, or space delimiters (auto-detected; a header
  with several of them asks the user to choose)
- Quoted fields with proper escape handling
- Mixed numeric and string columns

## Timestamp Modes

- **Row index**: synthetic timestamps from row numbers
- **Single column**: select a column containing timestamps (auto-detects
  epoch seconds/millis/micros/nanos, ISO 8601, and custom date formats)
- **Combined date + time**: merge a date-only and a time-only column

## Configuration

The dialog selects the delimiter, timestamp mode, and time column(s). Custom
date format strings use Qt-style codes (`yyyy-MM-dd hh:mm:ss`). Column picks
are saved by name, so the next file with the same header reopens with them.
