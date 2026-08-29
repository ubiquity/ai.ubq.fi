#!/bin/sh
set -e
grep -q '^mode=safe$' src/app.txt
