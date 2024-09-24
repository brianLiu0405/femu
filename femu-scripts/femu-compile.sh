#!/bin/bash

NRCPUS="$(cat /proc/cpuinfo | grep "vendor_id" | wc -l)"

make clean 2>&1 | tee -a compile_log
# --disable-werror --extra-cflags=-w --disable-git-update
../configure --enable-kvm --target-list=x86_64-softmmu --enable-slirp --disable-werror 2>&1 | tee -a compile_log
make -j $NRCPUS  2>&1 | tee -a compile_log

echo "" 2>&1 | tee -a compile_log
echo "===> FEMU compilation done ..." 2>&1 | tee -a compile_log
echo "" 2>&1 | tee -a compile_log
exit
