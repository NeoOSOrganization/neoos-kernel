#ifndef NEOOS_BANNER_H
#define NEOOS_BANNER_H

// Clear the screen and draw the boot banner: the butterfly-N logo
// (purple N strokes, red wings) and an info panel (version + git rev,
// CPU brand, cores, memory, features). Call once, after
// con_driver_select().
void banner_show(void);

#endif
