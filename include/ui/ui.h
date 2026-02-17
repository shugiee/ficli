#ifndef FICLI_UI_H
#define FICLI_UI_H

#include <sqlite3.h>

typedef enum {
    SCREEN_DASHBOARD,
    SCREEN_TRANSACTIONS,
    SCREEN_CATEGORIES,
    SCREEN_BUDGETS,
    SCREEN_REPORTS,
    SCREEN_COUNT
} screen_t;

void ui_init(void);
void ui_cleanup(void);
void ui_run(sqlite3 *db);

#endif
