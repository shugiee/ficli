#ifndef FICLI_CATEGORY_H
#define FICLI_CATEGORY_H

#include <stdint.h>

typedef enum {
    CATEGORY_EXPENSE,
    CATEGORY_INCOME
} category_type_t;

typedef struct {
    int64_t id;
    char name[64];
    category_type_t type;
    int64_t parent_id;  // 0 if top-level
} category_t;

#endif
