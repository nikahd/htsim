#!/bin/bash -eux

SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )
cd ${SCRIPT_DIR}

# Base directory for outputs
base_outdir="../outputs_fct"

# Parameter lists
# link_speeds=(100 200 400 800)        # Link speed options in Gbps
link_speeds=(800)        # Link speed options in Gbps
queue_sizes=(1)               # Queue size options (BDP multipliers)
random_drop_probs=(0.0 0.001) # Random drop probabilities

# compile htsim
# cd ../htsim/sim
# make clean
# make -j 16
# cd -  # Go back to the previous directory

# Synthetic Workloads
for link_speed in "${link_speeds[@]}"; do
  for queue_size in "${queue_sizes[@]}"; do
    for drop_prob in "${random_drop_probs[@]}"; do
      # Define unique output directory for this combination
      outdir="$base_outdir/pfld_syn_speed_${link_speed}_queue_${queue_size}_drop_${drop_prob}"

      # Run the experiment
      python run_experiment.py -p 32 -o $outdir -t pfld -n 128 \
        --link-speed-gbps $link_speed --queue-size-bdp $queue_size \
        --switch-random-drop-prob $drop_prob --pfld-exp-set 3

      # fct vs msg
      python plot_experiment.py -i $outdir/incast16 --fct-vs-msg-size
      python plot_experiment.py -i $outdir/permutation --fct-vs-msg-size

      # plot
      python plot_normalized_fct.py -i $outdir

    done
  done
done

# AI collectives

# NODES=32
# CONNS=32
# GROUP_SIZE=${NODES}
# PARALLEL=2
# MSG_SIZE=2097152
# # generate matrix
# python ../htsim/sim/datacenter/connection_matrices/gen_serialn_alltoall.py \
#   ./connection_matrices/matrix=serialn_alltoall_${CONNS}.cm \
#   ${NODES} \
#   ${CONNS} \
#   ${GROUP_SIZE} \
#   ${PARALLEL} \
#   ${MSG_SIZE} \
#   0 42

# NODES=32
# CONNS=8
# GROUP_SIZE=${CONNS}
# PARALLEL=2
# MSG_SIZE=2097152
# # generate matrix
# python ../htsim/sim/datacenter/connection_matrices/gen_serialn_alltoall.py \
#   ./connection_matrices/matrix=serialn_alltoall_${CONNS}.cm \
#   ${NODES} \
#   ${CONNS} \
#   ${GROUP_SIZE} \
#   ${PARALLEL} \
#   ${MSG_SIZE} \
#   0 42

# NODES=32
# CONNS=32
# GROUP_SIZE=${NODES}
# MSG_SIZE=2097152
# python ../htsim/sim/datacenter/connection_matrices/gen_allreduce.py \
#   ./connection_matrices/matrix=allreduce.cm \
#   ${NODES} \
#   ${CONNS} \
#   ${GROUP_SIZE} \
#   ${MSG_SIZE} \
#   0 42

# python ../htsim/sim/datacenter/connection_matrices/gen_allreduce_butterfly.py \
#   ./connection_matrices/matrix=allreduce_butterfly.cm \
#   ${NODES} \
#   ${CONNS} \
#   ${GROUP_SIZE} \
#   ${MSG_SIZE} \
#   0 42

for link_speed in "${link_speeds[@]}"; do
  for queue_size in "${queue_sizes[@]}"; do
    for drop_prob in "${random_drop_probs[@]}"; do
      # Define unique output directory for this combination
      outdir="$base_outdir/pfld_ai_speed_${link_speed}_queue_${queue_size}_drop_${drop_prob}"

      # Run the experiment
      # python run_experiment.py -p 32 -o $outdir -t pfld -n 128 \
      #   --link-speed-gbps $link_speed --queue-size-bdp $queue_size \
      #   --switch-random-drop-prob $drop_prob --pfld-exp-set 4
      
      # fct vs msg
      # python plot_experiment.py -i $outdir/serialn_alltoall_4 --fct-vs-msg-size
      # python plot_experiment.py -i $outdir/serialn_alltoall_8 --fct-vs-msg-size
      # python plot_experiment.py -i $outdir/allreduce --fct-vs-msg-size
      # python plot_experiment.py -i $outdir/allreduce_butterfly --fct-vs-msg-size

      # plot
      # python plot_fct.py -i $outdir
    done
  done
done

