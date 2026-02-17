#include "ui/ui.h"

#include <ncurses.h>
#include <string.h>

void ui_init(void) {
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
}

void ui_cleanup(void) {
    endwin();
}

void ui_draw_welcome(void) {
    int rows, cols;
    getmaxyx(stdscr, rows, cols);

    const char *msg = "ficli - Press any key to exit";
    int len = (int)strlen(msg);
    mvprintw(rows / 2, (cols - len) / 2, "%s", msg);
    refresh();
}
