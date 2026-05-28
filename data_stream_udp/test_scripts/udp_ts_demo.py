#!/usr/bin/env python3
"""
UDP timestamp demo — espejo exacto del mqtt_ts_demo, adaptado a UDP.

En MQTT los dos sensores son dos TOPICS distintos (pj/sensor_A, pj/sensor_B).
UDP no lleva el topic en el payload: cada puerto ES un topic. Por eso cada
sensor necesita su propio puerto, y así sus timestamps quedan monótonos
(sin intercalar, que es lo que rompía el caso de un solo puerto).

  Puerto 9870 (sensor_A)  →  "timestamp": t          (empieza en 0 s)
  Puerto 9871 (sensor_B)  →  "timestamp": t + 5.0    (5 s por delante)

Resultado en PJ4 con ☑ Use embedded timestamp en AMBOS streams:
  udp/data/sin (stream 9870)  →  X =  0 ..  N s
  udp/data/sin (stream 9871)  →  X =  5 .. N+5 s   ← 5 s de diferencia visible

Resultado SIN embedded timestamp:
  ambos  →  X = ~1.78e9 s (Unix epoch)             ← la diferencia desaparece

CÓMO USAR EN PJ4:
  Stream 1 → puerto 9870  json  ☑ Use embedded timestamp  field: timestamp
  Stream 2 → puerto 9871  json  ☑ Use embedded timestamp  field: timestamp
  Arrastra udp/data/sin de AMBOS streams al MISMO plot.

  ☑ ts ON  → dos curvas separadas 5 s
  ☐ ts OFF → dos curvas superpuestas (mismo epoch)

Arrancar:  python3 udp_ts_demo.py
"""

import json
import math
import random
import socket
import time

HOST    = "127.0.0.1"
PORT_A  = 9870
PORT_B  = 9871
RATE_HZ = 20
DT      = 1.0 / RATE_HZ
OFFSET  = 5.0   # sensor_B siempre 5 s por delante de sensor_A


def main():
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    print(f"UDP PUB  {HOST}:{PORT_A} (sensor_A)  +  {HOST}:{PORT_B} (sensor_B)  a {RATE_HZ} Hz")
    print()
    print("┌──────────────────────────────────────────────────────────────┐")
    print("│  ☑ embedded timestamp activo:                                │")
    print("│     udp/data/sin (9870)  →  X =  0 ..  N s                 │")
    print("│     udp/data/sin (9871)  →  X =  5 .. N+5 s  (5 s más)      │")
    print("│                                                               │")
    print("│  ☐ embedded timestamp desactivado:                           │")
    print("│     ambos  →  X = ~1.78e9 s  (mismo epoch, sin offset)      │")
    print("└──────────────────────────────────────────────────────────────┘")
    print()
    print("Ctrl+C para parar\n")

    t = 0.0

    while True:
        ts_a = round(t,          4)   # sensor_A: 0, 0.05, 0.10 ...
        ts_b = round(t + OFFSET, 4)   # sensor_B: 5, 5.05, 5.10 ...

        sensor_a = {
            "timestamp": ts_a,
            "sin":  round(5.0 * math.sin(2 * math.pi * 0.5 * t), 4),
            "cos":  round(5.0 * math.cos(2 * math.pi * 0.5 * t), 4),
            "ramp": round((t % 5.0) * 2.0,                        4),
        }
        sensor_b = {
            "timestamp": ts_b,
            "sin":  round(5.0 * math.sin(2 * math.pi * 0.5 * t), 4),
            "cos":  round(5.0 * math.cos(2 * math.pi * 0.5 * t), 4),
            "temp": round(20.0 + 3.0 * math.sin(2 * math.pi * 0.2 * t) + random.uniform(-0.1, 0.1), 4),
        }

        sock.sendto(json.dumps(sensor_a).encode(), (HOST, PORT_A))
        sock.sendto(json.dumps(sensor_b).encode(), (HOST, PORT_B))

        print(
            f"\r  A.ts={ts_a:6.2f}s  B.ts={ts_b:6.2f}s  "
            f"Δ={ts_b - ts_a:.1f}s  sin={sensor_a['sin']:+.2f}",
            end="", flush=True,
        )

        t += DT
        time.sleep(DT)


if __name__ == "__main__":
    main()
