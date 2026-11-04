.globl _start
.p2align 2
_start:
    mov x0, #42
    mov x16, #1
    svc #0x80
