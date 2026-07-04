#!/bin/bash
# deploy.sh — compile and OTA-flash both Pencreus boards over WiFi
# Usage:  ./deploy.sh           (flash both)
#         ./deploy.sh 1         (flash pencreus-1 only)
#         ./deploy.sh 2         (flash pencreus-2 only)

set -e

FQBN="esp32:esp32:esp32s3:FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,PSRAM=opi,UploadSpeed=921600"
SKETCH="ESP32_RLCD_Display/ESP32_RLCD_Display.ino"
BUILD_DIR="/tmp/pencreus-build"
OTA_PASS="pencreus"
TARGET="${1:-both}"

echo "==> Compiling..."
arduino-cli compile --fqbn "$FQBN" --output-dir "$BUILD_DIR" "$SKETCH"

flash_board() {
  local host="$1"
  echo "==> Flashing $host..."
  arduino-cli upload \
    --fqbn "$FQBN" \
    --port "$host" \
    --protocol network \
    --upload-field password="$OTA_PASS" \
    --input-dir "$BUILD_DIR"
  echo "    $host done."
}

if [[ "$TARGET" == "1" ]]; then
  flash_board "pencreus-1.local"
elif [[ "$TARGET" == "2" ]]; then
  flash_board "pencreus-2.local"
else
  flash_board "pencreus-1.local"
  flash_board "pencreus-2.local"
fi

echo "==> All done."
