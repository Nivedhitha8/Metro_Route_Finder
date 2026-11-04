#!/usr/bin/env bash
# Builds and runs Metro Route Finder on http://localhost:8080 (needs gcc).
cd "$(dirname "$0")" && gcc -O2 -o metro metro.c && ./metro
