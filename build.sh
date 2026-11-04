#!/bin/sh
# Link the emulator at a low address so the guest can use 0x100000000.
cd "$(dirname "$0")" && clang -arch x86_64 -std=gnu2x -O2 -Wall -Wextra -Wno-unused-parameter -Wno-format-nonliteral -Wno-format-security -Wno-deprecated-declarations \
    -Wl,-no_pie -Wl,-pagezero_size,0x1000 -Wl,-image_base,0x10000 \
    rebar.c -lobjc -o rebar
