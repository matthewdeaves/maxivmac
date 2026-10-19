#!/bin/sh
# Build the ethhub-peer image from a minimal tar context (4 files, not the whole repo).
# Run from anywhere; usage: build.sh [image-name]
set -eu
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
IMG="${1:-ethhub-peer}"
tar -C "$ROOT" -cf - tools/ethhub/ethhub.c docker/ethhub-peer \
    | docker build -t "$IMG" -f docker/ethhub-peer/Dockerfile -
