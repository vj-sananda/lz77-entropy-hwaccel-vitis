#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
revision=b2c657d254ead080de401ee33133702f1577424a
mkdir -p vendor
if [[ ! -d vendor/Vitis_Libraries/.git ]]; then
    git clone --filter=blob:none --no-checkout https://github.com/Xilinx/Vitis_Libraries.git vendor/Vitis_Libraries
    git -C vendor/Vitis_Libraries sparse-checkout set data_compression/L1
    git -C vendor/Vitis_Libraries checkout "$revision"
fi
[[ $(git -C vendor/Vitis_Libraries rev-parse HEAD) == "$revision" ]] || {
    echo 'Unexpected Vitis revision; preserve the checkout and use the pinned commit.' >&2; exit 1;
}
