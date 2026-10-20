#include "dht11_decoder.h"

/* Run on Cortex-M3 (or an ARM emulator). Return 0 for success; each set bit
 * identifies the failing scenario below. No HAL, RTOS or attached sensor needed. */
static void feed_frame(DHT11_Decoder *decoder, const uint8_t raw[5], uint8_t bits)
{
    uint8_t bit;
    uint8_t value;
    DHT11_DecoderReset(decoder);
    DHT11_DecoderEdge(decoder, 1U, 5U); /* Host release edge must be ignored. */
    DHT11_DecoderEdge(decoder, 0U, 25U);
    DHT11_DecoderEdge(decoder, 1U, 80U);
    DHT11_DecoderEdge(decoder, 0U, 80U);
    for (bit = 0U; bit < bits; ++bit) {
        value = (uint8_t)((raw[bit / 8U] >> (7U - (bit % 8U))) & 1U);
        DHT11_DecoderEdge(decoder, 1U, (bit & 1U) ? 48U : 53U);
        DHT11_DecoderEdge(decoder, 0U,
                         value ? ((bit & 1U) ? 66U : 75U) : ((bit & 1U) ? 24U : 31U));
    }
}

unsigned int RunDecoderTests(void)
{
    static const uint8_t good[5] = {60U, 5U, 25U, 4U, 94U};
    static const uint8_t bad_crc[5] = {60U, 5U, 25U, 4U, 95U};
    static const uint8_t bad_range[5] = {101U, 0U, 25U, 0U, 126U};
    static const uint8_t bad_decimal[5] = {60U, 10U, 25U, 0U, 95U};
    DHT11_Decoder decoder;
    DHT11_Reading reading;
    uint8_t frame[5];
    unsigned int failures = 0U;
    unsigned int h;
    unsigned int t;

    /* bit 0: mixed bits, timing jitter, and decimal bytes. */
    feed_frame(&decoder, good, 40U);
    if ((DHT11_DecoderResult(&decoder, &reading) != DHT_FRAME_OK) ||
        (reading.temperature_x10 != 254U) || (reading.humidity_x10 != 605U)) {
        failures |= 1U;
    }
    /* bit 1: checksum rejection must not overwrite the previous reading. */
    reading.temperature_x10 = 777U;
    reading.humidity_x10 = 888U;
    feed_frame(&decoder, bad_crc, 40U);
    if ((DHT11_DecoderResult(&decoder, &reading) != DHT_FRAME_CHECKSUM) ||
        (reading.temperature_x10 != 777U) || (reading.humidity_x10 != 888U)) {
        failures |= 2U;
    }
    /* bit 2: a truncated reply cannot become a valid sample. */
    feed_frame(&decoder, good, 39U);
    if (DHT11_DecoderResult(&decoder, &reading) != DHT_FRAME_INCOMPLETE) { failures |= 4U; }
    /* bit 3: no response remains incomplete for the driver's timeout. */
    DHT11_DecoderReset(&decoder);
    if (DHT11_DecoderResult(&decoder, &reading) != DHT_FRAME_INCOMPLETE) { failures |= 8U; }
    /* bit 4: invalid preamble and overlong high pulse. */
    DHT11_DecoderEdge(&decoder, 0U, 25U);
    DHT11_DecoderEdge(&decoder, 1U, 20U);
    if (decoder.state != DHT_DEC_INVALID) { failures |= 16U; }
    feed_frame(&decoder, good, 0U);
    DHT11_DecoderEdge(&decoder, 1U, 50U);
    DHT11_DecoderEdge(&decoder, 0U, 150U);
    if (decoder.state != DHT_DEC_INVALID) { failures |= 16U; }
    /* bit 5: valid checksum cannot bypass range / decimal validation. */
    feed_frame(&decoder, bad_range, 40U);
    if (DHT11_DecoderResult(&decoder, &reading) != DHT_FRAME_RANGE) { failures |= 32U; }
    feed_frame(&decoder, bad_decimal, 40U);
    if (DHT11_DecoderResult(&decoder, &reading) != DHT_FRAME_RANGE) { failures |= 32U; }
    /* bit 6: a completed frame is unchanged by the trailing release edge. */
    feed_frame(&decoder, good, 40U);
    DHT11_DecoderEdge(&decoder, 1U, 50U);
    if ((decoder.state != DHT_DEC_COMPLETE) ||
        (DHT11_DecoderResult(&decoder, &reading) != DHT_FRAME_OK)) { failures |= 64U; }
    /* bit 7: a grid includes all-zero, high humidity and repeated transactions. */
    for (h = 0U; h <= 100U; h += 10U) {
        for (t = 0U; t <= 50U; t += 5U) {
            frame[0] = (uint8_t)h;
            frame[1] = ((h == 100U) || (h == 0U)) ? 0U : 3U;
            frame[2] = (uint8_t)t;
            frame[3] = 0U;
            frame[4] = (uint8_t)(frame[0] + frame[1] + frame[2]);
            feed_frame(&decoder, frame, 40U);
            if ((DHT11_DecoderResult(&decoder, &reading) != DHT_FRAME_OK) ||
                (reading.humidity_x10 != h * 10U + frame[1]) ||
                (reading.temperature_x10 != t * 10U)) { failures |= 128U; }
        }
    }
    return failures;
}
