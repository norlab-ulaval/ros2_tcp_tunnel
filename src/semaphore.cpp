#include "semaphore.h"
#include <rclcpp/rclcpp.hpp>

Semaphore::Semaphore(const int& initialCount):
        mutex_(), condition_(), count_(initialCount)
{
}

bool Semaphore::acquire()
{
    std::unique_lock<decltype(mutex_)> lock(mutex_);
    while(rclcpp::ok() && !count_)
    {
        condition_.wait(lock);
    }
    --count_;
    return rclcpp::ok();
}

bool Semaphore::tryAcquire()
{
    std::lock_guard<decltype(mutex_)> lock(mutex_);
    if(count_)
    {
        --count_;
        return true;
    }
    return false;
}

void Semaphore::release()
{
    std::lock_guard<decltype(mutex_)> lock(mutex_);
    ++count_;
    condition_.notify_one();
}

void Semaphore::wakeUp()
{
    condition_.notify_all();
}
