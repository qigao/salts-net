#include "order_manager.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

static int g_submit_count = 0;
static int g_cancel_count = 0;

static order_error_t mock_submit(order_executor_t *self, order_t *order) {
    (void)self;
    (void)order;
    g_submit_count++;
    return ORDER_OK;
}

static void mock_cancel(order_executor_t *self, uint64_t order_id) {
    (void)self;
    (void)order_id;
    g_cancel_count++;
}

static void mock_sync(order_executor_t *self, order_manager_t *mgr) {
    (void)self;
    (void)mgr;
}

void test_live_order_manager() {
    printf("Testing Live Order Manager...\n");

    order_executor_t mock;
    mock.name = "MockExecutor";
    mock.user_data = NULL;
    mock.submit = mock_submit;
    mock.cancel = mock_cancel;
    mock.sync   = mock_sync;

    order_manager_t *mgr = order_manager_create(NULL, 100000.0, 10, NULL);
    order_manager_set_executor(mgr, &mock);

    // 1. Test Market Order submission
    order_manager_market(mgr, 123, 1, 0.1, 100.0, 100.0, 0, 0);
    assert(g_submit_count == 1);
    // In live mode, internal position should NOT update immediately (waiting for fill)
    assert(order_manager_position(mgr, 123) == 0.0);

    // 2. Test Limit Order submission
    uint64_t oid = 0;
    order_manager_limit(mgr, 123, 1, 0.1, 95.0, 0, 0, 1, &oid);
    assert(g_submit_count == 2);
    assert(oid > 0);

    // 3. Test Cancel
    order_manager_cancel_order(mgr, oid);
    assert(g_cancel_count == 1);

    // 4. Test Close (Flat)
    // Manually inject a position to test close
    for(size_t i=0; i<mgr->capacity; i++) {
        if(mgr->positions[i].asset_id == 0) {
            mgr->positions[i].asset_id = 456;
            mgr->positions[i].position = 100.0;
            break;
        }
    }
    order_manager_close(mgr, 456, 110.0, 0);
    assert(g_submit_count == 3);

    order_manager_free(mgr);
    printf("Live Order Manager tests passed!\n");
}

int main() {
    test_live_order_manager();
    return 0;
}
