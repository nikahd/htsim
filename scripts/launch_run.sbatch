#!/bin/bash
#SBATCH --job-name=pfld-sweep             # Job name
#SBATCH --output=./slurm_out/job_%j_out.txt   # Output file (job ID and task number)
#SBATCH --error=./slurm_out/job_%j_err.txt    # Error file (job ID and task number)
#SBATCH --nodes=1                         # Number of nodes
#SBATCH --ntasks=1                        # Number of tasks (1 for single task)
#SBATCH --cpus-per-task=64                # Number of CPU cores per task
#SBATCH --mem=128G                        # Memory allocation
#SBATCH --time=04:00:00                   # Time limit (4 hours in this case)

# Print detailed job metadata
JOB_TIME_LIMIT=$(squeue -j $SLURM_JOB_ID -h --Format TimeLimit)
CURRENT_TIMESTAMP_FULL=$(date --utc)
CURRENT_TIMESTAMP_SHORT=$(date --utc -d "$CURRENT_TIMESTAMP_FULL" +"%Y%m%d_%H%M%S")
echo "================== JOB METADATA =================="
echo "Job ID: $SLURM_JOB_ID"
echo "Job Name: $SLURM_JOB_NAME"
echo "User ID: $SLURM_JOB_USER"
echo "Job Submit Directory: $SLURM_SUBMIT_DIR"
echo "Node List: $SLURM_JOB_NODELIST"
echo "Partition: $SLURM_JOB_PARTITION"
echo "Number of Nodes: $SLURM_JOB_NUM_NODES"
echo "CPUs per Task: $SLURM_CPUS_PER_TASK"
echo "Memory Allocated: $SLURM_MEM_PER_NODE"
echo "GPUs Allocated: $SLURM_GPUS"
echo "Time Limit: $JOB_TIME_LIMIT"
echo "Job Start Time: $CURRENT_TIMESTAMP_FULL"
echo "==================================================="

SIGULARITY_IMAGES_DIR=${SCRATCH}/singularity_images
DOCKERHUB_USER=exc1ted
REPO_NAME=exc1ted
IMAGE_TAG=htsim-test
SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )

CONDA_SETUP="
  source /opt/miniconda3/etc/profile.d/conda.sh
  conda activate base
"

singularity exec \
  --bind $SCRATCH:$SCRATCH \
  ${SIGULARITY_IMAGES_DIR}/${REPO_NAME}_${IMAGE_TAG}.sif \
  bash -c "
    ${CONDA_SETUP}
    cd ./scripts
    ./run.sh
  "

wait # Wait for all background srun processes to finish