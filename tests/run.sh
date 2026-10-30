#!/bin/sh
# Host tests for the store.  SW2_SDK points at an sw2-sdk checkout.
set -e
cd "$(dirname "$0")/.."
SDK=${SW2_SDK:-../sw2-sdk}
mkdir -p build
${HOSTCC:-cc} -std=gnu11 -O1 -Wall -Wextra -Werror -I"$SDK/sdk/include" \
  -isystem "$SDK/sdk/third_party/vulkan-headers/include" \
  tests/store_harness.c app/src/store.c -o build/store_harness
./build/store_harness
