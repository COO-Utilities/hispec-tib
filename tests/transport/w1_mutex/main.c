#include <zephyr/drivers/w1.h>
#include <zephyr/ztest.h>

static K_THREAD_STACK_DEFINE(waiter_stack, 1024);
static struct k_thread waiter;
static K_SEM_DEFINE(entered, 0, 1);
static K_SEM_DEFINE(acquired, 0, 1);

/* Exercise the real driver's mutex; a zeroed, uninitialized wait queue only
 * fails when another thread contends, so uncontended lock/unlock is insufficient.
 */
static void contend(void *bus, void *unused1, void *unused2)
{
	ARG_UNUSED(unused1);
	ARG_UNUSED(unused2);
	k_sem_give(&entered);
	zassert_ok(w1_lock_bus(bus));
	k_sem_give(&acquired);
	zassert_ok(w1_unlock_bus(bus));
}

ZTEST(w1_mutex, test_both_bus_mutexes_contended)
{
	const struct device *buses[] = {
		DEVICE_DT_GET(DT_NODELABEL(relay_bus)),
		DEVICE_DT_GET(DT_NODELABEL(temperature_bus)),
	};

	for (size_t i = 0; i < ARRAY_SIZE(buses); ++i) {
		zassert_true(device_is_ready(buses[i]));
		zassert_ok(w1_lock_bus(buses[i]));
		/* Nested native-bus operations must retain recursive ownership. */
		zassert_ok(w1_lock_bus(buses[i]));
		zassert_ok(w1_unlock_bus(buses[i]));
		k_thread_create(&waiter, waiter_stack, K_THREAD_STACK_SIZEOF(waiter_stack),
				contend, (void *)buses[i], NULL, NULL,
				K_PRIO_PREEMPT(0), 0, K_NO_WAIT);
		zassert_ok(k_sem_take(&entered, K_SECONDS(1)));
		zassert_equal(k_sem_take(&acquired, K_MSEC(10)), -EAGAIN);
		zassert_ok(w1_unlock_bus(buses[i]));
		zassert_ok(k_sem_take(&acquired, K_SECONDS(1)));
		zassert_ok(k_thread_join(&waiter, K_SECONDS(1)));
		zassert_ok(w1_lock_bus(buses[i]));
		zassert_ok(w1_unlock_bus(buses[i]));
	}
}

ZTEST_SUITE(w1_mutex, NULL, NULL, NULL, NULL, NULL);
