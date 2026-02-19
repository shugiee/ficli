#include "ui/ui.h"
#include "ui/account_list.h"
#include "ui/colors.h"
#include "ui/form.h"
#include "ui/import_dialog.h"
#include "ui/txn_list.h"

#include <errno.h>
#include <locale.h>
#include <ncurses.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#define SIDEBAR_WIDTH 18


static const char *menu_labels[SCREEN_COUNT] = {"Dashboard",  "Transactions",
                                                "Categories", "Budgets",
                                                "Reports",    "Accounts"};

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
    account_list_state_t *account_list;
    bool dark_mode;
} state;

typedef struct {
    const char *key; /* NULL = section header, "" = blank separator */
    const char *desc;
} help_row_t;

static const help_row_t help_rows[] = {
    {NULL, "Global"},
    {"q", "Quit"},
    {"a", "Add transaction"},
    {"i", "Import CSV"},
    {"t", "Toggle theme"},
    {"?", "This help"},

    {"", ""},
    {NULL, "Navigation (sidebar)"},
    {"j / \u2193", "Move down"},
    {"k / \u2191", "Move up"},
    {"l / \u2192 / Enter", "Select / enter content"},
    {"h / \u2190 / Esc", "Back to sidebar"},

    {"", ""},
    {NULL, "Transactions list"},
    {"Space", "Toggle selection"},
    {"Shift+\u2191/\u2193", "Extend selection"},
    {"Esc", "Clear selection"},
    {"e", "Edit selected"},
    {"c", "Edit category only"},
    {"d", "Delete selected"},
    {"/", "Filter"},
    {"s", "Cycle sort column"},
    {"S", "Toggle sort direction"},
    {"g / Home", "Jump to first"},
    {"G / End", "Jump to last"},
    {"1-9", "Switch account tab"},

    {"", ""},
    {NULL, "Filter mode (transactions)"},
    {"type", "Add to filter"},
    {"Backspace", "Remove character"},
    {"Enter", "Confirm filter"},
    {"Esc", "Clear and close filter"},

    {"", ""},
    {NULL, "Transaction form"},
    {"Tab / \u2193", "Next field"},
    {"Shift+Tab / \u2191", "Previous field"},
    {"Ctrl+S", "Save"},
    {"Esc", "Cancel"},

    {"", ""},
    {NULL, "Accounts"},
    {"Enter", "Add account"},
    {"e", "Edit selected account"},
    {"d", "Delete selected account"},
    {"\u2190 / \u2192", "Change type"},
};

#define HELP_ROW_COUNT ((int)(sizeof(help_rows) / sizeof(help_rows[0])))

#define HELP_WIN_W 52
#define HELP_KEY_W 18
/* desc width: 52 - 2(border) - 18(key) - 2(left pad + gap) - 1(right pad) = 29
 */
#define HELP_DESC_W (HELP_WIN_W - 2 - HELP_KEY_W - 2 - 1)

static bool ui_get_config_dir(char *out, size_t out_sz) {
    const char *base = getenv("XDG_CONFIG_HOME");
    if (!base || base[0] == '\0') {
        const char *home = getenv("HOME");
        if (!home || home[0] == '\0')
            return false;
        snprintf(out, out_sz, "%s/.config/ficli", home);
        return true;
    }
    snprintf(out, out_sz, "%s/ficli", base);
    return true;
}

static bool ui_get_config_path(char *out, size_t out_sz) {
    char dir[512];
    if (!ui_get_config_dir(dir, sizeof(dir)))
        return false;
    snprintf(out, out_sz, "%s/config.ini", dir);
    return true;
}

static bool ui_ensure_config_dir(void) {
    char dir[512];
    if (!ui_get_config_dir(dir, sizeof(dir)))
        return false;

    const char *base = getenv("XDG_CONFIG_HOME");
    if (!base || base[0] == '\0') {
        const char *home = getenv("HOME");
        if (!home || home[0] == '\0')
            return false;
        char parent[512];
        snprintf(parent, sizeof(parent), "%s/.config", home);
        if (mkdir(parent, 0755) != 0 && errno != EEXIST)
            return false;
    } else {
        if (mkdir(base, 0755) != 0 && errno != EEXIST)
            return false;
    }

    if (mkdir(dir, 0755) != 0 && errno != EEXIST)
        return false;
    return true;
}

