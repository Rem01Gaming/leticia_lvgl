#!/usr/bin/env bash
set -euo pipefail

if [ "$1" == "clean" ]; then
  ndk-build -j12 clean
  rm -rf out
  exit 0
fi

if [ "$1" == "build" ]; then
  [ ! -d device_config/"$2" ] && {
    echo "ERROR: device config $2 does not exists!"
    exit 1
  }

  ndk-build -j12

  rm -rf out
  mkdir -p out
  cp -r flashable/. out/

  cp libs/arm64-v8a/update-binary out/META-INF/com/google/android
  cp device_config/"$2"/* out/config
  cp LICENSE out
  (cd out && zip -9 -r leticia_"${2//\//_}".zip . -x 'flashable/*' '.gitkeep' '*/.gitkeep')
fi
