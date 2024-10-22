#!/bin/bash
# Huaicheng <huaicheng@cs.uchicago.edu>
# Please run this script as root.

SYSTEM=`uname -s`

if [[ -f /etc/debian_version ]]; then
	# Includes Ubuntu, Debian
    apt-get install -y gcc pkg-config git libglib2.0-dev libfdt-dev libpixman-1-dev zlib1g-dev
    apt-get install -y libaio-dev libslirp-dev

	# Additional dependencies
	apt-get install -y libnuma-dev
    apt-get install -y ninja-build

    apt-get install make
    # For python read NAND flash physical data tool - python3 & tkinter, matplotlib
    apt-get install -y python3 python3-tk
    apt-get install -y python3-matplotlib
else
    echo "pkgdep: unsupported system type ($SYSTEM), please install QEMU depencies manually"
	exit 1
fi

echo "===> Dependency installation ... Done!"
