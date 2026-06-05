#!/usr/bin/env python3
"""
sensor_server.py — Lee /dev/sensor_cdd y sirve los datos por HTTP + SSE.

Endpoints:
  GET  /           → Página web con el gráfico en tiempo real
  GET  /stream     → Server-Sent Events: una línea JSON por muestra
  POST /select/<n> → Selecciona la señal activa (n = 1 ó 2)
  GET  /status     → JSON con el estado actual

Uso:
  sudo python3 sensor_server.py [--device /dev/sensor_cdd] [--port 8080]

Modo simulación:
  Si el dispositivo no existe, el servidor genera datos sintéticos.
  Esto permite desarrollar la interfaz web sin la RPi.

Corrección de escala (a nivel usuario, según el enunciado):
  Señal 1 (temperatura): raw es centidegrees → dividir por 100 → °C
  Señal 2 (voltaje):     raw es mV           → dividir por 1000 → V
"""

import argparse
import json
import math
import os
import queue
import random
import threading
import time
from datetime import datetime
from flask import Flask, Response, jsonify, render_template, abort

# ─────────────────────────────────────────────
# Configuración de señales (escala + metadata)
# ─────────────────────────────────────────────
SIGNAL_META = {
    1: {"name": "Temperatura", "unit": "°C",  "scale": 100,  "y_min": 10,  "y_max": 40},
    2: {"name": "Voltaje",     "unit": "V",   "scale": 1000, "y_min": 0,   "y_max": 3.5},
}

# ─────────────────────────────────────────────
# Estado global compartido
# ─────────────────────────────────────────────
current_signal  = 1
last_value      = None
sse_clients     = []
sse_lock        = threading.Lock()

# ─────────────────────────────────────────────
# Lectura del dispositivo
# ─────────────────────────────────────────────
def _read_device_loop(device_path: str):
    """Hilo bloqueante: lee el CDD y distribuye datos a los clientes SSE."""
    global last_value

    while True:
        try:
            fd = os.open(device_path, os.O_RDONLY)
            print(f"[sensor] Conectado a {device_path}")
            try:
                while True:
                    raw_bytes = os.read(fd, 64)
                    if not raw_bytes:
                        break
                    raw = int(raw_bytes.decode().strip())
                    _dispatch(raw)
            finally:
                os.close(fd)
        except FileNotFoundError:
            print(f"[sensor] {device_path} no encontrado, reintentando en 3s…")
            time.sleep(3)
        except (OSError, ValueError) as e:
            print(f"[sensor] Error: {e}, reintentando en 2s…")
            time.sleep(2)


def _simulate_loop():
    """Hilo alternativo cuando el dispositivo no está disponible."""
    global current_signal
    s1_raw = 2500
    s2_step = 0
    print("[sensor] Modo simulación activo")

    while True:
        if current_signal == 1:
            s1_raw = max(1500, min(3500, s1_raw + random.randint(-12, 12)))
            raw = s1_raw
        else:
            s2_step = (s2_step + 1) % 100
            raw = (s2_step * 66) if s2_step < 50 else ((100 - s2_step) * 66)
        _dispatch(raw)
        time.sleep(1)


def _dispatch(raw: int):
    """Convierte raw → unidad física, construye el evento SSE y lo envía a todos los clientes."""
    global last_value, current_signal
    meta  = SIGNAL_META[current_signal]
    value = round(raw / meta["scale"], 3)
    ts    = datetime.now().strftime("%H:%M:%S")

    event = {
        "ts":      ts,
        "raw":     raw,
        "value":   value,
        "unit":    meta["unit"],
        "signal":  current_signal,
        "name":    meta["name"],
        "y_min":   meta["y_min"],
        "y_max":   meta["y_max"],
    }
    last_value = event

    with sse_lock:
        dead = []
        for q in sse_clients:
            try:
                q.put_nowait(event)
            except queue.Full:
                dead.append(q)
        for q in dead:
            sse_clients.remove(q)


# ─────────────────────────────────────────────
# Flask app
# ─────────────────────────────────────────────
app = Flask(__name__)


@app.route("/")
def index():
    return render_template("index.html")


@app.route("/stream")
def stream():
    """SSE: cada muestra del CDD produce un evento 'message'."""
    client_q = queue.Queue(maxsize=120)
    with sse_lock:
        sse_clients.append(client_q)

    def generate():
        try:
            while True:
                try:
                    event = client_q.get(timeout=5)
                    yield f"data: {json.dumps(event)}\n\n"
                except queue.Empty:
                    yield ": heartbeat\n\n"  # mantener la conexión HTTP viva
        finally:
            with sse_lock:
                if client_q in sse_clients:
                    sse_clients.remove(client_q)

    return Response(generate(), mimetype="text/event-stream",
                    headers={"Cache-Control": "no-cache", "X-Accel-Buffering": "no"})


@app.route("/select/<int:sig>", methods=["POST"])
def select_signal(sig):
    global current_signal
    if sig not in SIGNAL_META:
        abort(400, "Señal inválida. Use 1 ó 2.")

    # Escribir en el dispositivo (o sólo actualizar estado en modo simulación)
    if os.path.exists(app.config["DEVICE_PATH"]):
        try:
            with open(app.config["DEVICE_PATH"], "w") as f:
                f.write(str(sig))
        except OSError as e:
            return jsonify({"error": str(e)}), 500

    current_signal = sig
    return jsonify({"signal": sig, "name": SIGNAL_META[sig]["name"]})


@app.route("/status")
def status():
    meta = SIGNAL_META[current_signal]
    return jsonify({
        "signal":     current_signal,
        "name":       meta["name"],
        "unit":       meta["unit"],
        "last_value": last_value,
    })


# ─────────────────────────────────────────────
# Entry point
# ─────────────────────────────────────────────
def main():
    parser = argparse.ArgumentParser(description="Servidor web del CDD sensor")
    parser.add_argument("--device", default="/dev/sensor_cdd",
                        help="Ruta al dispositivo de caracteres")
    parser.add_argument("--port", type=int, default=8080,
                        help="Puerto HTTP (default: 8080)")
    args = parser.parse_args()

    app.config["DEVICE_PATH"] = args.device

    if os.path.exists(args.device):
        t = threading.Thread(target=_read_device_loop, args=(args.device,), daemon=True)
    else:
        print(f"[warn] {args.device} no encontrado → activando simulación")
        t = threading.Thread(target=_simulate_loop, daemon=True)
    t.start()

    print(f"[web]  Servidor en http://0.0.0.0:{args.port}")
    print(f"[web]  Abrí http://<ip-de-la-pi>:{args.port} desde tu PC")
    app.run(host="0.0.0.0", port=args.port, threaded=True, debug=False)


if __name__ == "__main__":
    main()
