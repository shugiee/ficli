#include "db/db.h"
#include "ui/ui.h"

#include <ncurses.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    // Build the database path: ~/.local/share/ficli/ficli.db
    const char *home = getenv("HOME");
    if (!home) {
        fprintf(stderr, "HOME environment variable not set\n");
        return 1;
    }

    char db_path[512];
    snprintf(db_path, sizeof(db_path), "%s/.local/share/ficli/ficli.db", home);

    sqlite3 *db = db_init(db_path);
    if (!db) {
        return 1;
    }

    ui_init();
    ui_draw_welcome();
    getch();
    ui_cleanup();

    db_close(db);
    return 0;
}
