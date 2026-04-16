#!/bin/sh
set -e

cd "$(dirname "$0")/.." || exit 1
mkdir -p build
cd build

if [ -d depot_tools ]; then
  echo "depot_tools is here, let's pull the latest version"
  (cd depot_tools && git pull origin main)
  echo "depot_tools is updated"
else
  echo "Fetching depot_tools..."
  git clone https://chromium.googlesource.com/chromium/tools/depot_tools.git
  echo "depot_tools is updated"
fi

if [ -d v8 ]; then
  echo "v8 is already here, no need to fetch"
else
  echo "Fetching v8..."
  PATH="$PWD/depot_tools:$PATH" depot_tools/vpython3 depot_tools/fetch.py v8
fi

echo "Syncing v8..."
cd v8
PATH="$PWD/../depot_tools:$PATH" gclient sync
echo "Done!"

echo "V8 is fetched and synced. Now you can run build.sh"
