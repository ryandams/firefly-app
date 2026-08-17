// SPDX-License-Identifier: Apache-2.0
#include <zephyr/ztest.h>
#include <errno.h>
#include "thread.hpp"

namespace
{
class ContractThread final : public firefly::Thread<1024>
{
  public:
    ContractThread() : Thread(5, "contract")
    {
        k_sem_init(&entered_, 0, 1);
    }
    int waitEntered()
    {
        return k_sem_take(&entered_, K_MSEC(500));
    }
    uint32_t entries() const
    {
        return static_cast<uint32_t>(atomic_get(&entries_));
    }

  protected:
    void run() override
    {
        atomic_inc(&entries_);
        k_sem_give(&entered_);
        while (!shouldStop())
        {
            k_sleep(K_MSEC(1));
        }
    }

  private:
    struct k_sem entered_{};
    atomic_t entries_{0};
};
ContractThread thread;
void before(void *)
{
    zassert_ok(thread.stop());
}
void after(void *)
{
    zassert_ok(thread.stop());
}
} // namespace

ZTEST(thread_contract, test_stop_before_start_and_duplicate_stop)
{
    zassert_ok(thread.stop());
    zassert_ok(thread.stop());
    zassert_false(thread.isRunning());
}

ZTEST(thread_contract, test_start_duplicate_stop_and_restart)
{
    const uint32_t before_entries = thread.entries();
    zassert_ok(thread.start());
    zassert_ok(thread.waitEntered());
    zassert_true(thread.isRunning());
    zassert_equal(thread.start(), -EALREADY);
    zassert_ok(thread.stop());
    zassert_false(thread.isRunning());
    zassert_ok(thread.start());
    zassert_ok(thread.waitEntered());
    zassert_equal(thread.entries(), before_entries + 2);
}

ZTEST(thread_contract, test_stop_is_bounded)
{
    zassert_ok(thread.start());
    zassert_ok(thread.waitEntered());
    const int64_t start = k_uptime_get();
    zassert_ok(thread.stop());
    zassert_true(k_uptime_get() - start < 500);
}

ZTEST(thread_contract, test_stack_has_measured_safety_margin)
{
    zassert_ok(thread.start());
    zassert_ok(thread.waitEntered());
    size_t unused = 0;
    zassert_ok(thread.stackUnused(&unused));
    zassert_true(unused >= 128, "thread stack safety margin is only %zu bytes", unused);
}

ZTEST_SUITE(thread_contract, NULL, NULL, before, after, NULL);
