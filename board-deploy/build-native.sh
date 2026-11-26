#!/bin/bash
# Rebuild xiaoai_plus_speaker on the Armbian board as root.
# Prerequisites:
#   /root/sherpa_shared  - sherpa-onnx aarch64 shared-cpu package + c-api headers
#   /root/deps           - json-3.11.3, IXWebSocket-11.4.5, spdlog-1.15.1
set -u
SRC=/root/xiaoai-doubao-src/xiaoai-plus
cd "$SRC" || exit 1
rm -rf build-native
exec > /root/build.log 2>&1
set -x
cmake -S . -B build-native -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DSHERPA_ONNX_STATIC_ROOT=/root/sherpa_shared \
  -DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON=/root/deps/json-3.11.3 \
  -DFETCHCONTENT_SOURCE_DIR_IXWEBSOCKET=/root/deps/IXWebSocket-11.4.5 \
  -DFETCHCONTENT_SOURCE_DIR_SPDLOG=/root/deps/spdlog-1.15.1
rc=$?
if [ $rc -eq 0 ]; then
  cmake --build build-native --parallel 2 --target xiaoai_plus_speaker
  rc=$?
fi
echo BUILD_RC=$rc
