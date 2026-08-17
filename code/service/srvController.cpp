// SPDX-License-Identifier: Apache-2.0
#include "srvController.h"
#include "thread.hpp"
#include "myLeds.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/zbus/zbus.h>
#include <errno.h>

LOG_MODULE_REGISTER(srvController, CONFIG_FIREFLY_LOG_LEVEL);

namespace
{
constexpr k_timeout_t kWait = K_MSEC(50);
class SrvController final : public firefly::Thread<CONFIG_FIREFLY_CONTROLLER_STACK_SIZE>
{
  public:
    SrvController() : Thread(5, "srvController")
    {
        k_sem_init(&processed_sem_, 0, 1);
    }

    int startController(const struct zbus_observer *subscriber)
    {
        if (subscriber == nullptr || subscriber->type != ZBUS_OBSERVER_MSG_SUBSCRIBER_TYPE)
        {
            return -EINVAL;
        }
        if (isRunning())
        {
            return -EALREADY;
        }
        const int error = myLeds_init();
        if (error != 0)
        {
            return error;
        }
        subscriber_ = subscriber;
        return start();
    }

    int lastEvent(struct event_msg *event)
    {
        if (event == nullptr)
        {
            return -EINVAL;
        }
        k_spinlock_key_t key = k_spin_lock(&state_lock_);
        if (!has_event_)
        {
            k_spin_unlock(&state_lock_, key);
            return -ENODATA;
        }
        *event = last_event_;
        k_spin_unlock(&state_lock_, key);
        return 0;
    }

    uint32_t count() const
    {
        return static_cast<uint32_t>(atomic_get(&processed_));
    }

    int waitProcessed(uint32_t previous, int32_t timeout_ms)
    {
        if (timeout_ms < 0)
        {
            return -EINVAL;
        }
        while (count() <= previous)
        {
            const int error = k_sem_take(&processed_sem_, K_MSEC(timeout_ms));
            if (error != 0)
            {
                return error;
            }
        }
        return 0;
    }

#ifdef CONFIG_FIREFLY_TEST_HOOKS
    int reset()
    {
        if (isRunning())
        {
            return -EBUSY;
        }
        subscriber_ = nullptr;
        myLeds_reset();
        atomic_clear(&processed_);
        k_sem_reset(&processed_sem_);
        k_spinlock_key_t key = k_spin_lock(&state_lock_);
        has_event_ = false;
        last_event_ = {};
        k_spin_unlock(&state_lock_, key);
        return 0;
    }
#endif

  protected:
    void run() override
    {
        LOG_INF("srvController ready");
        while (!shouldStop())
        {
            const struct zbus_channel *channel = nullptr;
            struct bus_msg message{};
            const int error = zbus_sub_wait_msg(subscriber_, &channel, &message, kWait);
            if (error == -ENOMSG || error == -EAGAIN)
            {
                continue;
            }
            if (error != 0)
            {
                LOG_ERR("message wait failed (%d)", error);
                continue;
            }
            if (channel != &bus_chan)
            {
                LOG_WRN("unexpected channel");
                continue;
            }
            dispatch(message);
        }
    }

  private:
    void dispatch(const struct bus_msg &message)
    {
        int error = 0;
        if (message.kind == BUS_MSG_EVENT && bus_event_is_valid(&message.data.event))
        {
            k_spinlock_key_t key = k_spin_lock(&state_lock_);
            last_event_ = message.data.event;
            has_event_ = true;
            k_spin_unlock(&state_lock_, key);
            LOG_INF("event %u", static_cast<unsigned>(message.data.event.evt));
        }
        else if (message.kind == BUS_MSG_BUTTON && bus_button_is_valid(&message.data.button))
        {
            error = myLeds_set(message.data.button.button, message.data.button.pressed);
            if (error != 0)
            {
                LOG_ERR("LED set failed (%d)", error);
            }
        }
        else
        {
            LOG_WRN("invalid message kind/payload");
            return;
        }
        atomic_inc(&processed_);
        k_sem_give(&processed_sem_);
    }

    const struct zbus_observer *subscriber_ = nullptr;
    struct k_spinlock state_lock_{};
    struct k_sem processed_sem_{};
    struct event_msg last_event_{};
    bool has_event_ = false;
    atomic_t processed_{0};
};

SrvController g_controller;
} // namespace

extern "C" int srvController_start(const struct zbus_observer *subscriber)
{
    return g_controller.startController(subscriber);
}
extern "C" int srvController_stop(void)
{
    return g_controller.stop();
}
extern "C" bool srvController_is_running(void)
{
    return g_controller.isRunning();
}
extern "C" int srvController_last_event(struct event_msg *event)
{
    return g_controller.lastEvent(event);
}
extern "C" uint32_t srvController_processed_count(void)
{
    return g_controller.count();
}
extern "C" int srvController_wait_processed(uint32_t count, int32_t timeout_ms)
{
    return g_controller.waitProcessed(count, timeout_ms);
}
#ifdef CONFIG_FIREFLY_TEST_HOOKS
extern "C" int srvController_reset(void)
{
    return g_controller.reset();
}
#endif
