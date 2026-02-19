#ifndef FICLI_ACCOUNT_LIST_H
#define FICLI_ACCOUNT_LIST_H

#include <ncurses.h>
#include <sqlite3.h>
#include <stdbool.h>

typedef struct account_list_state account_list_state_t;

account_list_state_t *account_list_create(sqlite3 *db);
void                  account_list_destroy(account_list_state_t *ls);
void                  account_list_draw(account_list_state_t *ls, WINDOW *win, bool focused);
bool                  account_list_handle_input(account_list_state_t *ls, WINDOW *parent, int ch);
const char           *account_list_status_hint(const account_list_state_t *ls);
void                  account_list_mark_dirty(account_list_state_t *ls);
bool                  account_list_consume_changed(account_list_state_t *ls);

#endif
