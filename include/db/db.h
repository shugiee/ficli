#ifndef FICLI_DB_H
#define FICLI_DB_H

#include <sqlite3.h>

sqlite3 *db_init(const char *path);
void db_close(sqlite3 *db);

#endif
