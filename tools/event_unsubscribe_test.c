/* Build twice with the SDK, with and without TEST_ASYNC_API, and without
 * linking libaxevent. --gc-sections discards the unused ACAP runtime. */
#include "../app/ACAP.c"
#undef puts

static GMutex worker_mutex;
static GCond worker_cond;
static gboolean worker_started;
static gboolean worker_released;
static gboolean fail_unsubscribe;
static gboolean error_without_detail;
static int warnings;
static int legacy_calls;
static int async_calls;
#ifdef TEST_ASYNC_API
static AXUnsubscribeCompleteCallback pending_callback;
static guint pending_subscription;
static gpointer pending_user_data;
#endif

static void on_warning(const char *message) {
    g_assert_nonnull(strstr(message, "subscription"));
    warnings++;
}

gboolean ax_event_handler_unsubscribe(AXEventHandler *handler, guint subscription, GError **error) {
    g_assert_true(handler == ACAP_EVENTS_HANDLER);
    g_assert_cmpuint(subscription, >, 0);
    g_assert_false(g_main_context_is_owner(g_main_context_default()));
    g_mutex_lock(&worker_mutex);
    legacy_calls++;
    worker_started = TRUE;
    g_cond_signal(&worker_cond);
    while (!worker_released)
        g_cond_wait(&worker_cond, &worker_mutex);
    g_mutex_unlock(&worker_mutex);
    if (fail_unsubscribe && !error_without_detail)
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "legacy test failure");
    return !fail_unsubscribe;
}

#ifdef TEST_ASYNC_API
gboolean ax_event_handler_unsubscribe_and_notify(
    AXEventHandler *handler,
    guint subscription,
    AXUnsubscribeCompleteCallback callback,
    gpointer user_data,
    GError **error) {
    g_assert_true(handler == ACAP_EVENTS_HANDLER);
    async_calls++;
    if (fail_unsubscribe) {
        if (!error_without_detail)
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "async test failure");
        return FALSE;
    }
    pending_callback = callback;
    pending_subscription = subscription;
    pending_user_data = user_data;
    return TRUE;
}
#endif

#ifndef TEST_ASYNC_API
static gboolean mark_main_loop_live(gpointer user_data) {
    gboolean *live = user_data;
    *live = TRUE;
    return G_SOURCE_REMOVE;
}
#endif

static void run_case(guint subscription, gboolean fail, gboolean no_detail) {
    fail_unsubscribe = fail;
    error_without_detail = no_detail;
    worker_started = FALSE;
    worker_released = FALSE;
    int previous_warnings = warnings;
    g_assert_true(g_main_context_acquire(g_main_context_default()));
    unsubscribe_async(subscription);
    g_main_context_release(g_main_context_default());
    g_assert_true(subscription_retired(subscription));

#ifdef TEST_ASYNC_API
    if (!fail)
        pending_callback(pending_subscription, pending_user_data, NULL);
#else
    gboolean main_loop_live = FALSE;
    g_mutex_lock(&worker_mutex);
    gint64 deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    while (!worker_started)
        g_assert_true(g_cond_wait_until(&worker_cond, &worker_mutex, deadline));
    g_mutex_unlock(&worker_mutex);
    g_idle_add(mark_main_loop_live, &main_loop_live);
    while (!main_loop_live)
        g_main_context_iteration(NULL, FALSE);
    g_assert_true(subscription_retired(subscription));
    g_mutex_lock(&worker_mutex);
    worker_released = TRUE;
    g_cond_signal(&worker_cond);
    g_mutex_unlock(&worker_mutex);
    while (fail ? warnings == previous_warnings : subscription_retired(subscription)) {
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        g_main_context_iteration(NULL, FALSE);
        g_usleep(1000);
    }
#endif
    g_assert_cmpint(warnings, ==, previous_warnings + (fail ? 1 : 0));
    g_assert_cmpint(subscription_retired(subscription), ==, fail);
}

int main(void) {
    /* The handler is opaque to the test; no event-service connection is made. */
    ACAP_EVENTS_HANDLER = (AXEventHandler *)&worker_mutex;
    ACAP_Set_Warning_Hook(on_warning);
    run_case(1, FALSE, FALSE);
    run_case(2, TRUE, FALSE);
    run_case(3, TRUE, TRUE);
#ifdef TEST_ASYNC_API
    g_assert_cmpint(async_calls, ==, 3);
    g_assert_cmpint(legacy_calls, ==, 0);
    fail_unsubscribe = FALSE;
    unsubscribe_async(4);
    GError *error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED, "completion test failure");
    pending_callback(pending_subscription, pending_user_data, error);
    g_assert_true(subscription_retired(4));
    g_assert_cmpint(warnings, ==, 3);
#else
    g_assert_cmpint(legacy_calls, ==, 3);
    g_assert_cmpint(async_calls, ==, 0);
#endif
    g_hash_table_destroy(retired_subscriptions);
    retired_subscriptions = NULL;
    g_cond_clear(&worker_cond);
    g_mutex_clear(&worker_mutex);
#ifdef TEST_ASYNC_API
    puts("Async event unsubscribe tests passed");
#else
    puts("Legacy event unsubscribe tests passed");
#endif
    return 0;
}
