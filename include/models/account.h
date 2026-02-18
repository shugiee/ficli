#ifndef FICLI_ACCOUNT_H
#define FICLI_ACCOUNT_H

#include <stdint.h>

typedef enum {
    ACCOUNT_CASH,
    ACCOUNT_CHECKING,
    ACCOUNT_SAVINGS,
    ACCOUNT_CREDIT_CARD,
    ACCOUNT_PHYSICAL_ASSET,
    ACCOUNT_INVESTMENT,
    ACCOUNT_TYPE_COUNT
} account_type_t;

typedef struct {
    int64_t id;
    char name[64];
    account_type_t type;
} account_t;

#endif
