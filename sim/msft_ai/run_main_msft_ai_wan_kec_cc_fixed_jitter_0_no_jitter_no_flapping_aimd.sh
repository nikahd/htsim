#!/bin/bash
 
# CONN_MATRICES=("one_one_1_200MB.cm" "one_one_2_200MB.cm" "one_one_4_200MB.cm" "one_one_8_200MB.cm" "one_one_16_200MB.cm" "one_one_32_200MB.cm" "one_one_64_200MB.cm" "one_one_128_200MB.cm" "one_one_256_200MB.cm")
CONN_MATRICES=("4_1_4_2GB.cm")
IS_LINK_DOWN=(0)
drop_rates=("0")
use_jitter=0
enable_pfc=0

INIT_CWND_RATIO=(0.7) # 70%, 80%, 90%
RECOVERABLE_THRESHOLD=(1) # recoverable threshold
bitmap_size_list=(1024)
fullskip=(0) 
as_fast_recoverable=(0) # 0: no, 1: yes
bitmap_full_percent=(0.9) 
use_replace_path=(0) 
original_loss_path_replace_threshold=(0)
original_jittery_path_replace_threshold=(0)
apply_mimd=0

experiment_run=(1 2 3 4 5)

folder_name="msft_ai_wan_kec_cc"
binary_name="$folder_name"

mkdir -p "$folder_name"

for MATRIX in "${CONN_MATRICES[@]}"; do
    for exp in "${experiment_run[@]}"; do
        for drop_rate in "${drop_rates[@]}"; do
            for bitmapsize in "${bitmap_size_list[@]}"; do
                for CWND_RATIO in "${INIT_CWND_RATIO[@]}"; do
                    for as_fast_recoverable in "${as_fast_recoverable[@]}"; do
                        for percent in "${bitmap_full_percent[@]}"; do
                            for isfullskip in "${fullskip[@]}"; do
                                for rec in "${RECOVERABLE_THRESHOLD[@]}"; do
                                    for use_replace in "${use_replace_path[@]}"; do
                                        if [ "$use_replace" -eq 0 ]; then 
                                            loss_path_replace_threshold=(0)
                                            jittery_path_replace_threshold=(0)
                                        else
                                            loss_path_replace_threshold=("${original_loss_path_replace_threshold[@]}")
                                            jittery_path_replace_threshold=("${original_jittery_path_replace_threshold[@]}")
                                        fi
                                        for loss_replace_percent in "${loss_path_replace_threshold[@]}"; do
                                            for jittery_path_replace in "${jittery_path_replace_threshold[@]}"; do
                                                for ((j=0; j<${#IS_LINK_DOWN[@]}; j++)); do
                                                    LINK_DOWN=${IS_LINK_DOWN[$j]}

                                                    CONN_MATRIX="./scripts/msft_ai_connection_matrices/${MATRIX}"
                                                    BASENAME=$(basename "$CONN_MATRIX")
                                                    SUFFIX="_linkdown${LINK_DOWN}_droprate${drop_rate}_usejitter${use_jitter}_initcwnd${CWND_RATIO}_bitmapsize${bitmapsize}_recoverable${rec}_isfullskip${isfullskip}_fastrecoverable${as_fast_recoverable}_bitmapfulllossy1_fullpercent${percent}_lossreplace${loss_replace_percent}_jitterreplace${jittery_path_replace}_mimd${apply_mimd}_pfc${enable_pfc}_exp${exp}"
                                                    OUTFILE="$folder_name/output_${BASENAME%.*}${SUFFIX}.txt"
                                                    STATISTICS_FILENAME="$folder_name/statistics_${BASENAME%.*}${SUFFIX}.txt"

                                                    cmd="./build/${binary_name} -o uec_entry -switch_latency 0 -collect_data 1 -strat ecmp_host \
-tm ${CONN_MATRIX} -noFi -noQaInter -noQaIntra -noRto \
-drop-rate ${drop_rate} -use-jitter ${use_jitter} \
-interQSize 4000000 -intraQSize 200000000 -is-link-down ${LINK_DOWN} \
-init-cwnd ${CWND_RATIO} -recoverable-threshold ${rec} \
-statistics-filename ${STATISTICS_FILENAME} \
-use-full-skip ${isfullskip} -use-as-fast-as-possible-recoverable-skip ${as_fast_recoverable} -bitmap_full_percent ${percent} \
-use-replace-path ${use_replace} -loss-path-replace-threshold ${loss_replace_percent} -jittery-path-replace-threshold ${jittery_path_replace} \
-apply-mimd ${apply_mimd} -enable-pfc ${enable_pfc}"

                                                    echo "Executing command:"
                                                    echo "$cmd"

                                                    echo "Statistics will be written to: $STATISTICS_FILENAME"
                                                    echo "----------------------------------------"

                                                    # $cmd > /dev/null 2> "$OUTFILE" # only store errors
                                                    $cmd > "$OUTFILE"

                                                    if [ $? -ne 0 ]; then
                                                        echo "Command timed out or failed"
                                                    else
                                                        echo "Command completed successfully"
                                                    fi
                                                    echo "----------------------------------------"
                                                done
                                            done
                                        done
                                    done
                                done
                            done
                        done
                    done
                done
            done
        done
    done
done