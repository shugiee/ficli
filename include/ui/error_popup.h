#ifndef FICLI_ERROR_POPUP_H
#define FICLI_ERROR_POPUP_H

#include <ncurses.h>

// Show a generic error popup centered horizontally near the top of parent.
// The popup blocks until the user presses any key.
void ui_show_error_popup(WINDOW *parent, const char *title,
                         const char *message);

#endif
