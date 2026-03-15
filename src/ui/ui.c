#include "ui/ui.h"
#include "db/query.h"
#include "ui/account_list.h"
#include "ui/budget_list.h"
#include "ui/category_list.h"
#include "ui/colors.h"
#include "ui/error_popup.h"
#include "ui/form.h"
#include "ui/import_dialog.h"
#include "ui/report_list.h"
#include "ui/resize.h"
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
#define RESIZE_DEBOUNCE_MS 60
#define MIN_TERM_COLS 80
#define MIN_TERM_ROWS 24
#define AUTO_LINK_DATE_WINDOW_DAYS 3

typedef struct {
    const char *label;
    bool content_focusable;
} screen_info_t;

static const screen_info_t screen_info[SCREEN_COUNT] = {
#define SCREEN_DEF(id, label, content_focusable)                                  \
    [id] = {label, content_focusable},
#include "ui/screens.def"
#undef SCREEN_DEF
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
    account_list_state_t *account_list;
    category_list_state_t *category_list;
    budget_list_state_t *budget_list;
    report_list_state_t *report_list;
    bool dark_mode;
    bool layout_ready;
    int layout_rows;
    int layout_cols;
} state;

typedef struct {
    const char *key; /* NULL = section header, "" = blank separator */
    const char *desc;
} help_row_t;

static const help_row_t help_rows[] = {
    {NULL, "Global"},
    {"q", "Quit"},
    {"a", "Add transaction"},
    {"i", "Import file"},
    {"L", "Auto-link transfers"},
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
    {"e", "Edit selected (or choose edited-only vs all filtered)"},
    {"c", "Edit category only"},
    {"D", "Duplicate selected/current (confirm modal)"},
    {"d", "Delete selected"},
    {"/", "Filter"},
    {"s", "Cycle sort column"},
    {"S", "Toggle sort direction"},
    {"g / Home", "Jump to first"},
    {"G / End", "Jump to last"},
    {"Ctrl+D / Ctrl+U", "Half-page down/up"},
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
    {NULL, "Categories"},
    {"Enter", "Show add category form"},
    {"e", "Edit selected category"},
    {"d", "Delete selected category (can reassign transactions)"},
    {"\u2190 / \u2192", "Change type"},

    {"", ""},
    {NULL, "Accounts"},
    {"Enter", "Show add account form"},
    {"e", "Edit selected account"},
    {"d", "Delete selected account"},
    {"\u2190 / \u2192", "Change type"},

    {"", ""},
    {NULL, "Budgets"},
    {"h / \u2190", "Previous month"},
    {"l / \u2192", "Next month"},
    {"r", "Jump to current month"},
    {"Enter", "Show matches / leave matches focus"},
    {"e", "Edit budget or focused transaction"},
    {"Enter (budget edit)", "Choose save scope (onward/current month)"},
    {"f", "Open category filter panel"},
    {"Space / Enter (filter)", "Toggle selected category"},
    {"m (filter)", "Toggle include/exclude mode"},
    {"Esc", "Cancel inline edit / close filter"},

    {"", ""},
    {NULL, "Reports"},
    {"[ or ]", "Switch category/payee"},
    {"/", "Toggle category/payee"},
    {"Enter", "Show matching transactions"},
    {"e", "Edit selected matching transaction"},
    {"p", "Cycle period"},
    {"s", "Cycle sort column"},
    {"S", "Toggle sort direction"},
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
        init_color(CUST_BG, HEX_NC(0x2d), HEX_NC(0x35), HEX_NC(0x3b));
        init_color(CUST_RED, HEX_NC(0xe6), HEX_NC(0x7e), HEX_NC(0x80));
        init_color(CUST_GREEN, HEX_NC(0xa7), HEX_NC(0xc0), HEX_NC(0x80));
        init_color(CUST_YELLOW, HEX_NC(0xdb), HEX_NC(0xbc), HEX_NC(0x7f));
        init_color(CUST_BLUE, HEX_NC(0x7f), HEX_NC(0xbb), HEX_NC(0xb3));
        init_color(CUST_PURPLE, HEX_NC(0xd6), HEX_NC(0x99), HEX_NC(0xb6));
        init_color(CUST_AQUA, HEX_NC(0x83), HEX_NC(0xc0), HEX_NC(0x92));
        init_color(CUST_FG, HEX_NC(0xd3), HEX_NC(0xc6), HEX_NC(0xaa));
        // Slightly lighter than dark background for layered dropdowns
        init_color(CUST_SURFACE, HEX_NC(0x37), HEX_NC(0x40), HEX_NC(0x46));
    } else {
        // Everforest Light (Medium)
        init_color(CUST_BG, HEX_NC(0xfd), HEX_NC(0xf6), HEX_NC(0xe3));
        init_color(CUST_RED, HEX_NC(0xe6), HEX_NC(0x7e), HEX_NC(0x80));
        init_color(CUST_GREEN, HEX_NC(0xa7), HEX_NC(0xc0), HEX_NC(0x80));
        init_color(CUST_YELLOW, HEX_NC(0xdb), HEX_NC(0xbc), HEX_NC(0x7f));
        init_color(CUST_BLUE, HEX_NC(0x7f), HEX_NC(0xbb), HEX_NC(0xb3));
        init_color(CUST_PURPLE, HEX_NC(0xd6), HEX_NC(0x99), HEX_NC(0xb6));
        init_color(CUST_AQUA, HEX_NC(0x83), HEX_NC(0xc0), HEX_NC(0x92));
        init_color(CUST_FG, HEX_NC(0x5c), HEX_NC(0x6a), HEX_NC(0x72));
        // Slightly darker than light background for layered dropdowns
        init_color(CUST_SURFACE, HEX_NC(0xf3), HEX_NC(0xec), HEX_NC(0xd9));
    }
