#!/usr/bin/env bash
set -euo pipefail

: "${PICO_SDK_PATH:?Set PICO_SDK_PATH to your Raspberry Pi Pico SDK directory}"

build_dir="${BUILD_DIR:-build}"
cvbs_std="${CVBS_STD:-PAL}"
sysclk_khz="${CVBS_SYSCLK_KHZ:-240000}"

cmake -S . -B "$build_dir" \
  -DPICO_SDK_PATH="$PICO_SDK_PATH" \
  -DCVBS_STD="$cvbs_std" \
  -DCVBS_SYSCLK_KHZ="$sysclk_khz"
cmake --build "$build_dir" --parallel
