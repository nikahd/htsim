#!/bin/bash

system_baselines=("1ecbr" "kecbr")
drop_rates=("0" "p99")
use_jitters=("no_jitter" "use_jitter")
flapping=("no_flapping" "use_flapping")

for baseline in "${system_baselines[@]}"; do
    for drop_rate in "${drop_rates[@]}"; do
        for use_jitter in "${use_jitters[@]}"; do
            for flap in "${flapping[@]}"; do
                # Construct the script name based on the parameters
                script_name="run_main_msft_ai_wan_${baseline}_fixed_jitter_${drop_rate}_${use_jitter}_${flap}.sh"
                cmd="bash ./sim/msft_ai/${script_name}"

                if [ -e ./sim/msft_ai/${script_name} ]; then
                    # Define the session name
                    SESSION_NAME="${baseline}_${drop_rate}_${use_jitter}_${flap}"

                    # Check if the session already exists
                    tmux has-session -t $SESSION_NAME 2>/dev/null

                    if [ $? != 0 ]; then
                        # Create a new tmux session
                        tmux new-session -d -s $SESSION_NAME $cmd
                        echo "Started tmux session: $SESSION_NAME"
                    fi
                fi

            done
        done
    done
done