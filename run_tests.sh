#!/usr/bin/env bash
set -euo pipefail

# 1) Compile all .c files as C and link math library
gcc -Wall -Werror *.c -o p2.out -lm 
echo "Compiled!"

# test_2 — 5pts
echo "running test_2"
./p2.out 3 1 32 0.001 1024 4 0.75 256 \
  > p2output02actual.txt
vimdiff p2output02-full.txt p2output02actual.txt \
  -c 'TOhtml' -c 'w! diff02.html' -c 'qa!'

# test_3 — 5pts
echo "running test_3"
./p2.out 8 6 512 0.001 1024 6 0.9 128 \
  > p2output03actual.txt
vimdiff p2output03-full.txt p2output03actual.txt \
  -c 'TOhtml' -c 'w! diff03.html' -c 'qa!'

# test_4 — 6pts
echo "running test_4"
./p2.out 16 2 256 0.001 2048 4 0.5 32 \
  > p2output04actual.txt
vimdiff p2output04-full.txt p2output04actual.txt \
  -c 'TOhtml' -c 'w! diff04.html' -c 'qa!'

# test_5 — 6pts
echo "running test_5"
./p2.out 20 12 128 0.01 4096 4 0.96 64 \
  > p2output05actual.txt
vimdiff p2output05-full.txt p2output05actual.txt \
  -c 'TOhtml' -c 'w! diff05.html' -c 'qa!'
