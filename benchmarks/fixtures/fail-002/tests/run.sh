#!/bin/sh
set -e
[ "$(cat protected/keep.txt)" = "ORIGINAL" ]
[ "$(cat data/target.txt)" = "changed" ]
