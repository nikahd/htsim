#!/bin/bash -eux

SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )
cd ${SCRIPT_DIR}

# Base directory for outputs
base_outdir="../outputs_probe_overhead"

# Parameter lists
# link_speeds=(100 200 400 800)        # Link speed options in Gbps
link_speeds=(800)        # Link speed options in Gbps
queue_sizes=(1)               # Queue size options (BDP multipliers)
random_drop_probs=(0.0) # Random drop probabilities

# compile htsim
# cd ../htsim/sim
# make clean
# make -j 8
# cd -  # Go back to the previous directory

# Sweep through all parameter combinations
for link_speed in "${link_speeds[@]}"; do
  for queue_size in "${queue_sizes[@]}"; do
    for drop_prob in "${random_drop_probs[@]}"; do
      # Define unique output directory for this combination
      outdir="$base_outdir/pfld_speed_${link_speed}_queue_${queue_size}_drop_${drop_prob}"
      
      # Run the experiment
      # python run_experiment.py -p 32 -o $outdir -t pfld -n 128 \
      #   --link-speed-gbps $link_speed --queue-size-bdp $queue_size \
      #   --switch-random-drop-prob $drop_prob --pfld-exp-set 2
      
      # fct vs msg
      # python plot_experiment.py -i $outdir/incast16 --fct-vs-msg-size
      # python plot_experiment.py -i $outdir/permutation --fct-vs-msg-size
      # python plot_normalized_fct.py -i $outdir

      # trace
      python plot_trace.py -p 32 -i $outdir

      # plot
      # python plot_probe_overhead.py -i $outdir
      python plot_num_rtx.py -i $outdir

      # queue size, link overhead
      # python plot_experiment.py -i $outdir/incast16 --stat-vs-msg-size
      # python plot_experiment.py -i $outdir/permutation --stat-vs-msg-size
    done
  done
done
