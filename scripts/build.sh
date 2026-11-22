#!/usr/bin/env bash
# Build firmware Smart Clock. Dùng chung cho máy local và GitHub Actions.
# Kết quả: build/firmware.bin và build/version.txt
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORK="${WORK:-$ROOT/.toolchain}"
SKETCH="$ROOT/firmware/smartclock"
OUT="$ROOT/build"

CORE_TAG=3.1.2
ACLI_VER=1.3.1
# Thư viện ghim theo commit để build luôn ra cùng một kết quả
LIBS=(
  "Bodmer/TFT_eSPI 16e37595040eac69cd628e4bffb56fc30cad6299"
  "bblanchon/ArduinoJson 1f6e62f63eeb3fcfdb9f7c131eba294bfef76390"
  "tzapu/WiFiManager 4131fe617d01d746677e8b909379597162c2ca58"
)

mkdir -p "$WORK" "$OUT"
export ARDUINO_DIRECTORIES_USER="$WORK/sketchbook"
export ARDUINO_DIRECTORIES_DATA="$WORK/arduino15"
mkdir -p "$ARDUINO_DIRECTORIES_USER/hardware/esp8266com" "$ARDUINO_DIRECTORIES_USER/libraries"

# arduino-cli
case "$(uname -s)-$(uname -m)" in
  Linux-x86_64) ACLI_PKG=Linux_64bit ;;
  Darwin-arm64) ACLI_PKG=macOS_ARM64 ;;
  Darwin-x86_64) ACLI_PKG=macOS_64bit ;;
  *) echo "Hệ điều hành chưa hỗ trợ"; exit 1 ;;
esac
if [ ! -x "$WORK/arduino-cli" ]; then
  curl -sSL "https://github.com/arduino/arduino-cli/releases/download/v${ACLI_VER}/arduino-cli_${ACLI_VER}_${ACLI_PKG}.tar.gz" | tar xz -C "$WORK" arduino-cli
fi

# Core ESP8266 (lấy thẳng từ GitHub, không cần board manager)
CORE="$ARDUINO_DIRECTORIES_USER/hardware/esp8266com/esp8266"
if [ ! -d "$CORE/tools/xtensa-lx106-elf" ]; then
  rm -rf "$CORE"
  git clone -q --depth 1 --branch "$CORE_TAG" https://github.com/esp8266/Arduino.git "$CORE"
  (cd "$CORE" && git submodule update -q --init --depth 1 tools/sdk/lwip2/builder)
  (cd "$CORE/tools" && python3 get.py >/dev/null)
fi

# Thư viện
for entry in "${LIBS[@]}"; do
  read -r repo sha <<<"$entry"; dir="$ARDUINO_DIRECTORIES_USER/libraries/${repo#*/}"
  if [ ! -d "$dir" ] || [ "$(git -C "$dir" rev-parse HEAD)" != "$sha" ]; then
    rm -rf "$dir"; git init -q "$dir"
    git -C "$dir" fetch -q --depth 1 "https://github.com/$repo.git" "$sha"
    git -C "$dir" checkout -q FETCH_HEAD
  fi
done

# ctags giả: code đã khai báo hàm trước khi dùng nên không cần sinh prototype
mkdir -p "$WORK/fakectags"; printf '#!/bin/sh\nexit 0\n' > "$WORK/fakectags/ctags"; chmod +x "$WORK/fakectags/ctags"

"$WORK/arduino-cli" compile \
  -b esp8266com:esp8266:generic:eesz=4M1M,baud=115200,xtal=160 \
  --build-property "runtime.tools.ctags.path=$WORK/fakectags" \
  --build-property "compiler.cpp.extra_flags=-include $SKETCH/tft_setup.h" \
  --output-dir "$WORK/out" "$SKETCH" 2>&1 | grep -vE "^Error initializing instance|^loading library|SyntaxWarning|words = re.split" || true

test -f "$WORK/out/smartclock.ino.bin"
cp "$WORK/out/smartclock.ino.bin" "$OUT/firmware.bin"
grep -m1 '#define FW_VERSION' "$SKETCH/smartclock.ino" | sed -E 's/.*"(.*)".*/\1/' > "$OUT/version.txt"
echo "Build xong: v$(cat "$OUT/version.txt"), $(wc -c < "$OUT/firmware.bin") byte"
