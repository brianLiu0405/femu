#!/bin/bash
# Huaicheng <huaicheng@cs.uchicago.edu>
# Copy necessary scripts for running FEMU

FSD="../femu-scripts"

CPL=(pkgdep.sh femu-compile.sh run-whitebox.sh run-blackbox.sh run-nossd.sh run-zns.sh pin.sh gen.sh ftk)

echo ""
echo "==> Copying following FEMU script to current directory:" 2>&1 | tee copy_log
for f in "${CPL[@]}"
do
	if [[ ! -e $FSD/$f ]]; then
		echo "Make sure you are under build-femu/ directory!" 2>&1 | tee -a copy_log
		exit
	fi
	cp -r $FSD/$f . && echo "    --> $f" 2>&1 | tee -a copy_log
done
cp -r ../mySSD . && echo "    --> mySSD" 2>&1 | tee -a copy_log
echo "create physical nand file" 2>&1 | tee -a copy_log
./gen.sh 2>&1 | tee -a copy_log
echo "Done!" 2>&1 | tee -a copy_log
echo "" 2>&1 | tee -a copy_log

