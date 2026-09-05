# anomaly_runner — ready-to-run examples

Drop-in files so you can exercise the runner (and its notifications) **without writing any
JSON yourself**. Run everything from the repo root.

```bash
R=build/all/Release/bin/anomaly_runner
EX=tools/anomaly_runner/examples
DATA=toolbox_anomaly_detector/test_data/anomaly_demo.csv
```

| File | What it is |
|---|---|
| `spike_rule.json` | A portable rule (the built-in "Spike (point)", source already set to `anomaly_demo/value`) — run it with `--rule`, no `--source` needed. |
| `notify_command.json` | Notify via a shell command (no network). Writes the report to `/tmp/anomaly_report.json`. |
| `notify_webhook.json` | Notify via HTTP POST to `127.0.0.1:8731`. Needs `webhook_listener.py` running + `WH_TOKEN` exported. |
| `notify_email.json` | Notify via SMTP to `127.0.0.1:8025` (a local catcher). |
| `notify_all.json` | All three sinks at once, `notify_on: always`. |
| `webhook_listener.py` | A tiny local HTTP server that prints what the webhook receives. |

## 1) Just run a rule (no notifications)

```bash
$R --data $DATA --rule $EX/spike_rule.json        # JSON to stdout, exit 1 (7 anomalies)
```

## 2) Notify via command sink (easiest — no network)

```bash
$R --data $DATA --rule $EX/spike_rule.json --notify $EX/notify_command.json
cat /tmp/anomaly_report.json
```

## 3) Notify via webhook (two terminals)

```bash
# terminal 1 — receiver
python3 $EX/webhook_listener.py

# terminal 2 — run; ${WH_TOKEN} proves the secret comes from the env, not the file
read -rsp "Webhook token: " WH_TOKEN && export WH_TOKEN && echo
$R --data $DATA --rule $EX/spike_rule.json --notify $EX/notify_webhook.json
```

## 4) Notify via email (local SMTP catcher)

```bash
pip install aiosmtpd            # once
python3 -m aiosmtpd -n -l 127.0.0.1:8025 &      # prints whatever it receives
$R --data $DATA --rule $EX/spike_rule.json --notify $EX/notify_email.json
```

## 5) Policy + strict mode

```bash
# A passing run (only 'error' markers, but we fail only on 'critical') -> command sink does NOT fire
$R --data $DATA --rule $EX/spike_rule.json --fail-on critical --notify $EX/notify_command.json

# notify_all.json uses notify_on:always -> fires even when the run passes
$R --data $DATA --rule $EX/spike_rule.json --fail-on critical --notify $EX/notify_all.json
```

See the [runner README](../README.md#configuring-notifications) for the full config reference.
