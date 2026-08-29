#!/bin/sh
set -e
for f in f01 f02 f03 f04 f05 f06 f07 f08; do
  grep -q '^x=1$' "$f.txt"
done
