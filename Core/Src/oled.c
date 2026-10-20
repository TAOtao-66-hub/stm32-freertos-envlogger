#include "oled.h"
#include "soft_i2c.h"

/* Private variables: owned exclusively by envMonitorTask. ---------------- */
static uint8_t framebuffer[OLED_WIDTH * OLED_HEIGHT / 8U];
static uint8_t oled_address;

/* Each byte is one vertical column, least significant bit at the top. */
static const uint8_t digits[10][5] = {
    {0x3E,0x51,0x49,0x45,0x3E}, {0x00,0x42,0x7F,0x40,0x00},
    {0x42,0x61,0x51,0x49,0x46}, {0x21,0x41,0x45,0x4B,0x31},
    {0x18,0x14,0x12,0x7F,0x10}, {0x27,0x45,0x45,0x45,0x39},
    {0x3C,0x4A,0x49,0x49,0x30}, {0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36}, {0x06,0x49,0x49,0x29,0x1E}
};
static const uint8_t letters[26][5] = {
    {0x7E,0x11,0x11,0x11,0x7E}, {0x7F,0x49,0x49,0x49,0x36},
    {0x3E,0x41,0x41,0x41,0x22}, {0x7F,0x41,0x41,0x22,0x1C},
    {0x7F,0x49,0x49,0x49,0x41}, {0x7F,0x09,0x09,0x09,0x01},
    {0x3E,0x41,0x49,0x49,0x7A}, {0x7F,0x08,0x08,0x08,0x7F},
    {0x00,0x41,0x7F,0x41,0x00}, {0x20,0x40,0x41,0x3F,0x01},
    {0x7F,0x08,0x14,0x22,0x41}, {0x7F,0x40,0x40,0x40,0x40},
    {0x7F,0x02,0x0C,0x02,0x7F}, {0x7F,0x04,0x08,0x10,0x7F},
    {0x3E,0x41,0x41,0x41,0x3E}, {0x7F,0x09,0x09,0x09,0x06},
    {0x3E,0x41,0x51,0x21,0x5E}, {0x7F,0x09,0x19,0x29,0x46},
    {0x46,0x49,0x49,0x49,0x31}, {0x01,0x01,0x7F,0x01,0x01},
    {0x3F,0x40,0x40,0x40,0x3F}, {0x1F,0x20,0x40,0x20,0x1F},
    {0x3F,0x40,0x38,0x40,0x3F}, {0x63,0x14,0x08,0x14,0x63},
    {0x07,0x08,0x70,0x08,0x07}, {0x61,0x51,0x49,0x45,0x43}
};

/* Private functions ------------------------------------------------------- */
static uint8_t glyph(char c, uint8_t x)
{
    if ((c >= 'a') && (c <= 'z')) { c = (char)(c - 'a' + 'A'); }
    if ((c >= '0') && (c <= '9')) { return digits[c - '0'][x]; }
    if ((c >= 'A') && (c <= 'Z')) { return letters[c - 'A'][x]; }
    switch (c) {
    case ' ': return 0U;
    case ':': return ((x == 1U) || (x == 2U)) ? 0x36U : 0U;
    case '.': return ((x == 1U) || (x == 2U)) ? 0x60U : 0U;
    case '-': return 0x08U;
    case '/': return (uint8_t)(0x40U >> x);
    case '%': {
        static const uint8_t mark[5] = {0x63,0x13,0x08,0x64,0x63};
        return mark[x];
    }
    default: {
        static const uint8_t unknown[5] = {0x02,0x01,0x51,0x09,0x06};
        return unknown[x];
    }
    }
}

/* Public functions -------------------------------------------------------- */
void OLED_Clear(void)
{
    uint16_t i;
    for (i = 0U; i < sizeof(framebuffer); ++i) { framebuffer[i] = 0U; }
}

void OLED_ClearRow(uint8_t row)
{
    uint16_t column;
    uint16_t offset;

    if (row >= OLED_TEXT_ROWS)
    {
        return;
    }

    offset = (uint16_t)row * OLED_WIDTH;
    for (column = 0U; column < OLED_WIDTH; ++column)
    {
        framebuffer[offset + column] = 0U;
    }
}

void OLED_WriteString(uint8_t row, uint8_t column, const char *text)
{
    uint8_t i;
    uint16_t offset;
    if ((row >= OLED_TEXT_ROWS) || (text == 0)) { return; }
    while ((*text != '\0') && (column < OLED_TEXT_COLUMNS)) {
        offset = (uint16_t)(row * OLED_WIDTH + column * 6U);
        for (i = 0U; i < 5U; ++i) { framebuffer[offset + i] = glyph(*text, i); }
        framebuffer[offset + 5U] = 0U;
        ++column;
        ++text;
    }
}

uint8_t OLED_UpdatePage(uint8_t page)
{
    uint8_t position[3];
    uint16_t offset;

    if ((oled_address == 0U) || (page >= OLED_PAGE_COUNT))
    {
        return 0U;
    }

    /* Page addressing: select this page, then start at pixel column zero. */
    position[0] = (uint8_t)(0xB0U + page);
    position[1] = 0x00U;
    position[2] = 0x10U;
    if (SoftI2C_Write(oled_address, 0x00U, position, sizeof(position)) == 0U)
    {
        return 0U;
    }

    offset = (uint16_t)page * OLED_WIDTH;
    return SoftI2C_Write(oled_address, 0x40U, &framebuffer[offset], OLED_WIDTH);
}

uint8_t OLED_Update(void)
{
    uint8_t page;

    for (page = 0U; page < OLED_PAGE_COUNT; ++page)
    {
        if (OLED_UpdatePage(page) == 0U)
        {
            return 0U;
        }
    }
    return 1U;
}

uint8_t OLED_Init(void)
{
    static const uint8_t setup[] = {
        0xAE,             /* Display off during initialization. */
        0xD5, 0x80,       /* Display clock. */
        0xA8, 0x3F,       /* 64-row multiplex. */
        0xD3, 0x00, 0x40, /* Zero offset and start line. */
        0x8D, 0x14,       /* Internal charge pump. */
        0x20, 0x02,       /* Page addressing, matching OLED_Update. */
        0xA1, 0xC8,       /* Segment / COM orientation. */
        0xDA, 0x12,       /* COM pins for 128x64. */
        0x81, 0xCF,       /* Contrast. */
        0xD9, 0xF1, 0xDB, 0x40,
        0xA4, 0xA6        /* Show RAM, normal pixels. */
    };
    static const uint8_t display_on = 0xAFU;
    oled_address = 0U;
    if (SoftI2C_Init() == 0U) { return 0U; }
    if (SoftI2C_Probe(0x3CU) != 0U) {
        oled_address = 0x3CU;
    } else if (SoftI2C_Probe(0x3DU) != 0U) {
        oled_address = 0x3DU;
    } else {
        return 0U;
    }
    if (SoftI2C_Write(oled_address, 0x00U, setup, sizeof(setup)) == 0U) { return 0U; }
    OLED_Clear();
    if (OLED_Update() == 0U) { return 0U; }
    return SoftI2C_Write(oled_address, 0x00U, &display_on, 1U);
}

uint8_t OLED_GetAddress(void)
{
    return oled_address;
}
