# DataTamer Message Parser

Decodes [DataTamer](https://github.com/facontidavide/data_tamer) binary
snapshots into typed numeric fields.

## Features

- Schema parsing via `BuilSchemaFromText()`
- Native type preservation: `float`, `double`, `int8_t`…`uint64_t` stored without casting to double
- Field names prefixed with topic path (`/field_name`)

## Encoding

Registered as parser for `"data_tamer"` encoding.

## Status

Fully ported with no significant gaps.
