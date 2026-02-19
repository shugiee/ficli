#ifndef FICLI_CATEGORY_LIST_H
#define FICLI_CATEGORY_LIST_H

#include <ncurses.h>
#include <sqlite3.h>
#include <stdbool.h>

typedef struct category_list_state category_list_state_t;

category_list_state_t *category_list_create(sqlite3 *db);
void                   category_list_destroy(category_list_state_t *ls);
void                   category_list_draw(category_list_state_t *ls, WINDOW *win,
                                          bool focused);
bool                   category_list_handle_input(category_list_state_t *ls,
                                                  WINDOW *parent, int ch);
const char            *category_list_status_hint(const category_list_state_t *ls);
void                   category_list_mark_dirty(category_list_state_t *ls);
bool                   category_list_consume_changed(category_list_state_t *ls);

#endif
