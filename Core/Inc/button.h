#ifndef BUTTON_H
#define BUTTON_H

/* Debounce timing is configured in project_config.h. */
typedef enum
{
    BUTTON_EVENT_NONE = 0,
    BUTTON_EVENT_PRESSED,
    BUTTON_EVENT_RELEASED
} Button_Event;

/* PA3 pull-up input: button connects PA3 to GND when pressed. */
/* Call Init once, then Poll about every 10 ms from one task. No delay inside. */
/* Holding the button at startup produces no press event until released. */
void Button_Init(void);
Button_Event Button_Poll(void);

#endif /* BUTTON_H */
