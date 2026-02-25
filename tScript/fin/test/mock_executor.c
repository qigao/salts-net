/**
 * @file mock_executor.c
 * @brief A mock order executor compiled as a DLL for testing the loader.
 */

#include "order_executor_plugin.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char name[64];
    int count;
} mock_impl_t;

static order_error_t mock_submit(order_executor_t *self, order_t *order) {
    mock_impl_t *impl = (mock_impl_t *)self->user_data;
    impl->count++;
    order->status = ORDER_STATUS_PENDING;
    printf("[MockExecutor] Submitted order %llu for asset %u\n", order->id, order->asset_id);
    return ORDER_OK;
}

static void mock_cancel(order_executor_t *self, uint64_t order_id) {
    (void)self;
    printf("[MockExecutor] Cancelled order %llu\n", order_id);
}

static void mock_sync(order_executor_t *self, order_manager_t *mgr) {
    (void)self;
    (void)mgr;
    printf("[MockExecutor] Syncing state...\n");
}

EXPORT order_executor_t* turbo_executor_create(const char *config_json) {
    printf("[MockExecutor] Creating with config: %s\n", config_json ? config_json : "NULL");
    
    mock_impl_t *impl = (mock_impl_t *)malloc(sizeof(mock_impl_t));
    if (!impl) return NULL;
    memset(impl, 0, sizeof(mock_impl_t));
    strncpy(impl->name, "MockDLL", sizeof(impl->name) - 1);

    order_executor_t *exec = (order_executor_t *)malloc(sizeof(order_executor_t));
    if (!exec) { free(impl); return NULL; }
    memset(exec, 0, sizeof(order_executor_t));

    exec->name = impl->name;
    exec->user_data = impl;
    exec->submit = mock_submit;
    exec->cancel = mock_cancel;
    exec->sync   = mock_sync;

    return exec;
}

EXPORT void turbo_executor_destroy(order_executor_t *self) {
    if (!self) return;
    if (self->user_data) free(self->user_data);
    free(self);
    printf("[MockExecutor] Destroyed\n");
}
