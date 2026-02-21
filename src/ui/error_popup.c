#include "ui/error_popup.h"

#include "ui/colors.h"
#include "ui/resize.h"

#include <string.h>

#define ERROR_POPUP_MIN_W 34
#define ERROR_POPUP_MAX_W 74
#define ERROR_POPUP_H 7

void ui_show_error_popup(WINDOW *parent, const char *title,
                         const char *message) {
    if (!parent)
        return;

    if (!title || title[0] == '\0')
        title = " Error ";
    if (!message || message[0] == '\0')
        message = "Unknown error.";

    int ph, pw;
    getmaxyx(parent, ph, pw);
    if (ph < 5 || pw < 20)
        return;

    int win_h = ERROR_POPUP_H;
    if (win_h > ph)
        win_h = ph;
    if (win_h < 5)
        return;

    int message_w = (int)strlen(message) + 4;
    int win_w = message_w;
    if (win_w < ERROR_POPUP_MIN_W)
        win_w = ERROR_POPUP_MIN_W;
    if (win_w > ERROR_POPUP_MAX_W)
        win_w = ERROR_POPUP_MAX_W;
    if (win_w > pw)
        win_w = pw;

    int py, px;
    getbegyx(parent, py, px);
    int win_x = px + (pw - win_w) / 2;

    // Keep the popup near the top while still fully inside the parent.
    int win_y = py + 2;
    int max_y = py + ph - win_h;
    if (win_y > max_y)
        win_y = max_y;
    if (win_y < py)
        win_y = py;

    WINDOW *w = newwin(win_h, win_w, win_y, win_x);
    if (!w)
        return;
    keypad(w, TRUE);

    wbkgd(w, COLOR_PAIR(COLOR_FORM));
    werase(w);

    wattron(w, COLOR_PAIR(COLOR_ERROR));
    box(w, 0, 0);
    wattroff(w, COLOR_PAIR(COLOR_ERROR));

    int title_x = (win_w - (int)strlen(title)) / 2;
    if (title_x < 1)
        title_x = 1;
    wattron(w, COLOR_PAIR(COLOR_ERROR) | A_BOLD);
    mvwprintw(w, 0, title_x, "%s", title);
    wattroff(w, COLOR_PAIR(COLOR_ERROR) | A_BOLD);

    int msg_w = win_w - 4;
    if (msg_w < 1)
        msg_w = 1;
    mvwprintw(w, 2, 2, "%-*.*s", msg_w, msg_w, message);

    const char *footer = "Press any key";
    int footer_x = (win_w - (int)strlen(footer)) / 2;
    if (footer_x < 1)
        footer_x = 1;
    wattron(w, A_DIM);
    mvwprintw(w, win_h - 2, footer_x, "%s", footer);
    wattroff(w, A_DIM);

    wrefresh(w);
    int ch = wgetch(w);
    bool requeued_resize = ui_requeue_resize_event(ch);
    if (!requeued_resize)
        flushinp();

    delwin(w);
    touchwin(parent);
    redrawwin(parent);
    wrefresh(parent);
}
