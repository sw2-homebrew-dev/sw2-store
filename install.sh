#!/bin/sh
# Build the on-console store into an sw2-sdk checkout's apps/ folder.
#   SW2_SDK  path to sw2-sdk (default ../sw2-sdk)
set -e
cd "$(dirname "$0")"
SDK=$(cd "${SW2_SDK:-../sw2-sdk}" && pwd)
rm -rf "$SDK/apps/sw2-store"
mkdir -p "$SDK/apps/sw2-store"
cp -r app/. "$SDK/apps/sw2-store/"
"$SDK/sw2" build "$SDK/apps/sw2-store"
echo "installed into $SDK/apps/sw2-store; start the launcher with: $SDK/sw2 run"
