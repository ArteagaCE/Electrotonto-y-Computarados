#!/usr/bin/env python3

import argparse
import json
import os
import queue
import threading
import time
from datetime import datetime
from flask import Flask, Response, jsonify, render_template, abort

SIGNAL_META = {
    1: {"name": "Señal 1", "unit": "nivel lógico", "scale": 1, "y_min": -0.1, "y_max": 1.1},
    2: {"name": "Señal 2", "unit": "nivel lógico", "scale": 1, "y_min": -0.1, "y_max": 1.1},
}

current_signal = 1
last_value = None
sse_clients = []
sse_lock = threading.Lock()

app = Flask(__name__)


def _dispatch(raw: int):
    global last_value

    meta = SIGNAL_META[current_signal]
    event = {
        "ts": datetime.now().strftime("%H:%M:%S"),
        "raw": raw,
        "value": round(raw / meta["scale"], 3),
        "unit": meta["unit"],
        "signal": current_signal,
        "name": meta["name"],
        "y_min": meta["y_min"],
        "y_max": meta["y_max"],
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
            if q in sse_clients:
                sse_clients.remove(q)


def _read_device_loop(device_path: str):
    while True:
        try:
            fd = os.open(device_path, os.O_RDONLY)
            print(f"[sensor] Conectado a {device_path}")
            try:
                while True:
                    raw_bytes = os.read(fd, 64)
                    if not raw_bytes:
                        break
                    _dispatch(int(raw_bytes.decode().strip()))
            finally:
                os.close(fd)
        except FileNotFoundError:
            print(f"[sensor] {device_path} no encontrado, reintentando en 3s")
            time.sleep(3)
        except (OSError, ValueError) as e:
            print(f"[sensor] Error: {e}, reintentando en 2s")
            time.sleep(2)


def _simulate_loop():
    tick = 0
    print("[sensor] Modo simulación activo")
    while True:
        raw = (tick // 4) % 2 if current_signal == 1 else (tick // 2) % 2
        _dispatch(raw)
        tick += 1
        time.sleep(1)


@app.route("/")
def index():
    return render_template("index.html")


@app.route("/stream")
def stream():
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
                    yield ": heartbeat\n\n"
        finally:
            with sse_lock:
                if client_q in sse_clients:
                    sse_clients.remove(client_q)

    return Response(
        generate(),
        mimetype="text/event-stream",
        headers={"Cache-Control": "no-cache", "X-Accel-Buffering": "no"},
    )


@app.route("/select/<int:sig>", methods=["POST"])
def select_signal(sig):
    global current_signal

    if sig not in SIGNAL_META:
        abort(400, "Señal inválida. Use 1 o 2.")

    if os.path.exists(app.config["DEVICE_PATH"]):
        try:
            with open(app.config["DEVICE_PATH"], "w") as f:
                f.write(str(sig))
        except OSError as e:
            return jsonify({"error": str(e)}), 500

    current_signal = sig
    return jsonify({
        "signal": sig,
        "name": SIGNAL_META[sig]["name"],
        "unit": SIGNAL_META[sig]["unit"],
        "y_min": SIGNAL_META[sig]["y_min"],
        "y_max": SIGNAL_META[sig]["y_max"],
    })


@app.route("/status")
def status():
    meta = SIGNAL_META[current_signal]
    return jsonify({
        "signal": current_signal,
        "name": meta["name"],
        "unit": meta["unit"],
        "last_value": last_value,
    })


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--device", default="/dev/sensor_cdd")
    parser.add_argument("--port", type=int, default=8080)
    args = parser.parse_args()

    app.config["DEVICE_PATH"] = args.device

    if os.path.exists(args.device):
        t = threading.Thread(target=_read_device_loop, args=(args.device,), daemon=True)
    else:
        t = threading.Thread(target=_simulate_loop, daemon=True)

    t.start()

    print(f"[web] Servidor en http://0.0.0.0:{args.port}")
    app.run(host="0.0.0.0", port=args.port, threaded=True, debug=False)


if __name__ == "__main__":
    main()