#undef HEX_NC

    init_pair(COLOR_NORMAL, CUST_FG, CUST_BG);
    init_pair(COLOR_HEADER, CUST_BG, CUST_BLUE);
    init_pair(COLOR_SELECTED, CUST_BG, CUST_FG);
    init_pair(COLOR_STATUS, CUST_BG, CUST_BLUE);
    init_pair(COLOR_FORM, CUST_BG, CUST_FG);
    init_pair(COLOR_FORM_ACTIVE, CUST_BG, CUST_AQUA);
    init_pair(COLOR_EXPENSE, CUST_RED, CUST_BG);
    init_pair(COLOR_INCOME, CUST_GREEN, CUST_BG);
    init_pair(COLOR_INFO, CUST_AQUA, CUST_BG);
    init_pair(COLOR_FORM_DROPDOWN, CUST_FG, CUST_SURFACE);
    init_pair(COLOR_ERROR, CUST_RED, CUST_BG);
    init_pair(COLOR_WARNING, CUST_YELLOW, CUST_BG);
    init_pair(COLOR_HILITE_GOOD, CUST_BG, CUST_GREEN);
    init_pair(COLOR_HILITE_WARN, CUST_BG, CUST_YELLOW);
    init_pair(COLOR_HILITE_BAD, CUST_BG, CUST_RED);
}

static bool ui_size_is_supported(int rows, int cols) {
    return rows >= MIN_TERM_ROWS && cols >= MIN_TERM_COLS;
}

static bool ui_create_layout(void) {
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    if (!ui_size_is_supported(rows, cols))
        return false;

    int content_h = rows - 2; // minus header and status
    int content_w = cols - SIDEBAR_WIDTH;

    state.header = newwin(1, cols, 0, 0);
    state.sidebar = newwin(content_h, SIDEBAR_WIDTH, 1, 0);
    state.content = newwin(content_h, content_w, 1, SIDEBAR_WIDTH);
    state.status = newwin(1, cols, rows - 1, 0);
    if (!state.header || !state.sidebar || !state.content || !state.status) {
        if (state.header) {
            delwin(state.header);
            state.header = NULL;
        }
        if (state.sidebar) {
            delwin(state.sidebar);
            state.sidebar = NULL;
        }
        if (state.content) {
            delwin(state.content);
            state.content = NULL;
        }
        if (state.status) {
            delwin(state.status);
            state.status = NULL;
        }
        return false;
    }

    wbkgd(stdscr, COLOR_PAIR(COLOR_NORMAL));
    wbkgd(state.header, COLOR_PAIR(COLOR_HEADER));
    wbkgd(state.sidebar, COLOR_PAIR(COLOR_NORMAL));
    wbkgd(state.content, COLOR_PAIR(COLOR_NORMAL));
    wbkgd(state.status, COLOR_PAIR(COLOR_STATUS));
    state.layout_ready = true;
    state.layout_rows = rows;
    state.layout_cols = cols;
    return true;
}

static void ui_destroy_layout(void) {
    if (state.header) {
        delwin(state.header);
        state.header = NULL;
    }
    if (state.sidebar) {
        delwin(state.sidebar);
        state.sidebar = NULL;
    }
    if (state.content) {
        delwin(state.content);
        state.content = NULL;
    }
    if (state.status) {
        delwin(state.status);
        state.status = NULL;
    }
    state.layout_ready = false;
    state.layout_rows = 0;
    state.layout_cols = 0;
}

