#!/bin/sh
set -e
grep -q 'return last + ", " + first' src/format.txt
