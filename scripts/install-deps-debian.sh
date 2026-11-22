#!/usr/bin/env bash
#
# Install the packages needed to build PostgreSQL 17, 18 or 19 with the
# patches from this repository and to run the tests, on Debian and Ubuntu.
# liburing is needed for io_method = io_uring on PostgreSQL 18 and later.
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
    liburing-dev \
    perl \
    pkg-config \
    shellcheck \
    zlib1g-dev