static void ui_sync_layout_to_terminal(void) {
    int rows, cols;
    getmaxyx(stdscr, rows, cols);

    if (!ui_size_is_supported(rows, cols)) {
        if (state.layout_ready) {
            ui_destroy_layout();
            clearok(stdscr, TRUE);
        }
        return;
    }

    bool needs_rebuild =
        !state.layout_ready || rows != state.layout_rows || cols != state.layout_cols;
    if (!needs_rebuild)
        return;

    if (state.layout_ready)
        ui_destroy_layout();
    if (ui_create_layout())
        clearok(stdscr, TRUE);
}

static void ui_handle_resize_event(void) {
#ifdef NCURSES_VERSION
    (void)resize_term(0, 0);
#endif

    int next = ERR;
    timeout(RESIZE_DEBOUNCE_MS);
    while (1) {
        next = getch();
        if (next != KEY_RESIZE)
            break;
#ifdef NCURSES_VERSION
        (void)resize_term(0, 0);
#endif
    }
    timeout(-1);

    ui_sync_layout_to_terminal();

    if (next != ERR)
        (void)ungetch(next);
}

static void ui_draw_centered_line(int row, const char *text) {
    if (!text)
        return;

    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    if (row < 0 || row >= rows || cols <= 0)
        return;

    int len = (int)strlen(text);
    int col = (cols - len) / 2;
    if (col < 0)
        col = 0;
    mvaddnstr(row, col, text, cols - col);
}

static void ui_draw_too_small_screen(void) {
    int rows, cols;
    getmaxyx(stdscr, rows, cols);

    char current[64];
    char required[64];
    snprintf(current, sizeof(current), "Current: %dx%d", cols, rows);
    snprintf(required, sizeof(required), "Required: %dx%d", MIN_TERM_COLS,
             MIN_TERM_ROWS);

    const char *title = "Window too small";
    const char *hint = "Resize terminal to continue  |  q:Quit";

    int base_row = rows / 2 - 2;
    if (base_row < 0)
        base_row = 0;

    werase(stdscr);
    wbkgd(stdscr, COLOR_PAIR(COLOR_NORMAL));
    curs_set(0);

    wattron(stdscr, COLOR_PAIR(COLOR_WARNING) | A_BOLD);
    ui_draw_centered_line(base_row, title);
    wattroff(stdscr, COLOR_PAIR(COLOR_WARNING) | A_BOLD);

    ui_draw_centered_line(base_row + 2, current);
    ui_draw_centered_line(base_row + 3, required);
    ui_draw_centered_line(base_row + 5, hint);

    wnoutrefresh(stdscr);
    doupdate();
}

static void ui_touch_layout_windows(void) {
    if (!state.layout_ready)
        return;
    touchwin(state.header);
    touchwin(state.sidebar);
    touchwin(state.content);
    touchwin(state.status);
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
                          screen_info[i].label);
                wattroff(state.sidebar, A_DIM | A_REVERSE);
            } else {
                wattron(state.sidebar, COLOR_PAIR(COLOR_SELECTED));
                mvwprintw(state.sidebar, i + 1, 1, " %-*s", SIDEBAR_WIDTH - 3,
                          screen_info[i].label);
                wattroff(state.sidebar, COLOR_PAIR(COLOR_SELECTED));
            }
        } else {
            mvwprintw(state.sidebar, i + 1, 2, "%-*s", SIDEBAR_WIDTH - 3,
                      screen_info[i].label);
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
    } else if (state.current_screen == SCREEN_CATEGORIES) {
        if (!state.category_list)
            state.category_list = category_list_create(state.db);
        if (state.category_list)
            category_list_draw(state.category_list, state.content,
                               state.content_focused);
    } else if (state.current_screen == SCREEN_ACCOUNTS) {
        if (!state.account_list)
            state.account_list = account_list_create(state.db);
        if (state.account_list)
            account_list_draw(state.account_list, state.content,
                              state.content_focused);
    } else if (state.current_screen == SCREEN_BUDGETS) {
        if (!state.budget_list)
            state.budget_list = budget_list_create(state.db);
        if (state.budget_list)
            budget_list_draw(state.budget_list, state.content,
                             state.content_focused);
    } else if (state.current_screen == SCREEN_REPORTS) {
        if (!state.report_list)
            state.report_list = report_list_create(state.db);
        if (state.report_list)
            report_list_draw(state.report_list, state.content,
                             state.content_focused);
    } else {
        int h, w;
        getmaxyx(state.content, h, w);
        const char *title = screen_info[state.current_screen].label;
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
               state.current_screen == SCREEN_CATEGORIES &&
               state.category_list) {
        mvwprintw(state.status, 0, 1, "%s",
                  category_list_status_hint(state.category_list));
    } else if (state.content_focused &&
               state.current_screen == SCREEN_ACCOUNTS && state.account_list) {
        mvwprintw(state.status, 0, 1, "%s",
                  account_list_status_hint(state.account_list));
    } else if (state.content_focused &&
               state.current_screen == SCREEN_BUDGETS && state.budget_list) {
        mvwprintw(state.status, 0, 1, "%s",
                  budget_list_status_hint(state.budget_list));
    } else if (state.content_focused &&
               state.current_screen == SCREEN_REPORTS && state.report_list) {
        mvwprintw(state.status, 0, 1, "%s",
                  report_list_status_hint(state.report_list));
    } else {
        mvwprintw(state.status, 0, 1,
                  "q:Quit  a:Add  i:Import  L:Auto-link  t:Theme  ?:Help  "
                  "\u2191\u2193:Navigate  Enter:Select");
    }
    wnoutrefresh(state.status);
}

