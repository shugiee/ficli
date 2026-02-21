#ifndef FICLI_UI_H
#define FICLI_UI_H

#include <sqlite3.h>

typedef enum {
#define SCREEN_DEF(id, label, content_focusable) id,
#include "ui/screens.def"
#undef SCREEN_DEF
    SCREEN_COUNT
} screen_t;

void ui_init(void);
void ui_cleanup(void);
void ui_run(sqlite3 *db);

#endif
