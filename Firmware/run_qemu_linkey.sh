#!/bin/bash
# Launch LinKey firmware built with the dedicated QEMU emulation target.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IDF_PATH="${IDF_PATH:-/home/smartimpulse/esp/v5.4.1/esp-idf}"
TARIFF_OPTION="base"

QEMU_CMD="qemu-system-xtensa"
QEMU_PATH="${HOME}/.espressif/tools/qemu-xtensa/esp_develop_9.0.0_20240606/qemu/bin/qemu-system-xtensa"

if [ -x "$QEMU_PATH" ]; then
    QEMU_CMD="$QEMU_PATH"
elif ! command -v "$QEMU_CMD" >/dev/null 2>&1; then
    echo "ERROR: qemu-system-xtensa not found"
    echo "Install Espressif QEMU with: python \$IDF_PATH/tools/idf_tools.py install qemu-xtensa"
    exit 1
fi

DEBUG_MODE=0
USE_MONITOR=0
BUILD_FIRST=0

usage() {
    echo "Usage: $0 [--build] [--debug] [--monitor] [--tariff_option base|hphc|ejp|tempo]"
}

while [ $# -gt 0 ]; do
    case "$1" in
        --build)
            BUILD_FIRST=1
            shift
            ;;
        --debug)
            DEBUG_MODE=1
            shift
            ;;
        --monitor)
            USE_MONITOR=1
            shift
            ;;
        --tariff_option)
            if [ $# -lt 2 ]; then
                echo "ERROR: --tariff_option requires a value"
                usage
                exit 1
            fi
            TARIFF_OPTION="$2"
            shift 2
            ;;
        --tariff_option=*)
            TARIFF_OPTION="${1#*=}"
            shift
            ;;
        *)
            echo "Unknown option: $1"
            usage
            exit 1
            ;;
    esac
done

case "$TARIFF_OPTION" in
    base|BASE)
        TARIFF_OPTION="base"
        TARIFF_CMAKE="BASE"
        TARIFF_KCONFIG="CONFIG_LINKEY_TARIFF_BASE=y"
        ;;
    hphc|HPHC)
        TARIFF_OPTION="hphc"
        TARIFF_CMAKE="HPHC"
        TARIFF_KCONFIG="CONFIG_LINKEY_TARIFF_HPHC=y"
        ;;
    ejp|EJP)
        TARIFF_OPTION="ejp"
        TARIFF_CMAKE="EJP"
        TARIFF_KCONFIG="CONFIG_LINKEY_TARIFF_EJP=y"
        ;;
    tempo|TEMPO)
        TARIFF_OPTION="tempo"
        TARIFF_CMAKE="TEMPO"
        TARIFF_KCONFIG="CONFIG_LINKEY_TARIFF_TEMPO=y"
        ;;
    *)
        echo "ERROR: invalid --tariff_option '$TARIFF_OPTION'"
        usage
        exit 1
        ;;
esac

BUILD_DIR="${LINKEY_QEMU_BUILD_DIR:-${SCRIPT_DIR}/build-qemu-${TARIFF_OPTION}}"
QEMU_FLASH="${BUILD_DIR}/qemu_flash.bin"
QEMU_EFUSE="${BUILD_DIR}/qemu_efuse.bin"
BUILD_APP_BIN="${BUILD_DIR}/linky-ulp-monitor.bin"
BUILD_ELF="${BUILD_DIR}/linky-ulp-monitor.elf"
FLASH_ARGS="${BUILD_DIR}/flash_args"
TARIFF_DEFAULTS="${BUILD_DIR}/sdkconfig.tariff.defaults"

mkdir -p "$BUILD_DIR"
printf '%s\n' "$TARIFF_KCONFIG" > "$TARIFF_DEFAULTS"

BUILD_CMD=(
    idf.py
    -B "$BUILD_DIR"
    -DSDKCONFIG="${BUILD_DIR}/sdkconfig"
    "-DSDKCONFIG_DEFAULTS=sdkconfig.defaults;emulation/sdkconfig.qemu.defaults;${TARIFF_DEFAULTS}"
    -DLINKEY_QEMU_EMULATION=ON
    -DLINKEY_QEMU_TARIFF_OPTION="$TARIFF_CMAKE"
    build
)

if ! command -v idf.py >/dev/null 2>&1; then
    # shellcheck disable=SC1091
    source "${IDF_PATH}/export.sh" >/dev/null 2>&1
fi

if [ "$BUILD_FIRST" -eq 1 ]; then
    cd "$SCRIPT_DIR"
    "${BUILD_CMD[@]}"
fi

if [ ! -f "$BUILD_ELF" ] || [ ! -f "$BUILD_APP_BIN" ] || [ ! -f "$FLASH_ARGS" ]; then
    echo "QEMU build artifacts not found in ${BUILD_DIR}; building ${TARIFF_OPTION} target..."
    cd "$SCRIPT_DIR"
    "${BUILD_CMD[@]}"
fi

QEMU_FLASH_SIZE=0
if [ -f "$QEMU_FLASH" ]; then
    QEMU_FLASH_SIZE="$(stat -c%s "$QEMU_FLASH")"
fi

if [ ! -f "$QEMU_FLASH" ] || [ "$QEMU_FLASH_SIZE" -ne 2097152 ] || [ "$BUILD_APP_BIN" -nt "$QEMU_FLASH" ] || [ "$FLASH_ARGS" -nt "$QEMU_FLASH" ]; then
    echo "Creating QEMU flash image..."
    cd "$BUILD_DIR"
    # shellcheck disable=SC1091
    source "${IDF_PATH}/export.sh" >/dev/null 2>&1
    esptool.py --chip esp32 merge_bin --fill-flash-size 2MB -o "$QEMU_FLASH" @flash_args
fi

if [ ! -f "$QEMU_EFUSE" ]; then
    truncate -s 124 "$QEMU_EFUSE"
fi

QEMU_ARGS=(
    -M esp32
    -drive "file=${QEMU_FLASH},if=mtd,format=raw"
    -drive "file=${QEMU_EFUSE},if=none,format=raw,id=efuse"
    -global "driver=nvram.esp32.efuse,property=drive,value=efuse"
    -global "driver=timer.esp32.timg,property=wdt_disable,value=true"
    -nic "user,model=open_eth"
    -m 4M
    -nographic
    -display none
)

if [ "$USE_MONITOR" -eq 1 ]; then
    QEMU_ARGS+=(
        -serial stdio
        -monitor telnet:127.0.0.1:55555,server,nowait
    )
    echo "INFO: QEMU monitor enabled on telnet 127.0.0.1:55555"
else
    QEMU_ARGS+=(
        -serial mon:stdio
    )
fi

if [ "$DEBUG_MODE" -eq 1 ]; then
    QEMU_ARGS+=(
        -d guest_errors,unimp,page
        -D qemu_debug.log
    )
    echo "INFO: QEMU debug log: qemu_debug.log"
fi

echo "=============================================="
echo "LinKey QEMU Emulation"
echo "=============================================="
echo "Firmware:  $BUILD_ELF"
echo "Flash:     $QEMU_FLASH"
echo "eFuse:     $QEMU_EFUSE"
echo "QEMU:      $QEMU_CMD"
echo "Build:     LINKEY_QEMU_EMULATION=ON"
echo "Tariff:    $TARIFF_OPTION"
echo "=============================================="

exec "$QEMU_CMD" "${QEMU_ARGS[@]}"
