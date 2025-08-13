#!/bin/bash

CONN_MATRICES=("one_one_1_200MB.cm" "one_one_2_200MB.cm" "one_one_4_200MB.cm" "one_one_8_200MB.cm" "one_one_16_200MB.cm" "one_one_32_200MB.cm" "one_one_64_200MB.cm" "one_one_128_200MB.cm" "one_one_256_200MB.cm")
IS_LINK_DOWN=(0)
drop_rates=("0")
use_jitter=0
experiment_run=(1)

folder_name="msft_ai_wan_single_dcqcn"
binary_name="$folder_name"

mkdir -p "$folder_name"

for MATRIX in "${CONN_MATRICES[@]}"; do
    for exp in "${experiment_run[@]}"; do
        for drop_rate in "${drop_rates[@]}"; do
            for ((j=0; j<${#IS_LINK_DOWN[@]}; j++)); do
                LINK_DOWN=${IS_LINK_DOWN[$j]}

                CONN_MATRIX="./scripts/msft_ai_connection_matrices/${MATRIX}"
                BASENAME=$(basename "$CONN_MATRIX")
                SUFFIX="_linkdown${LINK_DOWN}_droprate${drop_rate}_usejitter${use_jitter}_exp${exp}"
                OUTFILE="$folder_name/output_${BASENAME%.*}${SUFFIX}.txt"
                STATISTICS_FILENAME="$folder_name/statistics_${BASENAME%.*}${SUFFIX}.txt"

                cmd="./build/${binary_name} -o uec_entry -switch_latency 0 -collect_data 1 -strat ecmp_host \
-tm ${CONN_MATRIX} -noFi -noQaInter -noQaIntra -noRto \
-drop-rate ${drop_rate} -use-jitter ${use_jitter} \
-interQSize 4000000 -intraQSize 200000000 -is-link-down ${LINK_DOWN} \
-statistics-filename ${STATISTICS_FILENAME}"

                echo "Executing command:"
                echo "$cmd"

                echo "Statistics will be written to: $STATISTICS_FILENAME"
                echo "----------------------------------------"

                $cmd > /dev/null 2> "$OUTFILE" # only store errors
                # $cmd > "$OUTFILE"

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
 