static void ui_draw_all(void) {
    if (!state.layout_ready)
        return;
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
        if (ui_requeue_resize_event(ch)) {
            done = true;
            continue;
        }
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
    ui_touch_layout_windows();
}

typedef struct {
    int64_t id;
    int64_t account_id;
    char account_name[64];
    transaction_type_t type;
    int64_t amount_cents;
    char date[11];
    char payee[128];
} link_scan_txn_t;

typedef enum {
    LINK_PICK_SELECT = 0,
    LINK_PICK_SKIP = 1,
    LINK_PICK_CANCEL = 2
} link_pick_result_t;

typedef struct {
    int scanned;
    int linked;
    int skipped;
    int ambiguous_prompts;
    bool cancelled;
} auto_link_result_t;

static const char *ui_txn_type_label(transaction_type_t type) {
    return (type == TRANSACTION_INCOME) ? "Income" : "Expense";
}

static void ui_format_amount_plain(int64_t cents, char *buf, size_t buflen) {
    int64_t abs_cents = cents < 0 ? -cents : cents;
    snprintf(buf, buflen, "%ld.%02ld", (long)(abs_cents / 100),
             (long)(abs_cents % 100));
}

static bool ui_confirm_auto_link_start(WINDOW *parent) {
    if (!parent)
        return false;

    int ph, pw;
    getmaxyx(parent, ph, pw);
    int win_h = 9;
    int win_w = 74;
    if (win_h > ph)
        win_h = ph;
    if (win_w > pw)
        win_w = pw;
    if (win_h < 6 || win_w < 42)
        return false;

    int py, px;
    getbegyx(parent, py, px);
    WINDOW *w = newwin(win_h, win_w, py + (ph - win_h) / 2, px + (pw - win_w) / 2);
    keypad(w, TRUE);
    wbkgd(w, COLOR_PAIR(COLOR_FORM));
    box(w, 0, 0);
    mvwprintw(w, 0, 2, " Auto-link Transfers ");
    mvwprintw(w, 2, 2, "Scan all accounts and auto-link potential transfers?");
    mvwprintw(
        w, 4, 2,
        "Rule: same amount + opposite type + within %d days across accounts.",
        AUTO_LINK_DATE_WINDOW_DAYS);
    mvwprintw(w, win_h - 2, 2, "y:Start  n:Cancel");
    wrefresh(w);

    bool confirmed = false;
    bool done = false;
    while (!done) {
        int ch = wgetch(w);
        if (ui_requeue_resize_event(ch)) {
            done = true;
            confirmed = false;
            continue;
        }
        switch (ch) {
        case 'y':
        case 'Y':
            confirmed = true;
            done = true;
            break;
        case 'n':
        case 'N':
        case 27:
            confirmed = false;
            done = true;
            break;
        default:
            break;
        }
    }

    delwin(w);
    touchwin(parent);
    return confirmed;
}

static void ui_show_info_popup(WINDOW *parent, const char *title,
                               const char *line1, const char *line2) {
    if (!parent)
        return;

    int ph, pw;
    getmaxyx(parent, ph, pw);
    int win_h = 8;
    int win_w = 72;
    if (win_h > ph)
        win_h = ph;
    if (win_w > pw)
        win_w = pw;
    if (win_h < 5 || win_w < 30)
        return;

    int py, px;
    getbegyx(parent, py, px);
    WINDOW *w = newwin(win_h, win_w, py + (ph - win_h) / 2, px + (pw - win_w) / 2);
    keypad(w, TRUE);
    wbkgd(w, COLOR_PAIR(COLOR_FORM));
    box(w, 0, 0);
    mvwprintw(w, 0, 2, "%s", title && title[0] ? title : " Result ");
    mvwprintw(w, 2, 2, "%-*.*s", win_w - 4, win_w - 4, line1 ? line1 : "");
    mvwprintw(w, 3, 2, "%-*.*s", win_w - 4, win_w - 4, line2 ? line2 : "");
    mvwprintw(w, win_h - 2, 2, "Press any key");
    wrefresh(w);

    int ch = wgetch(w);
    (void)ui_requeue_resize_event(ch);
    delwin(w);
    touchwin(parent);
}

