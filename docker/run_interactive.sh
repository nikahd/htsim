#!/usr/bin/bash
SIGULARITY_IMAGES_DIR=${SCRATCH}/singularity_images
DOCKERHUB_USER=exc1ted
REPO_NAME=exc1ted
IMAGE_TAG=htsim-test
SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )

singularity pull --dir ${SIGULARITY_IMAGES_DIR} docker://${DOCKERHUB_USER}/${REPO_NAME}:${IMAGE_TAG}

srun --nodes 1 --job-name interactive --time 04:00:00 \
  --ntasks 1 --cpus-per-task 64 --mem 128 \
  --pty singularity shell --shell /bin/bash \
  --bind $SCRATCH:$SCRATCH \
  ${SIGULARITY_IMAGES_DIR}/${REPO_NAME}_${IMAGE_TAG}.sif