/* PL011 on QEMU virt. Runs at EL1 with the MMU off. */
#define UART ((volatile unsigned *)0x09000000UL)
#include "native.h"
#include "version.h"
#ifndef KERNEL_BANNER
#define KERNEL_BANNER "LixinOS native kernel (TCC)"
#endif

void uart_put(unsigned char c) {
    while (UART[0x18 / 4] & (1u << 5)) {
    }
    UART[0] = c;
}
int uart_poll(void) { return UART[0x18 / 4] & (1u << 4) ? -1 : (int)(UART[0] & 255); }
void serial_put(u8 c) {
    if (ssh_mode())
        mux_write(c);
    else
        uart_put(c);
    display_byte(c);
}

void serial_text(const char *text) {
    while (*text) {
        if (*text == '\n')
            serial_put('\r');
        serial_put(*text++);
    }
}

static struct input {
    u16 data[1024];
    unsigned head, tail;
} inputs[2];
void console_pump(void) {
    if (ssh_mode())
        return;
    for (;;) {
        struct input *q = &inputs[focused_pane()];
        if (q->tail - q->head == 1024)
            return;
        int key = keyboard_poll();
        if (key == 257) {
            cycle_focus();
            continue;
        }
        if (key < 0)
            key = uart_poll();
        if (key < 0)
            return;
        q->data[q->tail++ % 1024] = key;
    }
}
int console_ready(void) {
    if (ssh_mode())
        return mux_ready();
    console_pump();
    struct input *q = &inputs[process_pane()];
    return q->head != q->tail || popup_closed();
}
int console_key(void) {
    if (ssh_mode())
        return mux_key();
    console_pump();
    struct input *q = &inputs[process_pane()];
    clipboard_poll();
    int pasted = clipboard_key();
    if (pasted >= 0)
        return pasted;
    if (q->head == q->tail)
        return -1;
    int key = q->data[q->head++ % 1024];
    if (key == 258) {
        selection_copy();
        return -1;
    }
    if (key == 259) {
        clipboard_request(editing_file());
        return -1;
    }
    return key;
}
int console_read(unsigned timeout_ms) {
    u64 start = clock_ticks(), duration = clock_hz() * timeout_ms / 1000;
    do {
        int key = popup_key(console_key());
        if (key >= 0)
            return key;
        display_idle_pointer();
        net_poll();
        if (timeout_ms)
            idle_slice();
    } while (timeout_ms == 0xffffffff || clock_ticks() - start < duration);
    return 256;
}

void kernel_main(void) {
    UART[0x2c / 4] = 3 << 5;
    UART[0x30 / 4] = (1 << 8) | (1 << 9) | 1;
    serial_text(KERNEL_BANNER "\n");
    if (!mount_disk())
        serial_text("Cannot mount system or build volume\n");
    else {
        net_init();
        read_history();
        input_init();
        display_init();
        serial_text("Kernel " KERNEL_RELEASE "\n");
        clipboard_init();
        if (run_login())
            serial_text("Login process failed\n");
        if (!disk_checkpoint())
            serial_text("Build disk checkpoint failed\n");
    }
    power_off();
    for (;;) {
    }
}
