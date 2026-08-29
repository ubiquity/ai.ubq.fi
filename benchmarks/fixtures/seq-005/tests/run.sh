#!/bin/sh
set -e
grep -q '^hi world$' src/sample.txt
grep -q '^keep me$' src/other.txt
