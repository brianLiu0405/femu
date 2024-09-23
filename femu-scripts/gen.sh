#!/bin/bash

# Parameters
pgs_per_blk=256   # Number of pages per flash block
blks_per_pl=256   # Number of blocks per plane
pls_per_lun=1     # Number of planes per LUN (keep it at one, no multiplanes support)
luns_per_ch=8     # Number of LUNs per channel
nchs=8            # Number of channels

# Create directory structure
for ch in $(seq 0 $((nchs - 1))); do
    for lun in $(seq 0 $((luns_per_ch - 1))); do
        for pl in $(seq 0 $((pls_per_lun - 1))); do
            for blk in $(seq 0 $((blks_per_pl - 1))); do
                # Directory path
                dir_path="mySSD/ch${ch}/lun${lun}/pl${pl}/blk${blk}"
                
                # Create the directory if it doesn't exist
                mkdir -p "$dir_path"
                
                # Create page files within the block directory
                # for pg in $(seq 0 $((pgs_per_blk - 1))); do
                #     touch "${dir_path}/page${pg}.txt"
                # done

                echo "Created directory and page files: $dir_path"
            done
        done
    done
done

