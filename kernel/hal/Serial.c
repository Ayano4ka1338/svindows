/*
 * hal/Serial.c - 16550 UART driver for COM1.
 *
 * This is the kernel's primary log sink: under QEMU the emulated COM1 is wired
 * to stdout (`-serial stdio`), so everything the kernel prints shows up in the
 * host terminal even before there is any display driver.
 */
#include <svindows/Hal.h>

#define COM1_BASE 0x3F8

/* Register offsets from the port base. */
#define UART_DATA        0 /* DLAB=0: data; DLAB=1: divisor low  */
#define UART_IER         1 /* DLAB=0: interrupt enable; DLAB=1: divisor high */
#define UART_FCR         2 /* write: FIFO control */
#define UART_LCR         3 /* line control */
#define UART_MCR         4 /* modem control */
#define UART_LSR         5 /* line status */
#define UART_SCR         7 /* scratch register */
#define UART_TX_SPINS    100000u

#define LSR_THR_EMPTY    0x20 /* transmit holding register empty */
#define LSR_DATA_READY   0x01 /* receive data register has a byte */

static BOOLEAN g_serial_ready = FALSE;

void HalInitializeSerial(void)
{
    g_serial_ready = FALSE;
    /* An unpopulated ISA port normally reads as 0xff. Without probing, the
     * input worker mistakes that for an endless stream of received bytes. */
    UINT8 scratch = __inbyte(COM1_BASE + UART_SCR);
    __outbyte(COM1_BASE + UART_SCR, 0x55);
    BOOLEAN present = __inbyte(COM1_BASE + UART_SCR) == 0x55;
    __outbyte(COM1_BASE + UART_SCR, 0xAA);
    present = present && __inbyte(COM1_BASE + UART_SCR) == 0xAA;
    __outbyte(COM1_BASE + UART_SCR, scratch);
    if (!present || __inbyte(COM1_BASE + UART_LSR) == 0xFF)
        return;

    __outbyte(COM1_BASE + UART_IER, 0x00); /* disable interrupts           */
    __outbyte(COM1_BASE + UART_LCR, 0x80); /* enable DLAB to set baud rate */
    __outbyte(COM1_BASE + UART_DATA, 0x01);/* divisor low  = 1 (115200 baud) */
    __outbyte(COM1_BASE + UART_IER, 0x00); /* divisor high = 0             */
    __outbyte(COM1_BASE + UART_LCR, 0x03); /* 8 bits, no parity, one stop  */
    __outbyte(COM1_BASE + UART_FCR, 0xC7); /* enable + clear FIFOs, 14-byte */
    __outbyte(COM1_BASE + UART_MCR, 0x0B); /* DTR, RTS, OUT2 (irq gate)    */

    g_serial_ready = TRUE;
}

static BOOLEAN serial_wait_tx(void)
{
    for (UINT32 spin = 0; spin < UART_TX_SPINS; ++spin) {
        UINT8 status = __inbyte(COM1_BASE + UART_LSR);
        if (status == 0xFF) break;
        if (status & LSR_THR_EMPTY) return TRUE;
        __pause();
    }
    /* Logging can run with interrupts disabled. A broken UART must never
     * stop the scheduler or hold the kernel log lock forever. */
    g_serial_ready = FALSE;
    return FALSE;
}

void HalSerialPutChar(char c)
{
    if (!g_serial_ready)
        return;

    if (c == '\n') {
        if (!serial_wait_tx()) return;
        __outbyte(COM1_BASE + UART_DATA, '\r');
    }
    if (!serial_wait_tx()) return;
    __outbyte(COM1_BASE + UART_DATA, (UINT8)c);
}

BOOLEAN HalSerialDataAvailable(void)
{
    if (!g_serial_ready) return FALSE;
    UINT8 status = __inbyte(COM1_BASE + UART_LSR);
    if (status == 0xFF) {
        g_serial_ready = FALSE;
        return FALSE;
    }
    return (status & LSR_DATA_READY) != 0;
}

char HalSerialReadChar(void)
{
    return HalSerialDataAvailable()
        ? (char)__inbyte(COM1_BASE + UART_DATA) : 0;
}
