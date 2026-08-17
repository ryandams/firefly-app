// SPDX-License-Identifier: Apache-2.0
#ifndef FIREFLY_THREAD_HPP
#define FIREFLY_THREAD_HPP

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <errno.h>
#include <cstddef>

namespace firefly
{

enum class ThreadState : atomic_val_t
{
    stopped,
    starting,
    running,
    stopping
};

template <std::size_t STACK_SIZE> class Thread
{
  public:
    Thread(const Thread &) = delete;
    Thread &operator=(const Thread &) = delete;

    int start()
    {
        if (!atomic_cas(&state_, value(ThreadState::stopped), value(ThreadState::starting)))
        {
            return -EALREADY;
        }

        atomic_clear(&stop_requested_);
        k_sem_init(&started_, 0, 1);
        k_tid_t tid = k_thread_create(&thread_, stack_, K_KERNEL_STACK_SIZEOF(stack_), &Thread::entry, this, nullptr,
                                      nullptr, priority_, 0, K_NO_WAIT);
        if (tid == nullptr)
        {
            atomic_set(&state_, value(ThreadState::stopped));
            return -EIO;
        }

        const int name_error = k_thread_name_set(&thread_, name_);
        if (k_sem_take(&started_, K_SECONDS(1)) != 0)
        {
            atomic_set(&stop_requested_, 1);
            (void)k_thread_join(&thread_, K_SECONDS(1));
            atomic_set(&state_, value(ThreadState::stopped));
            return -ETIMEDOUT;
        }
        return name_error;
    }

    int stop()
    {
        atomic_val_t state = atomic_get(&state_);
        if (state == value(ThreadState::stopped))
        {
            return 0;
        }

        atomic_set(&stop_requested_, 1);
        while (state != value(ThreadState::stopping))
        {
            if (state == value(ThreadState::stopped))
            {
                return 0;
            }
            if (atomic_cas(&state_, state, value(ThreadState::stopping)))
            {
                break;
            }
            state = atomic_get(&state_);
        }

        onStopRequested();
        const int error = k_thread_join(&thread_, K_SECONDS(2));
        if (error == 0)
        {
            atomic_set(&state_, value(ThreadState::stopped));
        }
        return error;
    }

    bool isRunning() const
    {
        const atomic_val_t state = atomic_get(&state_);
        return state == value(ThreadState::starting) || state == value(ThreadState::running);
    }

    ThreadState state() const
    {
        return static_cast<ThreadState>(atomic_get(&state_));
    }

    int stackUnused(std::size_t *unused) const
    {
        if (unused == nullptr)
        {
            return -EINVAL;
        }
        return k_thread_stack_space_get(const_cast<struct k_thread *>(&thread_), unused);
    }

  protected:
    Thread(int priority, const char *name) : priority_(priority), name_(name)
    {
    }
    virtual ~Thread() = default;
    virtual void run() = 0;
    virtual void onStopRequested()
    {
    }
    bool shouldStop() const
    {
        return atomic_get(&stop_requested_) != 0;
    }

  private:
    static constexpr atomic_val_t value(ThreadState state)
    {
        return static_cast<atomic_val_t>(state);
    }

    static void entry(void *self, void *, void *)
    {
        Thread *thread = static_cast<Thread *>(self);
        (void)atomic_cas(&thread->state_, value(ThreadState::starting), value(ThreadState::running));
        k_sem_give(&thread->started_);
        thread->run();
        atomic_set(&thread->state_, value(ThreadState::stopped));
    }

    K_KERNEL_STACK_MEMBER(stack_, STACK_SIZE);
    struct k_thread thread_{};
    struct k_sem started_{};
    const int priority_;
    const char *const name_;
    atomic_t state_{value(ThreadState::stopped)};
    atomic_t stop_requested_{0};
};

} // namespace firefly
#endif
