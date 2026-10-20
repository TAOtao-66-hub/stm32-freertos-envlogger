#include "dht11_decoder.h"

void DHT11_DecoderReset(DHT11_Decoder *decoder)
{
    uint8_t i;
    for (i = 0U; i < 5U; ++i) { decoder->raw[i] = 0U; }
    decoder->bit_count = 0U;
    decoder->state = DHT_DEC_WAIT_RESPONSE_FALL;
}

void DHT11_DecoderEdge(DHT11_Decoder *decoder, uint8_t level, uint32_t elapsed_us)
{
    uint8_t index;
    switch (decoder->state) {
    case DHT_DEC_WAIT_RESPONSE_FALL:
        if (level == 0U) { decoder->state = DHT_DEC_RESPONSE_LOW; }
        break;
    case DHT_DEC_RESPONSE_LOW:
        if ((level != 1U) || (elapsed_us < 50U) || (elapsed_us > 120U)) {
            decoder->state = DHT_DEC_INVALID;
        } else {
            decoder->state = DHT_DEC_RESPONSE_HIGH;
        }
        break;
    case DHT_DEC_RESPONSE_HIGH:
        if ((level != 0U) || (elapsed_us < 50U) || (elapsed_us > 120U)) {
            decoder->state = DHT_DEC_INVALID;
        } else {
            decoder->state = DHT_DEC_BIT_LOW;
        }
        break;
    case DHT_DEC_BIT_LOW:
        if ((level != 1U) || (elapsed_us < 25U) || (elapsed_us > 85U)) {
            decoder->state = DHT_DEC_INVALID;
        } else {
            decoder->state = DHT_DEC_BIT_HIGH;
        }
        break;
    case DHT_DEC_BIT_HIGH:
        if ((level != 0U) || (elapsed_us < 10U) || (elapsed_us > 110U)) {
            decoder->state = DHT_DEC_INVALID;
            break;
        }
        index = decoder->bit_count / 8U;
        decoder->raw[index] = (uint8_t)((decoder->raw[index] << 1) |
                                      ((elapsed_us > 50U) ? 1U : 0U));
        ++decoder->bit_count;
        decoder->state = (decoder->bit_count == 40U) ? DHT_DEC_COMPLETE : DHT_DEC_BIT_LOW;
        break;
    default:
        /* A completed or invalid frame is immutable until the next reset. */
        break;
    }
}

DHT11_FrameResult DHT11_DecoderResult(const DHT11_Decoder *decoder,
                                    DHT11_Reading *reading)
{
    uint8_t checksum;
    uint16_t temperature;
    uint16_t humidity;
    if ((decoder->state != DHT_DEC_COMPLETE) || (reading == 0)) {
        return DHT_FRAME_INCOMPLETE;
    }
    checksum = (uint8_t)(decoder->raw[0] + decoder->raw[1] +
                         decoder->raw[2] + decoder->raw[3]);
    if (checksum != decoder->raw[4]) {
        return DHT_FRAME_CHECKSUM;
    }
    /* DHT11 uses integer + decimal bytes, unlike the DHT22 16-bit format. */
    if ((decoder->raw[1] > 9U) || (decoder->raw[3] > 9U)) {
        return DHT_FRAME_RANGE;
    }
    temperature = (uint16_t)(decoder->raw[2] * 10U + decoder->raw[3]);
    humidity = (uint16_t)(decoder->raw[0] * 10U + decoder->raw[1]);
    if ((temperature > 800U) || (humidity > 1000U)) {
        return DHT_FRAME_RANGE;
    }
    reading->temperature_x10 = temperature;
    reading->humidity_x10 = humidity;
    return DHT_FRAME_OK;
}
