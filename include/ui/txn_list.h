#ifndef FICLI_TXN_LIST_H
#define FICLI_TXN_LIST_H

#include <ncurses.h>
#include <sqlite3.h>
#include <stdbool.h>

typedef struct txn_list_state txn_list_state_t;

txn_list_state_t *txn_list_create(sqlite3 *db);
void              txn_list_destroy(txn_list_state_t *ls);
void              txn_list_draw(txn_list_state_t *ls, WINDOW *win, bool focused);
bool              txn_list_handle_input(txn_list_state_t *ls, int ch);
const char       *txn_list_status_hint(const txn_list_state_t *ls);
void              txn_list_mark_dirty(txn_list_state_t *ls);

#endif
