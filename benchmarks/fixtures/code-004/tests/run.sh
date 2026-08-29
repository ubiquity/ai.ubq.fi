#!/bin/sh
set -e
[ "$(grep -c "def dup_" src/utils.txt)" -eq 1 ]
