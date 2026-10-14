#!/usr/bin/env bash
# ============================================================
#  AHAU CTF Badge - Flasheo automatico (Linux / macOS)
#  Requiere Python 3.
# ============================================================
set -e
cd "$(dirname "$0")"

echo
echo "=== AHAU CTF Badge - Flasheo ESP32-S3 ==="
echo

if ! command -v python3 >/dev/null 2>&1; then
    echo "[ERROR] No se encontro python3. Instalalo con tu gestor de paquetes."
    exit 1
fi

echo "Comprobando esptool..."
if ! python3 -m esptool version >/dev/null 2>&1; then
    echo "Instalando esptool..."
    python3 -m pip install --user esptool
fi

echo
echo "Conecta el badge por USB. Si falla, entra en modo descarga:"
echo "  manten BOOT, toca RESET, suelta BOOT."
echo
read -p "Pulsa ENTER para flashear..."

python3 -m esptool --chip esp32s3 -b 921600 write_flash 0x0 firmware/wifi_monitor.ino.merged.bin

echo
echo "[OK] Firmware flasheado (si no hubo errores). Pulsa RESET en el badge."
