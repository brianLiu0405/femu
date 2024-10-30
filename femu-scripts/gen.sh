#!/bin/bash

pgs_per_blk=256
blks_per_pl=256
pls_per_lun=1
luns_per_ch=4
nchs=8

total_blocks=$((nchs * luns_per_ch * pls_per_lun * blks_per_pl))
bar_length=40

progress_file="progress.log"
echo 0 > "$progress_file"

print_progress() {
    current_block=$(cat "$progress_file")
    local progress=$((current_block * 100 / total_blocks))
    local filled_length=$((bar_length * current_block / total_blocks))

    local bar=""
    for ((i=0; i<filled_length; i++)); do
        bar+="#"
    done
    for ((i=filled_length; i<bar_length; i++)); do
        bar+="-"
    done

    printf "\rProgress: |%s| %d%% (%d/%d Blocks)" "$bar" "$progress" "$current_block" "$total_blocks"
}

create_block() {
    ch=$1
    lun=$2
    pl=$3
    blk=$4
    dir_path="mySSD/ch${ch}/lun${lun}/pl${pl}/blk${blk}"
    mkdir -p "$dir_path"
    printf "%s\n" $(seq 0 $((pgs_per_blk - 1)) | awk '{print "'$dir_path'/pg"$0}') | xargs touch

    (
        flock -x 200
        current_block=$(cat "$progress_file")
        current_block=$((current_block + 1))
        echo "$current_block" > "$progress_file"
    ) 200>"$progress_file.lock"

    print_progress
}

for ch in $(seq 0 $((nchs - 1))); do
    for lun in $(seq 0 $((luns_per_ch - 1))); do
        for pl in $(seq 0 $((pls_per_lun - 1))); do
            for blk in $(seq 0 $((blks_per_pl - 1))); do
                create_block "$ch" "$lun" "$pl" "$blk" &
            
                if [[ $((blk % 100)) -eq 0 ]]; then
                    wait
                fi
            done
        done
    done
done

wait

echo -e "\nAll directories and files created."
