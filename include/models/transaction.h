#ifndef FICLI_TRANSACTION_H
#define FICLI_TRANSACTION_H

#include <stdint.h>
#include <time.h>

typedef enum {
    TRANSACTION_EXPENSE,
    TRANSACTION_INCOME,
    TRANSACTION_TRANSFER
} transaction_type_t;

typedef struct {
    int64_t id;
    int64_t amount_cents;
    transaction_type_t type;
    int64_t account_id;
    int64_t category_id;    // 0 if no category (transfers)
    char date[11];          // "YYYY-MM-DD\0"
    char payee[128];
    char description[256];
    int64_t transfer_id;    // 0 if not a transfer
    time_t created_at;
} transaction_t;

#endif
