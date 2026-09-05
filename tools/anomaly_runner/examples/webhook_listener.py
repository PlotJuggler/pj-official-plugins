#!/usr/bin/env python3
# Tiny local HTTP server to receive (and pretty-print) webhook deliveries from
# anomaly_runner during testing. Run it, then use examples/notify_webhook.json.
#
#   python3 tools/anomaly_runner/examples/webhook_listener.py
#
# Listens on 127.0.0.1:8731 and prints the Authorization header + the report body
# of every POST. Ctrl-C to stop.

import http.server
import json


class Handler(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(length)
        lines = ["\n=== webhook received ===",
                 "Authorization: " + str(self.headers.get("Authorization")),
                 "Content-Type : " + str(self.headers.get("Content-Type"))]
        try:
            doc = json.loads(body)
            lines.append("status: %s | total: %s" % (doc.get("status"), doc.get("summary", {}).get("total")))
        except Exception:
            lines.append("body: " + str(body[:300]))
        print("\n".join(lines), flush=True)  # flush so it shows even when piped / killed
        self.send_response(200)
        self.end_headers()
        self.wfile.write(b"ok")

    def log_message(self, *args):
        pass  # quiet the default request logging


if __name__ == "__main__":
    print("listening on http://127.0.0.1:8731  (Ctrl-C to stop)")
    http.server.HTTPServer(("127.0.0.1", 8731), Handler).serve_forever()
