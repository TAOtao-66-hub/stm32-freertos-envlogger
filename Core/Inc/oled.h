#ifndef OLED_H
#define OLED_H

#include <stdint.h>

#define OLED_WIDTH 128U
#define OLED_HEIGHT 64U
#define OLED_PAGE_COUNT (OLED_HEIGHT / 8U)
#define OLED_TEXT_ROWS OLED_PAGE_COUNT
#define OLED_TEXT_COLUMNS 21U

/* GM009605 V4.3: SSD1306 128x64, PB6=SCL / PB7=SDA (software I2C).
 * Text is a compact 5x7 ASCII font: uppercase, digits and common punctuation.
 * One text row occupies one 8-pixel page. Draw into the SRAM framebuffer,
 * then call OLED_UpdatePage for one page or OLED_Update for the full screen.
 * All drawing and transfer functions must be called by the same task. */
uint8_t OLED_Init(void);
void OLED_Clear(void);
/* Clear one text row in SRAM only; row range: 0..OLED_TEXT_ROWS-1. */
void OLED_ClearRow(uint8_t row);
void OLED_WriteString(uint8_t row, uint8_t column, const char *text);
/* Send one 128-byte page; return 1 on success, 0 on invalid input/I2C error. */
uint8_t OLED_UpdatePage(uint8_t page);
uint8_t OLED_Update(void);
uint8_t OLED_GetAddress(void);

#endif
