#ifndef DISPLAY_OUTPUT_H
#define DISPLAY_OUTPUT_H

#include <stdint.h>

/* Call outside compositor_tick, after compositor_init. A mode change pauses
 * composition and stages a cleared unused frame. service keeps USB/storage
 * polling during the bounded clock-switch wait. Zero means the mode applied;
 * a negative result leaves the compositor at the PL's last working geometry. */
int display_output_apply(uint8_t mode, void (*service)(void));

#endif /* DISPLAY_OUTPUT_H */
