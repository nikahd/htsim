FROM ubuntu:22.04

# Set environment variables.
## Set non-interactive to prevent asking for user inputs blocking image creation.
ENV DEBIAN_FRONTEND=noninteractive
## Set timezone as it is required by some packages.
ENV TZ=UTC

SHELL ["/bin/bash", "-c"]
WORKDIR /workspace

# Install basic apt packages.
RUN apt-get update && \
    apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    git \
    nano \
    vim \
    wget \
    curl

# Install conda
ARG CONDA_INSTALL_DIR=/opt/miniconda3
RUN mkdir -p ${CONDA_INSTALL_DIR}
RUN wget --no-check-certificate https://repo.anaconda.com/miniconda/Miniconda3-latest-Linux-x86_64.sh -O ${CONDA_INSTALL_DIR}/miniconda.sh
RUN bash ${CONDA_INSTALL_DIR}/miniconda.sh -b -u -p ${CONDA_INSTALL_DIR}
RUN rm -rf ${CONDA_INSTALL_DIR}/miniconda.sh
# RUN ${CONDA_INSTALL_DIR}/bin/conda init bash

# Update conda and install packages
RUN ${CONDA_INSTALL_DIR}/bin/conda update -y conda && \
    ${CONDA_INSTALL_DIR}/bin/conda install -y python=3.9 pip

RUN ${CONDA_INSTALL_DIR}/bin/pip install \
    mmengine-lite==0.10.4 scikit-learn==1.5.0 tqdm==4.65.0 \
    pyarrow==16.1.0 numpy==1.26.4 matplotlib==3.9.0 pandas==2.2.2

RUN apt-get update && \
    apt-get install -y --no-install-recommends \
    g++ \
    gnuplot

# Add conda activation to all users' bashrc
RUN echo ". ${CONDA_INSTALL_DIR}/etc/profile.d/conda.sh" >> /etc/bash.bashrc && \
    echo "conda activate base" >> /etc/bash.bashrc

ENTRYPOINT /bin/bash
