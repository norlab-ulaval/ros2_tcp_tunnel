#include <mutex>
#include <condition_variable>

// taken from https://stackoverflow.com/questions/4792449/c0x-has-no-semaphores-how-to-synchronize-threads
class Semaphore
{
public:
    explicit Semaphore(const int& initialCount);
    bool acquire();
    bool tryAcquire();
    void release();
    void wakeUp();

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    unsigned long count_ = 0;
};
