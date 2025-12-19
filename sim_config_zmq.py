#!/usr/bin/env python3
import argparse
import json
import time
import zmq


def build_realtime_cfg(
    center_freq_hz: int,
    sample_rate_hz: int,
    rbw_hz: int,
    window: str,
    scale: float,
    overlap: float,
    antenna_amp: bool,
    span: int,
) -> str:
    # Estructura EXACTA según tu ejemplo
    payload = {
        "rf_mode": "realtime",
        "center_freq_hz": int(center_freq_hz),
        "sample_rate_hz": int(sample_rate_hz),
        "rbw_hz": int(rbw_hz),
        "window": window,
        "scale": scale,
        "overlap": float(overlap),
        "antenna_amp": bool(antenna_amp),
        "span": int(span),
    }
    return json.dumps(payload, separators=(",", ":"))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ipc", default="ipc:///tmp/rf_engine",
                    help="Debe coincidir con IPC_ADDR del motor C (C hace connect, Python hace bind)")
    ap.add_argument("--hz", type=float, default=2.0, help="Frecuencia de envío (Hz)")
    ap.add_argument("--fixed", action="store_true", help="Envía SIEMPRE el mismo JSON")
    ap.add_argument("--scan", action="store_true", help="Hace barrido cambiando center_freq_hz")
    ap.add_argument("--cf", type=int, default=105_700_000, help="Center freq inicial (Hz)")
    ap.add_argument("--sr", type=int, default=2_000_000, help="Sample rate (Hz)")
    ap.add_argument("--rbw", type=int, default=10_000, help="RBW (Hz)")
    ap.add_argument("--span", type=int, default=200_000, help="Span (Hz)")
    ap.add_argument("--window", default="hann", help="Ventana Welch (hann, hamming, blackman, ...)")
    ap.add_argument("--overlap", type=float, default=0.5, help="Overlap [0..1)")
    ap.add_argument("--scale", type=float, default=1.0, help="Escala PSD")
    ap.add_argument("--amp", action="store_true", help="antenna_amp=true")
    args = ap.parse_args()

    period = 1.0 / max(args.hz, 0.1)

    ctx = zmq.Context.instance()
    sock = ctx.socket(zmq.PAIR)

    # C usa zmq_connect(...) => Python debe bind
    sock.bind(args.ipc)
    print(f"[SIM] ZMQ PAIR bound at {args.ipc}")
    print(f"[SIM] Sending @ {args.hz:.2f} Hz (period {period:.3f}s)")

    cf = args.cf
    step = 50_000  # 50 kHz por paso (ajusta)
    i = 0

    try:
        while True:
            if args.fixed or (not args.scan):
                payload = build_realtime_cfg(
                    center_freq_hz=args.cf,
                    sample_rate_hz=args.sr,
                    rbw_hz=args.rbw,
                    window=args.window,
                    scale=args.scale,
                    overlap=args.overlap,
                    antenna_amp=args.amp,
                    span=args.span,
                )
            else:
                # scan: cambia center_freq_hz (y opcionalmente rbw)
                payload = build_realtime_cfg(
                    center_freq_hz=cf,
                    sample_rate_hz=args.sr,
                    rbw_hz=args.rbw,
                    window=args.window,
                    scale=args.scale,
                    overlap=args.overlap,
                    antenna_amp=args.amp,
                    span=args.span,
                )
                cf += step
                # pequeño barrido ida/vuelta (opcional)
                if cf > args.cf + 1_000_000:
                    cf = args.cf

            sock.send_string(payload)

            if i % max(int(args.hz), 1) == 0:
                print(f"[SIM] sent: {payload}")

            i += 1
            time.sleep(period)

    except KeyboardInterrupt:
        print("\n[SIM] stopped.")
    finally:
        sock.close(0)
        ctx.term()


if __name__ == "__main__":
    main()
