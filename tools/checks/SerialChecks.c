/* Host regression: exercise the actual UART driver with simulated port I/O.
 * cc -Ikernel/include tools/checks/SerialChecks.c -o /tmp/SerialChecks
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#define _SVINDOWS_HAL_H_
typedef uint8_t UINT8;
typedef uint16_t UINT16;
typedef uint32_t UINT32;
typedef int BOOLEAN;
#define TRUE 1
#define FALSE 0
static UINT8 registers[8];
static int absent, tx_count;
static UINT8 __inbyte(UINT16 port)
{
    return absent ? 0xff : registers[port - 0x3f8];
}
static void __outbyte(UINT16 port, UINT8 value)
{
    if (!absent) {
        registers[port - 0x3f8] = value;
        if (port == 0x3f8) ++tx_count;
    }
}
static void __pause(void) {}
#include "../../kernel/hal/Serial.c"
int main(void)
{
    absent = 1;
    HalInitializeSerial();
    assert(!HalSerialDataAvailable());
    assert(HalSerialReadChar() == 0);
    HalSerialPutChar('x');
    absent = 0;
    registers[UART_SCR] = 0x12;
    registers[UART_LSR] = LSR_THR_EMPTY;
    HalInitializeSerial();
    assert(g_serial_ready && registers[UART_SCR] == 0x12);
    tx_count = 0;
    HalSerialPutChar('\n');
    assert(tx_count == 2);
    registers[UART_DATA] = 'k';
    registers[UART_LSR] |= LSR_DATA_READY;
    assert(HalSerialReadChar() == 'k');
    absent = 1; /* device disappears after probing */
    assert(!HalSerialDataAvailable());
    absent = 0;
    registers[UART_LSR] = 0; /* transmitter never becomes ready */
    HalInitializeSerial();
    HalSerialPutChar('x');
    assert(!g_serial_ready);
    puts("PASS absent UART, normal RX/TX, disappearing UART, stuck transmitter");
}
