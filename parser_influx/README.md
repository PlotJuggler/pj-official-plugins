# InfluxDB Line Protocol Parser

Decodes [InfluxDB line protocol](https://docs.influxdata.com/influxdb/v2/reference/syntax/line-protocol/),
the text format Telegraf and InfluxDB clients send, into typed fields. Pick the
`influx` encoding in a streaming source (UDP, MQTT, ZMQ) to use it.

## Mapping

```
measurement[,tag=value...] field=value[,field=value...] [timestamp]
```

- Each field becomes the series `measurement/tag=value/.../field`, e.g.
  `cpu/cpu=cpu-total/host=box/usage_idle`.
- Field types stay native: `1.5` float, `12i` integer, `12u` unsigned, `t`/`false` (all
  spellings) boolean, `"text"` string.
- The timestamp is read as nanoseconds since the epoch. A line without one takes the
  message receive time.
- A message may carry many lines. Consecutive lines with the same timestamp share one
  row, so one Telegraf collection is one row.
- Backslash escapes (`\ `, `\,`, `\=` in names, `\"` and `\\` in strings) are decoded;
  `#` comment lines and blank lines are ignored.
- A malformed line (no field set, a bad timestamp, extra sections) or field (no `=`,
  an unreadable value) is skipped; the rest of the message is still imported.

## Telegraf

Send metrics to the UDP streaming source:

```toml
[[outputs.socket_writer]]
  address = "udp://127.0.0.1:8094"
  data_format = "influx"
```

Then start the UDP source on port 8094 with the `influx` encoding.

## Encoding

Registered as parser for `"influx"` encoding.

## Known limitations

- Timestamps must be nanoseconds, Telegraf's default precision for `socket_writer`.
- A raw newline inside a string value ends the line there (line protocol does not allow one).
