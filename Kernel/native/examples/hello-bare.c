/* Link with boot.S, which supplies the entry point, stack, and power-off call. */
#define UART ((volatile unsigned *)0x09000000UL)

extern void power_off(void);

void kernel_main(void) {
    UART[0x2c / 4] = 3u << 5;          /* PL011 line control: 8 data bits. */
    UART[0x30 / 4] = (1u << 8) | 1u;   /* Enable the transmitter and UART. */
    for (const char *p = "Hello from bare metal!\r\n"; *p; ++p) {
        while (UART[0x18 / 4] & (1u << 5)) { /* Transmit FIFO full. */
        }
        UART[0] = (unsigned char)*p;
    }
    power_off();
    for (;;) {
    }
}
