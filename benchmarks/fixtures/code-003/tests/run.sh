#!/bin/sh
set -e
grep -q 'range(1, max_index + 1)' src/loop.txt
