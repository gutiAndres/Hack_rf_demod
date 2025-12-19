#!/usr/bin/env bash
set -euo pipefail

# =========================================================
# Configuración general
# =========================================================
OUT="rf_engine"          # binario final
MAIN="rf_audio.c"        # archivo principal (ajústalo si tu archivo se llama distinto)

# Directorio de librerías locales
LIBDIR="./libs"

# =========================================================
# Flags de compilación
# =========================================================
CFLAGS=(
  -O2
  -Wall
  -Wextra
  -std=gnu11
  -D_GNU_SOURCE
  -I"$LIBDIR"
)

# Debug (actívalo si quieres)
# CFLAGS+=(-g -fsanitize=address)

# =========================================================
# Fuentes del proyecto
#   Cambios para Opus streaming:
#    - Añadir opus_tx.c
# =========================================================
SRCS=(
  "$MAIN"
  "$LIBDIR/psd.c"
  "$LIBDIR/ring_buffer.c"
  "$LIBDIR/zmq_util.c"
  "$LIBDIR/utils.c"
  "$LIBDIR/fm_radio.c"
  "$LIBDIR/sdr_HAL.c"
  "$LIBDIR/opus_tx.c"     # <-- NUEVO: encoder Opus + framing TCP 'OPU0'
)

# =========================================================
# Librerías del sistema
#   Cambios para Opus streaming:
#    - Añadir -lopus
# =========================================================
LIBS=(
  -lhackrf
  -lzmq
  -lcjson
  -lopus               # <-- NUEVO
  -lpthread
  -lm
  -lfftw3
)

# =========================================================
# Build
# =========================================================
echo "[BUILD] Compilando $OUT ..."
echo "[BUILD] Sources:"
for s in "${SRCS[@]}"; do echo "  - $s"; done

gcc "${CFLAGS[@]}" \
    "${SRCS[@]}" \
    -o "$OUT" \
    "${LIBS[@]}"

echo "[BUILD] OK → ./$OUT"
echo
echo "[NOTA] Dependencias (Debian/Ubuntu):"
echo "  sudo apt-get install -y libopus-dev"
