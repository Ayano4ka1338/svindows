/* Private UP byte queue: producer is a masked IRQ, consumer a DPC. */
#ifndef _HAL_DEFERRED_INPUT_H_
#define _HAL_DEFERRED_INPUT_H_
#include <svindows/Ke.h>
#define HAL_INPUT_BYTES 128
typedef struct _HAL_INPUT_BYTES {
    UINT8 Bytes[HAL_INPUT_BYTES];
    UINT32 Read, Write;
    BOOLEAN Lost;
} HAL_INPUT_BYTE_QUEUE;

static inline void HalQueueInputByte(HAL_INPUT_BYTE_QUEUE *queue, UINT8 byte)
{
    UINT32 next = (queue->Write + 1) % HAL_INPUT_BYTES;
    if (next == queue->Read) {
        /* Broken framing/modifier state must not survive a lost byte. Keep
         * only the new stream; the DPC resets its decoder before consuming. */
        queue->Read = queue->Write = 0;
        queue->Lost = TRUE;
        next = 1;
    }
    queue->Bytes[queue->Write] = byte;
    queue->Write = next;
}

static inline BOOLEAN HalTakeInputByte(HAL_INPUT_BYTE_QUEUE *queue, UINT8 *byte)
{
    if (queue->Read == queue->Write)
        return FALSE;
    *byte = queue->Bytes[queue->Read];
    queue->Read = (queue->Read + 1) % HAL_INPUT_BYTES;
    return TRUE;
}
#endif
