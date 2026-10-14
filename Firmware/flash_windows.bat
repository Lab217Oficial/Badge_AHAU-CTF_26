@echo off
REM ============================================================
REM  AHAU CTF Badge - Flasheo automatico (Windows)
REM  Requiere Python instalado.
REM ============================================================
setlocal
cd /d "%~dp0"

echo.
echo === AHAU CTF Badge - Flasheo ESP32-S3 ===
echo.

where python >nul 2>nul
if errorlevel 1 (
    echo [ERROR] No se encontro Python. Instalalo desde https://www.python.org/downloads/
    echo         Marca "Add Python to PATH" durante la instalacion.
    pause
    exit /b 1
)

echo Comprobando esptool...
python -m esptool version >nul 2>nul
if errorlevel 1 (
    echo Instalando esptool...
    python -m pip install --user esptool
)

echo.
echo Conecta el badge por USB. Si falla, entra en modo descarga:
echo   manten BOOT, toca RESET, suelta BOOT.
echo.
pause

python -m esptool --chip esp32s3 -b 921600 write_flash 0x0 firmware\wifi_monitor.ino.merged.bin

echo.
if errorlevel 1 (
    echo [ERROR] Fallo el flasheo. Prueba el modo descarga (BOOT+RESET) o la Opcion A del LEEME.
) else (
    echo [OK] Firmware flasheado. Pulsa RESET en el badge.
)
pause
