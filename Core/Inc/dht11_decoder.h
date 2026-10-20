#ifndef DHT11_DECODER_H
#define DHT11_DECODER_H

#include <stdint.h>

/* Hardware-independent pulse decoder, also used by the synthetic-frame tests. */
typedef enum {
    DHT_DEC_WAIT_RESPONSE_FALL = 0,
    DHT_DEC_RESPONSE_LOW,
    DHT_DEC_RESPONSE_HIGH,
    DHT_DEC_BIT_LOW,
    DHT_DEC_BIT_HIGH,
    DHT_DEC_COMPLETE,
    DHT_DEC_INVALID
} DHT11_DecodeState;

typedef struct {
    volatile DHT11_DecodeState state;
    volatile uint8_t raw[5];
    uint8_t bit_count;
} DHT11_Decoder;

typedef struct {
    uint16_t temperature_x10;
    uint16_t humidity_x10;
} DHT11_Reading;

typedef enum {
    DHT_FRAME_OK = 0,
    DHT_FRAME_INCOMPLETE,
    DHT_FRAME_CHECKSUM,
    DHT_FRAME_RANGE
} DHT11_FrameResult;

void DHT11_DecoderReset(DHT11_Decoder *decoder);
void DHT11_DecoderEdge(DHT11_Decoder *decoder, uint8_t level, uint32_t elapsed_us);
DHT11_FrameResult DHT11_DecoderResult(const DHT11_Decoder *decoder,
                                    DHT11_Reading *reading);

#endif
