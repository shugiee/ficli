#ifndef FICLI_UI_RESIZE_H
#define FICLI_UI_RESIZE_H

#include <stdbool.h>
#include <ncurses.h>

// Re-queue resize for the top-level loop so layout can be rebuilt centrally.
static inline bool ui_requeue_resize_event(int ch) {
    if (ch != KEY_RESIZE)
        return false;
    (void)ungetch(KEY_RESIZE);
    return true;
}

#endif