static int ui_collect_unlinked_transactions(sqlite3 *db, link_scan_txn_t **out) {
    if (!db || !out)
        return -1;
    *out = NULL;

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(
        db,
        "SELECT t.id, t.account_id, a.name, t.type, t.amount_cents, t.date,"
        "       COALESCE(t.payee, '')"
        " FROM transactions t"
        " JOIN accounts a ON a.id = t.account_id"
        " WHERE t.transfer_id IS NULL"
        "   AND t.type IN ('EXPENSE', 'INCOME')"
        " ORDER BY t.date, t.id",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return -1;

    int cap = 64;
    int count = 0;
    link_scan_txn_t *rows = malloc((size_t)cap * sizeof(*rows));
    if (!rows) {
        sqlite3_finalize(stmt);
        return -1;
    }

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (count >= cap) {
            cap *= 2;
            link_scan_txn_t *tmp =
                realloc(rows, (size_t)cap * sizeof(*rows));
            if (!tmp) {
                free(rows);
                sqlite3_finalize(stmt);
                return -1;
            }
            rows = tmp;
        }

        link_scan_txn_t *row = &rows[count++];
        memset(row, 0, sizeof(*row));
        row->id = sqlite3_column_int64(stmt, 0);
        row->account_id = sqlite3_column_int64(stmt, 1);
        const char *acct = (const char *)sqlite3_column_text(stmt, 2);
        snprintf(row->account_name, sizeof(row->account_name), "%s",
                 acct ? acct : "");
        const char *type = (const char *)sqlite3_column_text(stmt, 3);
        row->type = (type && strcmp(type, "INCOME") == 0) ? TRANSACTION_INCOME
                                                            : TRANSACTION_EXPENSE;
        row->amount_cents = sqlite3_column_int64(stmt, 4);
        const char *date = (const char *)sqlite3_column_text(stmt, 5);
        snprintf(row->date, sizeof(row->date), "%s", date ? date : "");
        const char *payee = (const char *)sqlite3_column_text(stmt, 6);
        snprintf(row->payee, sizeof(row->payee), "%s", payee ? payee : "");
    }

    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        free(rows);
        return -1;
    }

    if (count == 0) {
        free(rows);
        rows = NULL;
    }
    *out = rows;
    return count;
}

static int ui_find_transfer_matches(sqlite3 *db, const transaction_t *base,
                                    link_scan_txn_t **out) {
    if (!db || !base || !out)
        return -1;
    *out = NULL;
    if (base->type != TRANSACTION_EXPENSE && base->type != TRANSACTION_INCOME)
        return 0;

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(
        db,
        "SELECT t.id, t.account_id, a.name, t.type, t.amount_cents, t.date,"
        "       COALESCE(t.payee, '')"
        " FROM transactions t"
        " JOIN accounts a ON a.id = t.account_id"
        " WHERE t.transfer_id IS NULL"
        "   AND t.id != ?"
        "   AND t.account_id != ?"
        "   AND t.type != 'TRANSFER'"
        "   AND t.amount_cents = ?"
        "   AND ABS(julianday(t.date) - julianday(?)) <= ?"
        " ORDER BY ABS(julianday(t.date) - julianday(?)) ASC, t.id DESC",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return -1;

    sqlite3_bind_int64(stmt, 1, base->id);
    sqlite3_bind_int64(stmt, 2, base->account_id);
    sqlite3_bind_int64(stmt, 3, base->amount_cents);
    sqlite3_bind_text(stmt, 4, base->date, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 5, AUTO_LINK_DATE_WINDOW_DAYS);
    sqlite3_bind_text(stmt, 6, base->date, -1, SQLITE_STATIC);

    int cap = 8;
    int count = 0;
    link_scan_txn_t *rows = malloc((size_t)cap * sizeof(*rows));
    if (!rows) {
        sqlite3_finalize(stmt);
        return -1;
    }

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (count >= cap) {
            cap *= 2;
            link_scan_txn_t *tmp =
                realloc(rows, (size_t)cap * sizeof(*rows));
            if (!tmp) {
                free(rows);
                sqlite3_finalize(stmt);
                return -1;
            }
            rows = tmp;
        }

        link_scan_txn_t *row = &rows[count++];
        memset(row, 0, sizeof(*row));
        row->id = sqlite3_column_int64(stmt, 0);
        row->account_id = sqlite3_column_int64(stmt, 1);
        const char *acct = (const char *)sqlite3_column_text(stmt, 2);
        snprintf(row->account_name, sizeof(row->account_name), "%s",
                 acct ? acct : "");
        const char *type = (const char *)sqlite3_column_text(stmt, 3);
        row->type = (type && strcmp(type, "INCOME") == 0) ? TRANSACTION_INCOME
                                                            : TRANSACTION_EXPENSE;
        row->amount_cents = sqlite3_column_int64(stmt, 4);
        const char *date = (const char *)sqlite3_column_text(stmt, 5);
        snprintf(row->date, sizeof(row->date), "%s", date ? date : "");
        const char *payee = (const char *)sqlite3_column_text(stmt, 6);
        snprintf(row->payee, sizeof(row->payee), "%s", payee ? payee : "");
    }

    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        free(rows);
        return -1;
    }

    if (count == 0) {
        free(rows);
        rows = NULL;
    }
    *out = rows;
    return count;
}

