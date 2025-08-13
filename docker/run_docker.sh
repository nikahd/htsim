#!/usr/bin/bash

IMAGE_NAME="exc1ted/exc1ted:htsim-test"
SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )

docker run -it --rm --ipc=host \
  --name htsim \
  -v "${SCRIPT_DIR}/..":"/workspace/src" \
  ${IMAGE_NAME}