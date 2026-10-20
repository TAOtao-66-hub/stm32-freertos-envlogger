# 硬件连接

| STM32 引脚 | 外设 |
|---|---|
| PA0 / ADC1_IN0 | 电位器滑动端，固定端接 3.3V 与 GND |
| PA2 | 有源蜂鸣器控制，低有效 |
| PA3 | 消音按键，上拉输入、低有效 |
| PA4 | 报警 LED，高有效 |
| PA5 / PA6 / PA7 | SPI1 SCK / MISO / MOSI，连接 W25Q32 |
| PB0 | W25Q32 CS，低有效 |
| PA9 / PA10 | USART1 TX / RX，接 USB-TTL RX / TX |
| PB6 / PB7 | OLED SCL / SDA，软件 I²C，外部上拉至 3.3V |
| PB10 | DHT11 数据，按器件要求上拉 |
| PA13 / PA14 | SWDIO / SWCLK，保留调试 |

所有模块共地。STM32、W25Q32 和 USB-TTL 信号按 3.3V 逻辑连接，蜂鸣器供电按具体模块规格。STM32 信号脚不直接承受 5V 模块输出。

当前 Flash 的 JEDEC ID 为 EF4016，对应 4MiB 容量。前两个 4KiB 扇区保存参数，其余用于历史记录。

系统使用内部 RTC 和 LSI 时钟；每次启动主动进入未校时状态，没有外接带备用电源的 RTC。
