#ifndef HAL_H
#define HAL_H

#include <stdint.h>

/*
 * Hardware Abstraction Layer — generic interface for input polling.
 * High-level UI code calls these; the underlying PS/2 or USB drivers
 * fill in the results via the backend structs.
 */

typedef struct {
    int32_t  x;
    int32_t  y;
    uint8_t  buttons;  /* bit 0 = left, bit 1 = right, bit 2 = middle */
} hal_mouse_state_t;

typedef struct {
    char     ch;       /* 0 if no key available */
} hal_key_event_t;

/* Initialize the HAL (probes hardware, installs IRQ handlers).
 * Returns 0 on success, nonzero on fatal failure. */
int  hal_init(uint16_t code_selector, int32_t screen_w, int32_t screen_h);

/* Poll mouse — fills state with current cursor position + buttons */
void hal_poll_mouse(hal_mouse_state_t *state);

/* Poll keyboard — returns next buffered character or 0 */
char hal_poll_keyboard(void);

/* HAL status flags (set during init) */
extern int hal_ps2_mouse_present;
extern int hal_ps2_keyboard_present;

#endif /* HAL_H */
