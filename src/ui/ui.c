#include "ui/ui.h"
#include "ui/form.h"
#include "ui/txn_list.h"

#include <ncurses.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#define SIDEBAR_WIDTH 18

enum {
    COLOR_HEADER = 1,
    COLOR_SELECTED,
    COLOR_STATUS,
    // 10, 11 reserved for form colors
    COLOR_EXPENSE = 12,
    COLOR_INCOME  = 13
};

static const char *menu_labels[SCREEN_COUNT] = {
    "Dashboard",
    "Transactions",
    "Categories",
    "Budgets",
    "Reports"
};

static struct {
    WINDOW *header;
    WINDOW *sidebar;
    WINDOW *content;
    WINDOW *status;
    sqlite3 *db;
    screen_t current_screen;
    int sidebar_sel;
    bool content_focused;
    bool running;
    txn_list_state_t *txn_list;
} state;

static void ui_create_layout(void) {
    int rows, cols;
    getmaxyx(stdscr, rows, cols);

    int content_h = rows - 2; // minus header and status
    int content_w = cols - SIDEBAR_WIDTH;

    state.header  = newwin(1, cols, 0, 0);
    state.sidebar = newwin(content_h, SIDEBAR_WIDTH, 1, 0);
    state.content = newwin(content_h, content_w, 1, SIDEBAR_WIDTH);
    state.status  = newwin(1, cols, rows - 1, 0);
}

static void ui_destroy_layout(void) {
    delwin(state.header);
    delwin(state.sidebar);
    delwin(state.content);
    delwin(state.status);
}

static void ui_draw_header(void) {
    werase(state.header);
    wbkgd(state.header, COLOR_PAIR(COLOR_HEADER));
    mvwprintw(state.header, 0, 1, "ficli");
    wnoutrefresh(state.header);
}

static void ui_draw_sidebar(void) {
    werase(state.sidebar);
    for (int i = 0; i < SCREEN_COUNT; i++) {
        if (i == state.sidebar_sel) {
            if (state.content_focused)
                wattron(state.sidebar, A_DIM);
            wattron(state.sidebar, COLOR_PAIR(COLOR_SELECTED));
            mvwprintw(state.sidebar, i + 1, 1, " %-*s", SIDEBAR_WIDTH - 3, menu_labels[i]);
            wattroff(state.sidebar, COLOR_PAIR(COLOR_SELECTED));
            if (state.content_focused)
                wattroff(state.sidebar, A_DIM);
        } else {
            mvwprintw(state.sidebar, i + 1, 2, "%-*s", SIDEBAR_WIDTH - 3, menu_labels[i]);
        }
    }
    wnoutrefresh(state.sidebar);
}

static void ui_draw_content(void) {
    werase(state.content);
    box(state.content, 0, 0);

    if (state.current_screen == SCREEN_TRANSACTIONS) {
        if (!state.txn_list)
            state.txn_list = txn_list_create(state.db);
        if (state.txn_list)
            txn_list_draw(state.txn_list, state.content);
    } else {
        int h, w;
        getmaxyx(state.content, h, w);
        const char *title = menu_labels[state.current_screen];
        int len = (int)strlen(title);
        mvwprintw(state.content, h / 2, (w - len) / 2, "%s", title);
    }

    wnoutrefresh(state.content);
}

static void ui_draw_status(void) {
    werase(state.status);
    wbkgd(state.status, COLOR_PAIR(COLOR_STATUS));
    if (state.content_focused && state.current_screen == SCREEN_TRANSACTIONS && state.txn_list) {
        mvwprintw(state.status, 0, 1, "%s", txn_list_status_hint(state.txn_list));
    } else {
        mvwprintw(state.status, 0, 1, "q:Quit  a:Add  \u2191\u2193:Navigate  Enter:Select");
    }
    wnoutrefresh(state.status);
}

static void ui_draw_all(void) {
    ui_draw_header();
    ui_draw_sidebar();
    ui_draw_content();
    ui_draw_status();
    doupdate();
}

static void ui_handle_input(int ch) {
    // When content is focused, delegate to the active content handler first
    if (state.content_focused) {
        // LEFT / Escape always unfocus content
        if (ch == KEY_LEFT || ch == 27) {
            state.content_focused = false;
            return;
        }

        // Delegate to list handler; if it consumes the key, we're done
        if (state.current_screen == SCREEN_TRANSACTIONS && state.txn_list) {
            if (txn_list_handle_input(state.txn_list, ch))
                return;
        }

        // Fall through for keys not consumed (q, a, KEY_RESIZE)
    }

    switch (ch) {
    case 'q':
        state.running = false;
        break;
    case KEY_UP:
        if (!state.content_focused && state.sidebar_sel > 0)
            state.sidebar_sel--;
        break;
    case KEY_DOWN:
        if (!state.content_focused && state.sidebar_sel < SCREEN_COUNT - 1)
            state.sidebar_sel++;
        break;
    case '\n':
    case KEY_RIGHT:
        state.current_screen = state.sidebar_sel;
        state.content_focused = true;
        break;
    case 'a':
        form_add_transaction(state.db, state.content);
        if (state.txn_list)
            txn_list_mark_dirty(state.txn_list);
        touchwin(state.header);
        touchwin(state.sidebar);
        touchwin(state.content);
        touchwin(state.status);
        break;
    case KEY_RESIZE:
        ui_destroy_layout();
        ui_create_layout();
        break;
    }
}

void ui_init(void) {
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);

    // Disable XON/XOFF flow control so Ctrl+S reaches the application
    struct termios term;
    tcgetattr(STDIN_FILENO, &term);
    term.c_iflag &= ~(IXON | IXOFF);
    tcsetattr(STDIN_FILENO, TCSANOW, &term);

    start_color();
    use_default_colors();
    init_pair(COLOR_HEADER,   COLOR_BLACK, COLOR_CYAN);
    init_pair(COLOR_SELECTED,  COLOR_BLACK, COLOR_WHITE);
    init_pair(COLOR_STATUS,   COLOR_BLACK, COLOR_CYAN);
    init_pair(10, COLOR_WHITE, COLOR_BLUE);   // COLOR_FORM
    init_pair(11, COLOR_BLACK, COLOR_CYAN);   // COLOR_FORM_ACTIVE
    init_pair(COLOR_EXPENSE, COLOR_RED,   -1);
    init_pair(COLOR_INCOME,  COLOR_GREEN, -1);
}

void ui_cleanup(void) {
    endwin();
}

void ui_run(sqlite3 *db) {
    state.db = db;

    state.current_screen = SCREEN_DASHBOARD;
    state.sidebar_sel = 0;
    state.content_focused = false;
    state.running = true;
    state.txn_list = NULL;

    ui_create_layout();

    while (state.running) {
        ui_draw_all();
        int ch = getch();
        ui_handle_input(ch);
    }

    txn_list_destroy(state.txn_list);
    state.txn_list = NULL;
    ui_destroy_layout();
}