static link_pick_result_t
ui_pick_transfer_match(WINDOW *parent, const transaction_t *base,
                       const char *base_account_name,
                       const link_scan_txn_t *matches, int match_count,
                       int *out_index) {
    if (!parent || !base || !matches || match_count <= 0 || !out_index)
        return LINK_PICK_SKIP;

    int ph, pw;
    getmaxyx(parent, ph, pw);
    int win_h = 14;
    int win_w = 92;
    if (win_h > ph)
        win_h = ph;
    if (win_w > pw)
        win_w = pw;
    if (win_h < 8 || win_w < 44)
        return LINK_PICK_SKIP;

    int py, px;
    getbegyx(parent, py, px);
    WINDOW *w = newwin(win_h, win_w, py + (ph - win_h) / 2, px + (pw - win_w) / 2);
    keypad(w, TRUE);
    wbkgd(w, COLOR_PAIR(COLOR_FORM));

    int sel = 0;
    int scroll = 0;
    int list_top = 5;
    int list_rows = win_h - list_top - 2;
    if (list_rows < 1)
        list_rows = 1;

    char amount[24];
    ui_format_amount_plain(base->amount_cents, amount, sizeof(amount));

    while (1) {
        werase(w);
        box(w, 0, 0);
        mvwprintw(w, 0, 2, " Auto-link Transfers ");
        mvwprintw(
            w, 1, 2,
            "Multiple matches found. Choose a transaction to link or skip.");
        mvwprintw(w, 2, 2, "Current: %-14.14s | %s | %s | %s %s | %-.18s",
                  base_account_name ? base_account_name : "", base->date,
                  ui_txn_type_label(base->type), amount,
                  (base->type == TRANSACTION_EXPENSE) ? "out" : "in",
                  base->payee);
        mvwprintw(w, 3, 2, "Keys: Enter=link  s=skip  Esc=cancel run");

        if (sel < scroll)
            scroll = sel;
        if (sel >= scroll + list_rows)
            scroll = sel - list_rows + 1;

        for (int i = 0; i < list_rows; i++) {
            int idx = scroll + i;
            if (idx >= match_count)
                break;
            int row = list_top + i;
            bool active = (idx == sel);
            if (active)
                wattron(w, COLOR_PAIR(COLOR_FORM_ACTIVE));
            mvwprintw(w, row, 2, "%-*s", win_w - 4, "");
            mvwprintw(w, row, 2, "%2d) %-16.16s | %-10.10s | %-7.7s | %-.38s",
                      idx + 1, matches[idx].account_name, matches[idx].date,
                      ui_txn_type_label(matches[idx].type), matches[idx].payee);
            if (active)
                wattroff(w, COLOR_PAIR(COLOR_FORM_ACTIVE));
        }

        if (scroll > 0)
            mvwprintw(w, list_top, win_w - 2, "\u25b2");
        if (scroll + list_rows < match_count)
            mvwprintw(w, list_top + list_rows - 1, win_w - 2, "\u25bc");

        wrefresh(w);
        int ch = wgetch(w);
        if (ui_requeue_resize_event(ch)) {
            delwin(w);
            touchwin(parent);
            return LINK_PICK_CANCEL;
        }

        if (ch == KEY_UP || ch == 'k') {
            if (sel > 0)
                sel--;
            continue;
        }
        if (ch == KEY_DOWN || ch == 'j') {
            if (sel < match_count - 1)
                sel++;
            continue;
        }
        if (ch == '\n' || ch == KEY_ENTER) {
            *out_index = sel;
            delwin(w);
            touchwin(parent);
            return LINK_PICK_SELECT;
        }
        if (ch == 's' || ch == 'S') {
            delwin(w);
            touchwin(parent);
            return LINK_PICK_SKIP;
        }
        if (ch == 27) {
            delwin(w);
            touchwin(parent);
            return LINK_PICK_CANCEL;
        }
    }
}

