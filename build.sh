#!/usr/bin/env bash

if [ "$1" == "clean" ]; then
  ndk-build -j12 clean
  rm -rf out
  exit 0
fi

if [ "$1" == "build" ]; then
  ndk-build -j12

  [ ! -d ./out ] && cp -r flashable out
  cp libs/arm64-v8a/update-binary out/META-INF/com/google/android
  cp device_config/"$2"/* out/config

  mkdir -p out/flashable
  (cd out && zip -9 -r flashable/leticia_"${2//\//_}".zip . -x 'flashable/*' '.gitkeep' '*/.gitkeep')
fi
