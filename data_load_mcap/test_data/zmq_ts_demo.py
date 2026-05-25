#!/usr/bin/env python3
"""
ZMQ timestamp demo — diferencia inconfundible entre embedded ts y receive time.

Dos sensores miden EL MISMO valor Y = sin(t), pero sensor_B lleva
el reloj 30 segundos adelantado (clock offset = +30 s).

Con ☑ embedded timestamp activo:
  PJ4 coloca sensor_B 30 s a la DERECHA de sensor_A en el eje X.
  Las dos curvas aparecen SEPARADAS por 30 s.

Con ☐ embedded timestamp desactivado:
  PJ4 usa el receive time (mismo instante de llegada para ambos).
  Las dos curvas se SUPERPONEN — una sola línea.

La diferencia es inconfundible: o ves 1 línea o ves 2 líneas separadas 30 s.

Instalar:  pip install pyzmq
Arrancar:  python3 zmq_ts_demo.py

Diálogo ZMQ en PJ4:
  localhost | 9872 | connect | json
  Configure parser → ☑ Use embedded timestamp | field: timestamp
  Arrastra sensor_A/sin y sensor_B/sin al MISMO plot.
"""

import json
import math
import time

import zmq

PORT       = 9872
RATE_HZ    = 20
DT         = 1.0 / RATE_HZ
CLOCK_BIAS = 30.0   # sensor_B cree que son 30 s más de lo que son


def main():
    ctx    = zmq.Context()
    socket = ctx.socket(zmq.PUB)
    socket.bind(f"tcp://*:{PORT}")
    time.sleep(0.3)

    print(f"ZMQ PUB  tcp://*:{PORT}  a {RATE_HZ} Hz")
    print()
    print("┌──────────────────────────────────────────────────────────────┐")
    print("│  Arrastra sensor_A/sin y sensor_B/sin al MISMO plot          │")
    print("│                                                               │")
    print("│  ☑ embedded timestamp → 2 líneas separadas 30 s en el eje X │")
    print("│  ☐ sin embedded ts    → 1 sola línea (superpuestas)          │")
    print("└──────────────────────────────────────────────────────────────┘")
    print()

    t = 0.0

    while True:
        y = round(5.0 * math.sin(2 * math.pi * 0.5 * t), 4)

        # Misma medición física, distinto reloj
        msg_a = {"timestamp": round(t,              4), "sin": y}
        msg_b = {"timestamp": round(t + CLOCK_BIAS, 4), "sin": y}

        socket.send_multipart([b"sensor_A", json.dumps(msg_a).encode()])
        socket.send_multipart([b"sensor_B", json.dumps(msg_b).encode()])

        print(
            f"\r  t={t:6.1f}s | A.ts={t:.1f}  B.ts={t+CLOCK_BIAS:.1f}  Y={y:+.2f}",
            end="", flush=True,
        )

        t += DT
        time.sleep(DT)


if __name__ == "__main__":
    main()