static bool ui_load_theme_pref(bool *dark_mode) {
    char path[512];
    if (!ui_get_config_path(path, sizeof(path)))
        return false;

    FILE *fp = fopen(path, "r");
    if (!fp)
        return false;

    char line[128];
    bool found = false;
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "theme=", 6) == 0) {
            const char *val = line + 6;
            if (strncmp(val, "dark", 4) == 0) {
                *dark_mode = true;
                found = true;
                break;
            }
            if (strncmp(val, "light", 5) == 0) {
                *dark_mode = false;
                found = true;
                break;
            }
        } else if (strncmp(line, "dark_mode=", 10) == 0) {
            const char *val = line + 10;
            *dark_mode = (val[0] == '1');
            found = true;
            break;
        }
    }

    fclose(fp);
    return found;
}

static void ui_save_theme_pref(bool dark_mode) {
    char path[512];
    if (!ui_get_config_path(path, sizeof(path)))
        return;
    if (!ui_ensure_config_dir())
        return;

    FILE *fp = fopen(path, "w");
    if (!fp)
        return;

    fprintf(fp, "theme=%s\n", dark_mode ? "dark" : "light");
    fclose(fp);
}

static void ui_apply_theme(bool dark_mode) {
    // Scale an 8-bit hex component to ncurses 0-1000 range
#define HEX_NC(v) ((v) * 1000 / 255)
    if (dark_mode) {
        // Everforest Dark (Medium)
        init_color(CUST_BG,     HEX_NC(0x2d), HEX_NC(0x35), HEX_NC(0x3b));
        init_color(CUST_RED,    HEX_NC(0xe6), HEX_NC(0x7e), HEX_NC(0x80));
        init_color(CUST_GREEN,  HEX_NC(0xa7), HEX_NC(0xc0), HEX_NC(0x80));
        init_color(CUST_YELLOW, HEX_NC(0xdb), HEX_NC(0xbc), HEX_NC(0x7f));
        init_color(CUST_BLUE,   HEX_NC(0x7f), HEX_NC(0xbb), HEX_NC(0xb3));
        init_color(CUST_PURPLE, HEX_NC(0xd6), HEX_NC(0x99), HEX_NC(0xb6));
        init_color(CUST_AQUA,   HEX_NC(0x83), HEX_NC(0xc0), HEX_NC(0x92));
        init_color(CUST_FG,     HEX_NC(0xd3), HEX_NC(0xc6), HEX_NC(0xaa));
    } else {
        // Everforest Light (Medium)
        init_color(CUST_BG,     HEX_NC(0xfd), HEX_NC(0xf6), HEX_NC(0xe3));
        init_color(CUST_RED,    HEX_NC(0xe6), HEX_NC(0x7e), HEX_NC(0x80));
        init_color(CUST_GREEN,  HEX_NC(0xa7), HEX_NC(0xc0), HEX_NC(0x80));
        init_color(CUST_YELLOW, HEX_NC(0xdb), HEX_NC(0xbc), HEX_NC(0x7f));
        init_color(CUST_BLUE,   HEX_NC(0x7f), HEX_NC(0xbb), HEX_NC(0xb3));
        init_color(CUST_PURPLE, HEX_NC(0xd6), HEX_NC(0x99), HEX_NC(0xb6));
        init_color(CUST_AQUA,   HEX_NC(0x83), HEX_NC(0xc0), HEX_NC(0x92));
        init_color(CUST_FG,     HEX_NC(0x5c), HEX_NC(0x6a), HEX_NC(0x72));
    }
#undef HEX_NC

    init_pair(COLOR_NORMAL,      CUST_FG,    CUST_BG);
    init_pair(COLOR_HEADER,      CUST_BG,    CUST_BLUE);
    init_pair(COLOR_SELECTED,    CUST_BG,    CUST_FG);
    init_pair(COLOR_STATUS,      CUST_BG,    CUST_BLUE);
    init_pair(COLOR_FORM,        CUST_BG,    CUST_FG);
    init_pair(COLOR_FORM_ACTIVE, CUST_BG,    CUST_AQUA);
    init_pair(COLOR_EXPENSE,     CUST_RED,   CUST_BG);
    init_pair(COLOR_INCOME,      CUST_GREEN, CUST_BG);
    init_pair(COLOR_INFO,        CUST_AQUA,  CUST_BG);
}

