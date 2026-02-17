#ifndef FICLI_ACCOUNT_H
#define FICLI_ACCOUNT_H

#include <stdint.h>

typedef struct {
    int64_t id;
    char name[64];
} account_t;

#endif
