#!/bin/sh
set -e
test -z "$(git status --porcelain)"
grep -q 'added note 1' notes/CHANGES.txt
grep -q 'added note 2' notes/CHANGES.txt
