#!/usr/bin/env bash

set -euo pipefail

if [[ $# -ne 2 ]]; then
    echo "Usage: $0 <opencv-version> <install-directory>" >&2
    exit 2
fi

opencv_version="$1"
install_dir="$2"
runner_temp="${RUNNER_TEMP:-/tmp}"
archive="${runner_temp}/opencv-${opencv_version}.tar.gz"
source_dir="${runner_temp}/opencv-${opencv_version}"
build_dir="${runner_temp}/opencv-build"

curl --fail --location --silent --show-error \
    "https://github.com/opencv/opencv/archive/refs/tags/${opencv_version}.tar.gz" \
    --output "${archive}"
tar --extract --gzip --file "${archive}" --directory "${runner_temp}"

cmake -S "${source_dir}" -B "${build_dir}" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="${install_dir}" \
    -DCMAKE_INSTALL_LIBDIR=lib \
    -DBUILD_LIST=core,imgproc,highgui,imgcodecs,videoio,objdetect \
    -DBUILD_DOCS=OFF \
    -DBUILD_EXAMPLES=OFF \
    -DBUILD_JAVA=OFF \
    -DBUILD_opencv_apps=OFF \
    -DBUILD_opencv_python2=OFF \
    -DBUILD_opencv_python3=OFF \
    -DBUILD_PERF_TESTS=OFF \
    -DBUILD_TESTS=OFF \
    -DWITH_FFMPEG=OFF \
    -DWITH_GSTREAMER=OFF \
    -DWITH_GTK=OFF \
    -DWITH_QT=OFF
cmake --build "${build_dir}" --parallel 2
cmake --install "${build_dir}"
