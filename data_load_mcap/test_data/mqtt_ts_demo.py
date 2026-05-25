#!/usr/bin/env python3
"""
MQTT timestamp demo — muestra la diferencia entre embedded timestamp
y receive time (Unix epoch) de forma visualmente clara.

Publica dos topics con timestamps controlados:
  pj/sensor_A  →  "timestamp": t          (empieza en 0 s)
  pj/sensor_B  →  "timestamp": t + 5.0    (empieza en 5 s, 5 s por delante)

Resultado en PJ4 con ☑ Use embedded timestamp:
  pj/sensor_A/sin  →  X axis   0 ..  N s
  pj/sensor_B/sin  →  X axis   5 .. N+5 s   ← 5 s de diferencia visible

Resultado SIN embedded timestamp:
  pj/sensor_A/sin  →  X axis  ~1.78e9 s  (Unix epoch)
  pj/sensor_B/sin  →  X axis  ~1.78e9 s  (mismo epoch, diferencia 0)
  → la diferencia de 5 s desaparece: ambos en el mismo X

Esto demuestra que el embedded timestamp es imprescindible para
preservar la relación temporal real entre señales.

Requisitos:
  pip install paho-mqtt
  sudo systemctl start mosquitto

Arrancar:
  python3 mqtt_ts_demo.py

Diálogo MQTT en PJ4:
  Broker: localhost:1883  |  Encoding: json
  Configure parser → ☑ Use embedded timestamp | field: timestamp
"""

import json
import math
import random
import time

import paho.mqtt.client as mqtt

HOST    = "localhost"
PORT    = 1883
RATE_HZ = 20
DT      = 1.0 / RATE_HZ
OFFSET  = 5.0   # sensor_B siempre 5 s por delante de sensor_A


def main():
    client = mqtt.Client()
    client.connect(HOST, PORT)
    client.loop_start()

    print(f"MQTT PUB  {HOST}:{PORT}  a {RATE_HZ} Hz")
    print()
    print("┌──────────────────────────────────────────────────────────────┐")
    print("│  ☑ embedded timestamp activo:                                │")
    print("│     pj/sensor_A/sin  →  X =  0 ..  N s                      │")
    print("│     pj/sensor_B/sin  →  X =  5 .. N+5 s  (5 s más tarde)    │")
    print("│                                                               │")
    print("│  ☐ embedded timestamp desactivado:                           │")
    print("│     pj/sensor_A/sin  →  X = ~1.78e9 s  (Unix epoch)         │")
    print("│     pj/sensor_B/sin  →  X = ~1.78e9 s  (mismo, sin offset)  │")
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

        client.publish("pj/sensor_A", json.dumps(sensor_a))
        client.publish("pj/sensor_B", json.dumps(sensor_b))

        print(
            f"\r  A.ts={ts_a:6.2f}s  B.ts={ts_b:6.2f}s  "
            f"Δ={ts_b - ts_a:.1f}s  sin={sensor_a['sin']:+.2f}",
            end="", flush=True,
        )

        t += DT
        time.sleep(DT)


if __name__ == "__main__":
    main()
