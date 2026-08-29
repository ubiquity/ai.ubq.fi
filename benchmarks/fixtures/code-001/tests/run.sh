#!/bin/sh
set -e
grep -q '^PORT = 9000$' src/config.txt
