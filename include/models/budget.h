#ifndef FICLI_BUDGET_H
#define FICLI_BUDGET_H

#include <stdint.h>

typedef struct {
    int64_t id;
    int64_t category_id;
    char month[8];       // "YYYY-MM\0"
    int64_t limit_cents;
} budget_t;

#endif
