#!/usr/bin/env bash

SCRIPTS_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
source $SCRIPTS_DIR/common-functions.sh

PREFIX=/usr/local/mapd-deps
SUFFIX=${SUFFIX:=$(date +%Y%m%d)}

# TODO: move to common-functions

mkdir "tmp${SUFFIX}"
cd "tmp${SUFFIX}"

# level_zero
L0_VERSION="1.0.22"
wget "https://github.com/oneapi-src/level-zero/releases/download/v${L0_VERSION}/level-zero-devel_${L0_VERSION}+u18.04_amd64.deb"
wget "https://github.com/oneapi-src/level-zero/releases/download/v${L0_VERSION}/level-zero_${L0_VERSION}+u18.04_amd64.deb"

#dpkg -i "level-zero-devel_${L0_VERSION}+u18.04_amd64.deb"
#dpkg -i "level-zero_${L0_VERSION}+u18.04_amd64.deb" 

# compute runtime
COMPUTE_VERSION="21.01.18793"
GMM_VERSION="20.3.2"
IGC_VERSION="1.0.5964"
L0GPU_VERSION="1.0.18793"

wget https://github.com/intel/compute-runtime/releases/download/${COMPUTE_VERSION}/intel-gmmlib_${GMM_VERSION}_amd64.deb
wget https://github.com/intel/intel-graphics-compiler/releases/download/igc-${IGC_VERSION}/intel-igc-core_${IGC_VERSION}_amd64.deb
wget https://github.com/intel/intel-graphics-compiler/releases/download/igc-${IGC_VERSION}/intel-igc-opencl_${IGC_VERSION}_amd64.deb
wget https://github.com/intel/compute-runtime/releases/download/${COMPUTE_VERSION}/intel-opencl_${COMPUTE_VERSION}_amd64.deb
wget https://github.com/intel/compute-runtime/releases/download/${COMPUTE_VERSION}/intel-ocloc_${COMPUTE_VERSION}_amd64.deb
wget https://github.com/intel/compute-runtime/releases/download/${COMPUTE_VERSION}/intel-level-zero-gpu_${L0GPU_VERSION}_amd64.deb

# install all
sudo dpkg -i *.deb
rm *.deb
cd ..
rmdir "tmp${SUFFIX}"
