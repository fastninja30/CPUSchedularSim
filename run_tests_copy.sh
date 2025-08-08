#!/usr/bin/env bash
set -euo pipefail

# 1) Compile all .c files as C and link math library
gcc -Wall -Werror *.c -o p2.out -lm 
echo "Compiled!"

# test_3 — 5pts
echo "running test_3"
./p2.out 8 6 512 0.001 1024 6 0.9 128 \
  > p2output03actual.txt
vimdiff p2output03-full.txt p2output03actual.txt \
  -c 'TOhtml' -c 'w! diff03.html' -c 'qa!'