static void ui_create_layout(void) {
    int rows, cols;
    getmaxyx(stdscr, rows, cols);

    int content_h = rows - 2; // minus header and status
    int content_w = cols - SIDEBAR_WIDTH;

    state.header = newwin(1, cols, 0, 0);
    state.sidebar = newwin(content_h, SIDEBAR_WIDTH, 1, 0);
    state.content = newwin(content_h, content_w, 1, SIDEBAR_WIDTH);
    state.status = newwin(1, cols, rows - 1, 0);

    wbkgd(stdscr, COLOR_PAIR(COLOR_NORMAL));
    wbkgd(state.sidebar, COLOR_PAIR(COLOR_NORMAL));
    wbkgd(state.content, COLOR_PAIR(COLOR_NORMAL));
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
            if (state.content_focused) {
                wattron(state.sidebar, A_DIM | A_REVERSE);
                mvwprintw(state.sidebar, i + 1, 1, " %-*s", SIDEBAR_WIDTH - 3,
                          menu_labels[i]);
                wattroff(state.sidebar, A_DIM | A_REVERSE);
            } else {
                wattron(state.sidebar, COLOR_PAIR(COLOR_SELECTED));
                mvwprintw(state.sidebar, i + 1, 1, " %-*s", SIDEBAR_WIDTH - 3,
                          menu_labels[i]);
                wattroff(state.sidebar, COLOR_PAIR(COLOR_SELECTED));
            }
        } else {
            mvwprintw(state.sidebar, i + 1, 2, "%-*s", SIDEBAR_WIDTH - 3,
                      menu_labels[i]);
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
            txn_list_draw(state.txn_list, state.content, state.content_focused);
    } else if (state.current_screen == SCREEN_ACCOUNTS) {
        if (!state.account_list)
            state.account_list = account_list_create(state.db);
        if (state.account_list)
            account_list_draw(state.account_list, state.content,
                              state.content_focused);
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
    if (state.content_focused && state.current_screen == SCREEN_TRANSACTIONS &&
        state.txn_list) {
        mvwprintw(state.status, 0, 1, "%s",
                  txn_list_status_hint(state.txn_list));
    } else if (state.content_focused &&
               state.current_screen == SCREEN_ACCOUNTS && state.account_list) {
        mvwprintw(state.status, 0, 1, "%s",
                  account_list_status_hint(state.account_list));
    } else {
        mvwprintw(state.status, 0, 1,
                  "q:Quit  a:Add  i:Import  t:Theme  ?:Help  "
                  "\u2191\u2193:Navigate  Enter:Select");
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

static void ui_show_help(void) {
    int scr_rows, scr_cols;
    getmaxyx(stdscr, scr_rows, scr_cols);

    int win_h = HELP_ROW_COUNT + 2; /* +2 for top/bottom border */
    int max_h = scr_rows - 2;
    if (max_h < 6)
        max_h = 6;
    if (win_h > max_h)
        win_h = max_h;
    int win_w = HELP_WIN_W;
    if (win_w > scr_cols)
        win_w = scr_cols;

    WINDOW *w =
        newwin(win_h, win_w, (scr_rows - win_h) / 2, (scr_cols - win_w) / 2);
    keypad(w, TRUE);

    /* Interior rows 1..win_h-2; footer printed in bottom border row */
    int visible = win_h - 2;
    if (visible < 1)
        visible = 1;
    int max_scroll = HELP_ROW_COUNT - visible;
    if (max_scroll < 0)
        max_scroll = 0;

    int scroll = 0;
    bool done = false;

    while (!done) {
        werase(w);
        wbkgd(w, COLOR_PAIR(COLOR_FORM));
        box(w, 0, 0);

        /* Title in top border */
        const char *title = " Keyboard Shortcuts ";
        mvwprintw(w, 0, (win_w - (int)strlen(title)) / 2, "%s", title);

        /* Footer in bottom border (display-col counts used for centering) */
        bool scrollable = (max_scroll > 0);
        const char *footer;
        int footer_cols;
        if (scrollable) {
            footer = " j/\u2193 k/\u2191:Scroll  Any other key:Close ";
            footer_cols = 38; /* display columns, not byte count */
        } else {
            footer = " Any key to close ";
            footer_cols = 18;
        }
        int fx = (win_w - footer_cols) / 2;
        if (fx < 1)
            fx = 1;
        mvwprintw(w, win_h - 1, fx, "%s", footer);

        /* Scroll indicators at col win_w-2 (right pad column, inside border) */
        if (scroll > 0)
            mvwprintw(w, 1, win_w - 2, "\u25b2");
        if (scroll < max_scroll)
            mvwprintw(w, win_h - 2, win_w - 2, "\u25bc");

        /* Content rows */
        for (int i = 0; i < visible; i++) {
            int idx = scroll + i;
            if (idx >= HELP_ROW_COUNT)
                break;
            const help_row_t *r = &help_rows[idx];
            int row = 1 + i;

            if (r->key == NULL) {
                wattron(w, A_BOLD);
                mvwprintw(w, row, 2, "%-*.*s", win_w - 4, win_w - 4, r->desc);
                wattroff(w, A_BOLD);
            } else if (r->key[0] != '\0') {
                mvwprintw(w, row, 2, "%-*.*s", HELP_KEY_W, HELP_KEY_W, r->key);
                mvwprintw(w, row, 2 + HELP_KEY_W + 1, "%-*.*s", HELP_DESC_W,
                          HELP_DESC_W, r->desc);
            }
            /* empty key string = blank row, nothing to draw */
        }

        wrefresh(w);

        int ch = wgetch(w);
        if (scrollable && (ch == KEY_DOWN || ch == 'j')) {
            if (scroll < max_scroll)
                scroll++;
        } else if (scrollable && (ch == KEY_UP || ch == 'k')) {
            if (scroll > 0)
                scroll--;
        } else {
            done = true;
        }
    }

    delwin(w);
    touchwin(state.header);
    touchwin(state.sidebar);
    touchwin(state.content);
    touchwin(state.status);
}

static void ui_handle_input(int ch) {
    // When content is focused, delegate to the active content handler first
    if (state.content_focused) {
        // Delegate to list handler; if it consumes the key, we're done
        if (state.current_screen == SCREEN_TRANSACTIONS && state.txn_list) {
            if (txn_list_handle_input(state.txn_list, state.content, ch))
                return;
        }
        if (state.current_screen == SCREEN_ACCOUNTS && state.account_list) {
            if (account_list_handle_input(state.account_list, state.content, ch)) {
                if (account_list_consume_changed(state.account_list) &&
                    state.txn_list) {
                    txn_list_mark_dirty(state.txn_list);
                }
                return;
            }
        }

        // LEFT / h / Escape unfocus content if not consumed above
        if (ch == KEY_LEFT || ch == 'h' || ch == 27) {
            state.content_focused = false;
            return;
        }

        // Fall through for keys not consumed (q, a, KEY_RESIZE)
    }

    switch (ch) {
    case 'q':
        state.running = false;
        break;
    case KEY_UP:
    case 'k':
        if (!state.content_focused && state.sidebar_sel > 0)
            state.sidebar_sel--;
        break;
    case KEY_DOWN:
    case 'j':
        if (!state.content_focused && state.sidebar_sel < SCREEN_COUNT - 1)
            state.sidebar_sel++;
        break;
    case '\n':
    case KEY_RIGHT:
    case 'l':
        state.current_screen = state.sidebar_sel;
        if (state.current_screen == SCREEN_TRANSACTIONS ||
            state.current_screen == SCREEN_ACCOUNTS)
            state.content_focused = true;
        break;
    case 'a': {
        transaction_t txn = {0};
        form_result_t res =
            form_transaction(state.content, state.db, &txn, false);
        if (res == FORM_SAVED && state.txn_list)
            txn_list_mark_dirty(state.txn_list);
    }
        touchwin(state.header);
        touchwin(state.sidebar);
        touchwin(state.content);
        touchwin(state.status);
        break;
    case 'i': {
        int64_t acct_id = state.txn_list
            ? txn_list_get_current_account_id(state.txn_list) : 0;
        int n = import_dialog(state.content, state.db, acct_id);
        if (n > 0 && state.txn_list)
            txn_list_mark_dirty(state.txn_list);
        touchwin(state.header);
        touchwin(state.sidebar);
        touchwin(state.content);
        touchwin(state.status);
    } break;
    case 't':
        state.dark_mode = !state.dark_mode;
        ui_apply_theme(state.dark_mode);
        ui_save_theme_pref(state.dark_mode);

        wbkgd(stdscr, COLOR_PAIR(COLOR_NORMAL));
        wbkgd(state.sidebar, COLOR_PAIR(COLOR_NORMAL));
        wbkgd(state.content, COLOR_PAIR(COLOR_NORMAL));
        touchwin(state.header);
        touchwin(state.sidebar);
        touchwin(state.content);
        touchwin(state.status);
        break;
    case '?':
        ui_show_help();
        break;
    case KEY_RESIZE:
        ui_destroy_layout();
        ui_create_layout();
        break;
    }
}

void ui_init(void) {
    setlocale(LC_ALL, "");
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
    state.dark_mode = false;
    ui_load_theme_pref(&state.dark_mode);
    ui_apply_theme(state.dark_mode);
}

void ui_cleanup(void) { endwin(); }

void ui_run(sqlite3 *db) {
    state.db = db;

    state.current_screen = SCREEN_DASHBOARD;
    state.sidebar_sel = 0;
    state.content_focused = false;
    state.running = true;
    state.txn_list = NULL;
    state.account_list = NULL;

    ui_create_layout();
    refresh(); // sync stdscr so getch() won't blank the screen

    while (state.running) {
        ui_draw_all();
        int ch = getch();
        ui_handle_input(ch);
    }

    txn_list_destroy(state.txn_list);
    state.txn_list = NULL;
    account_list_destroy(state.account_list);
    state.account_list = NULL;
    ui_destroy_layout();
}
