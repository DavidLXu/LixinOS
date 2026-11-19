.text
.global _start
_start:
    // msr daifset, #15 (TinyCC only accepts register-form MSR).
    .long 0xd5034fdf
    bl set_boot_stack
    .quad boot_stack_top
set_boot_stack:
    ldr x0, [x30]
    mov sp, x0
    b kernel_main
.global power_off
power_off:
    movz x0, #0x8400, lsl #16
    movk x0, #8
    .long 0xd4000002 // hvc #0, PSCI SYSTEM_OFF
    ret
.bss
.balign 16
    .skip 131072
boot_stack_top:
