#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Part of maxivmac (GPL v2, see LICENSE).
# Build the ethhub-peer image from a minimal tar context (4 files, not the whole repo).
# Run from anywhere; usage: build.sh [image-name]
set -eu
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
IMG="${1:-ethhub-peer}"
tar -C "$ROOT" -cf - tools/ethhub/ethhub.c docker/ethhub-peer \
    | docker build -t "$IMG" -f docker/ethhub-peer/Dockerfile -