static int ui_auto_link_transfers(WINDOW *parent, sqlite3 *db,
                                  auto_link_result_t *out_result) {
    if (!parent || !db || !out_result)
        return -1;
    memset(out_result, 0, sizeof(*out_result));

    link_scan_txn_t *snapshot = NULL;
    int n = ui_collect_unlinked_transactions(db, &snapshot);
    if (n < 0)
        return -1;

    for (int i = 0; i < n; i++) {
        transaction_t base = {0};
        if (db_get_transaction_by_id(db, (int)snapshot[i].id, &base) != 0)
            continue;
        if (base.transfer_id != 0 || base.type == TRANSACTION_TRANSFER)
            continue;
        if (base.type != TRANSACTION_EXPENSE && base.type != TRANSACTION_INCOME)
            continue;
        out_result->scanned++;

        link_scan_txn_t *matches = NULL;
        int match_count = ui_find_transfer_matches(db, &base, &matches);
        if (match_count < 0) {
            free(snapshot);
            return -1;
        }
        if (match_count == 0) {
            free(matches);
            continue;
        }

        int pick_idx = 0;
        if (match_count > 1) {
            int opposite_count = 0;
            int opposite_idx = -1;
            for (int j = 0; j < match_count; j++) {
                if ((base.type == TRANSACTION_EXPENSE &&
                     matches[j].type == TRANSACTION_INCOME) ||
                    (base.type == TRANSACTION_INCOME &&
                     matches[j].type == TRANSACTION_EXPENSE)) {
                    opposite_count++;
                    if (opposite_count == 1)
                        opposite_idx = j;
                }
            }

            if (opposite_count == 1 && opposite_idx >= 0) {
                pick_idx = opposite_idx;
            } else {
                out_result->ambiguous_prompts++;
                link_pick_result_t pick =
                    ui_pick_transfer_match(parent, &base,
                                           snapshot[i].account_name, matches,
                                           match_count, &pick_idx);
                if (pick == LINK_PICK_CANCEL) {
                    out_result->cancelled = true;
                    free(matches);
                    break;
                }
                if (pick == LINK_PICK_SKIP) {
                    out_result->skipped++;
                    free(matches);
                    continue;
                }
            }
        }

        int64_t source_id = 0;
        int64_t to_account_id = 0;
        if (base.type == TRANSACTION_EXPENSE) {
            source_id = base.id;
            to_account_id = matches[pick_idx].account_id;
        } else {
            source_id = matches[pick_idx].id;
            to_account_id = base.account_id;
        }

        transaction_t source = {0};
        if (db_get_transaction_by_id(db, (int)source_id, &source) != 0) {
            out_result->skipped++;
            free(matches);
            continue;
        }
        if (source.transfer_id != 0 || source.type == TRANSACTION_TRANSFER) {
            out_result->skipped++;
            free(matches);
            continue;
        }

        source.type = TRANSACTION_TRANSFER;
        source.category_id = 0;
        source.payee[0] = '\0';
        int rc = db_update_transfer(db, &source, to_account_id, true);
        if (rc == 0)
            out_result->linked++;
        else
            out_result->skipped++;

        free(matches);
    }

    free(snapshot);
    return 0;
}

