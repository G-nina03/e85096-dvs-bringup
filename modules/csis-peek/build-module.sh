#!/bin/bash
# Build csis-peek.ko out of tree against the vendor 5.10 build in /work/out:
#
#   docker run --rm -v ~/e850-a13-build:/work -v e850-kernel:/work/kernel -v e850-out:/work/out \
#       -v "$PWD/modules/csis-peek:/m:ro" e850-build-env:1 bash /m/build-module.sh
#
# Output: /work/out/ext-csis-peek/csis-peek.ko
set -eu
export PATH=/work/prebuilts/clang/host/linux-x86/clang-r450784e/bin:$PATH
EXT=/work/out/ext-csis-peek
rm -rf $EXT && mkdir -p $EXT && cp /m/Kbuild /m/csis-peek.c $EXT/
make -C /work/kernel O=/work/out ARCH=arm64 LLVM=1 LLVM_IAS=1 CROSS_COMPILE=aarch64-linux-gnu- \
     M=$EXT modules
ls -l $EXT/csis-peek.ko
