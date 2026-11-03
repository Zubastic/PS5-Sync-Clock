#!/bin/sh
# PS5 Sync Clock - build script (run from WSL/Linux).
#   zubastic@linux:/mnt/hgfs/PS5-Sync-clock$ ./build.sh

set -e

export PS5_PAYLOAD_SDK=${PS5_PAYLOAD_SDK:-/opt/ps5-payload-sdk}
export BUILD_VERSION=${BUILD_VERSION:-1.0.0}

make clean
make