static void ui_handle_input(int ch) {
    // When content is focused, delegate to the active content handler first
    if (state.content_focused) {
        // Delegate to list handler; if it consumes the key, we're done
        if (state.current_screen == SCREEN_TRANSACTIONS && state.txn_list) {
            if (txn_list_handle_input(state.txn_list, state.content, ch)) {
                if (state.budget_list &&
                    (ch == 'e' || ch == 'c' || ch == 'd')) {
                    budget_list_mark_dirty(state.budget_list);
                }
                if (state.report_list &&
                    (ch == 'e' || ch == 'c' || ch == 'd' || ch == 'D')) {
                    report_list_mark_dirty(state.report_list);
                }
                return;
            }
        }
        if (state.current_screen == SCREEN_ACCOUNTS && state.account_list) {
            if (account_list_handle_input(state.account_list, state.content,
                                          ch)) {
                bool account_changed =
                    account_list_consume_changed(state.account_list);
                if (account_changed && state.txn_list) {
                    txn_list_mark_dirty(state.txn_list);
                }
                if (account_changed && state.budget_list) {
                    budget_list_mark_dirty(state.budget_list);
                }
                if (account_changed && state.report_list) {
                    report_list_mark_dirty(state.report_list);
                }
                return;
            }
        }
        if (state.current_screen == SCREEN_CATEGORIES && state.category_list) {
            if (category_list_handle_input(state.category_list, state.content,
                                           ch)) {
                bool category_changed =
                    category_list_consume_changed(state.category_list);
                if (category_changed && state.txn_list) {
                    txn_list_mark_dirty(state.txn_list);
                }
                if (category_changed && state.budget_list) {
                    budget_list_mark_dirty(state.budget_list);
                }
                if (category_changed && state.report_list) {
                    report_list_mark_dirty(state.report_list);
                }
                return;
            }
        }
        if (state.current_screen == SCREEN_BUDGETS && state.budget_list) {
            if (budget_list_handle_input(state.budget_list, state.content, ch))
                return;
        }
        if (state.current_screen == SCREEN_REPORTS && state.report_list) {
            if (report_list_handle_input(state.report_list, state.content, ch))
                return;
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
        state.content_focused =
            screen_info[state.current_screen].content_focusable;
        break;
    case 'a': {
        transaction_t txn = {0};
        form_result_t res =
            form_transaction(state.content, state.db, &txn, false);
        if (res == FORM_SAVED && state.txn_list)
            txn_list_mark_dirty(state.txn_list);
        if (res == FORM_SAVED && state.budget_list)
            budget_list_mark_dirty(state.budget_list);
        if (res == FORM_SAVED && state.report_list)
            report_list_mark_dirty(state.report_list);
    }
        ui_touch_layout_windows();
        break;
    case 'i': {
        int64_t acct_id = state.txn_list
                              ? txn_list_get_current_account_id(state.txn_list)
                              : 0;
        int n = import_dialog(state.content, state.db, acct_id);
        if (n > 0 && state.txn_list)
            txn_list_mark_dirty(state.txn_list);
        if (n > 0 && state.budget_list)
            budget_list_mark_dirty(state.budget_list);
        if (n > 0 && state.report_list)
            report_list_mark_dirty(state.report_list);
        ui_touch_layout_windows();
    } break;
    case 'L': {
        if (!ui_confirm_auto_link_start(state.content))
            break;

        auto_link_result_t result = {0};
        if (ui_auto_link_transfers(state.content, state.db, &result) < 0) {
            ui_show_error_popup(state.content, " Auto-link Error ",
                                "Failed while scanning/linking transactions.");
            ui_touch_layout_windows();
            break;
        }

        if (result.linked > 0 && state.txn_list)
            txn_list_mark_dirty(state.txn_list);
        if (result.linked > 0 && state.budget_list)
            budget_list_mark_dirty(state.budget_list);
        if (result.linked > 0 && state.report_list)
            report_list_mark_dirty(state.report_list);

        char line1[160];
        char line2[160];
        snprintf(line1, sizeof(line1),
                 "Scanned: %d  Linked: %d  Skipped: %d", result.scanned,
                 result.linked, result.skipped);
        snprintf(line2, sizeof(line2),
                 "Ambiguous prompts: %d%s", result.ambiguous_prompts,
                 result.cancelled ? "  (run cancelled early)" : "");
        ui_show_info_popup(state.content, " Auto-link Transfers ", line1, line2);
        ui_touch_layout_windows();
    } break;
    case 't':
        state.dark_mode = !state.dark_mode;
        ui_apply_theme(state.dark_mode);
        ui_save_theme_pref(state.dark_mode);

        wbkgd(stdscr, COLOR_PAIR(COLOR_NORMAL));
        if (state.layout_ready) {
            wbkgd(state.sidebar, COLOR_PAIR(COLOR_NORMAL));
            wbkgd(state.content, COLOR_PAIR(COLOR_NORMAL));
            ui_touch_layout_windows();
        }
        break;
    case '?':
        ui_show_help();
        break;
    case KEY_RESIZE:
        ui_handle_resize_event();
        break;
    }
}

void ui_init(void) {
    setlocale(LC_ALL, "");
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    // Keep Esc responsive while still allowing escape-sequence keys to parse.
#ifdef NCURSES_VERSION
    (void)set_escdelay(150);
#endif
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
    state.category_list = NULL;
    state.budget_list = NULL;
    state.report_list = NULL;
    state.layout_ready = false;
    state.layout_rows = 0;
    state.layout_cols = 0;

    ui_sync_layout_to_terminal();
    refresh(); // sync stdscr so getch() won't blank the screen

    while (state.running) {
        ui_sync_layout_to_terminal();
        if (!state.layout_ready) {
            ui_draw_too_small_screen();
            int ch = getch();
            if (ch == 'q') {
                state.running = false;
            } else if (ch == KEY_RESIZE) {
                ui_handle_resize_event();
            }
            continue;
        }

        ui_draw_all();
        int ch = getch();
        ui_handle_input(ch);
    }

    txn_list_destroy(state.txn_list);
    state.txn_list = NULL;
    account_list_destroy(state.account_list);
    state.account_list = NULL;
    category_list_destroy(state.category_list);
    state.category_list = NULL;
    budget_list_destroy(state.budget_list);
    state.budget_list = NULL;
    report_list_destroy(state.report_list);
    state.report_list = NULL;
    ui_destroy_layout();
}
