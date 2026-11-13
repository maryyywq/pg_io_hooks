#!/usr/bin/env bash
#
# Install the packages needed to build PostgreSQL 17 with the patches from
# this repository and to run the tests, on Debian and Ubuntu.
#
# Usage: scripts/install-deps-debian.sh
set -euo pipefail

SUDO=""
if [ "$(id -u)" -ne 0 ]; then
    SUDO="sudo"
fi

$SUDO apt-get update
$SUDO apt-get install -y --no-install-recommends \
    build-essential \
    ca-certificates \
    flex \
    bison \
    git \
    libicu-dev \
    libipc-run-perl \
    libreadline-dev \
    libssl-dev \
    perl \
    pkg-config \
    zlib1g-dev
