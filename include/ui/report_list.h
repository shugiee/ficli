#ifndef FICLI_REPORT_LIST_H
#define FICLI_REPORT_LIST_H

#include <ncurses.h>
#include <sqlite3.h>
#include <stdbool.h>

typedef struct report_list_state report_list_state_t;

report_list_state_t *report_list_create(sqlite3 *db);
void                 report_list_destroy(report_list_state_t *ls);
void                 report_list_draw(report_list_state_t *ls, WINDOW *win,
                                      bool focused);
bool                 report_list_handle_input(report_list_state_t *ls,
                                              WINDOW *parent, int ch);
const char          *report_list_status_hint(const report_list_state_t *ls);
void                 report_list_mark_dirty(report_list_state_t *ls);

#endif
