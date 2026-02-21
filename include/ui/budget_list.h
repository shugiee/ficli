#ifndef FICLI_BUDGET_LIST_H
#define FICLI_BUDGET_LIST_H

#include <ncurses.h>
#include <sqlite3.h>
#include <stdbool.h>

typedef struct budget_list_state budget_list_state_t;

budget_list_state_t *budget_list_create(sqlite3 *db);
void                 budget_list_destroy(budget_list_state_t *ls);
void                 budget_list_draw(budget_list_state_t *ls, WINDOW *win,
                                      bool focused);
bool                 budget_list_handle_input(budget_list_state_t *ls,
                                              WINDOW *parent, int ch);
const char          *budget_list_status_hint(const budget_list_state_t *ls);
void                 budget_list_mark_dirty(budget_list_state_t *ls);

#endif
