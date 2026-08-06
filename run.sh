#!/bin/bash
set -euo pipefail

# Helper for running Python validation utilities in the same analysis container
# used by the ParTau training/export workflow. Override MLTAU_PYTHON_IMAGE if
# your site uses a different image.
export KERAS_BACKEND=${KERAS_BACKEND:-torch}

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
IMAGE=${MLTAU_PYTHON_IMAGE:-/home/software/singularity/pytorch.simg:2025-09-01}
BIND_PATHS=${MLTAU_BIND_PATHS:-/scratch/persistent,/local,/home}
PYTHONPATH_EXTRA=${MLTAU_PYTHONPATH:-${SCRIPT_DIR}:${SCRIPT_DIR}/ml-tau-data}

apptainer exec \
  -B "${BIND_PATHS}" \
  --env PYTHONPATH="${PYTHONPATH_EXTRA}" \
  --nv \
  "${IMAGE}" \
  "$@"
