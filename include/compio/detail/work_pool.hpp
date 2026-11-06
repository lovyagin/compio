#ifndef COMPIO_DETAIL_WORK_POOL_HPP_
#define COMPIO_DETAIL_WORK_POOL_HPP_

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace compio::detail {

// Runs the iterations of a loop on several threads; the calling thread takes part.
// Workers start at the first loop that has more than one iteration.
class work_pool {
public:
    explicit work_pool(unsigned threads) : threads_(threads == 0 ? 1 : threads) {}

    ~work_pool() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        start_.notify_all();
        for (auto &worker : workers_) worker.join();
    }

    work_pool(const work_pool &) = delete;
    work_pool &operator=(const work_pool &) = delete;

    unsigned threads() const { return threads_; }

    void run(size_t count, const std::function<void(size_t)> &body) {
        if (threads_ == 1 || count < 2) {
            for (size_t i = 0; i < count; ++i) body(i);
            return;
        }
        std::lock_guard<std::mutex> serial(run_mutex_);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (workers_.empty()) {
                for (unsigned i = 1; i < threads_; ++i) workers_.emplace_back([this] { work(); });
            }
            body_ = &body;
            count_ = count;
            next_.store(0);
            busy_ = static_cast<unsigned>(workers_.size());
            ++generation_;
        }
        start_.notify_all();
        take(body, count);
        std::unique_lock<std::mutex> lock(mutex_);
        done_.wait(lock, [this] { return busy_ == 0; });
        body_ = nullptr;
    }

private:
    const unsigned threads_;
    std::vector<std::thread> workers_;
    std::mutex run_mutex_;
    std::mutex mutex_;
    std::condition_variable start_;
    std::condition_variable done_;
    const std::function<void(size_t)> *body_ = nullptr;
    size_t count_ = 0;
    std::atomic<size_t> next_{0};
    unsigned busy_ = 0;
    uint64_t generation_ = 0;
    bool stop_ = false;

    void take(const std::function<void(size_t)> &body, size_t count) {
        for (size_t i = next_.fetch_add(1); i < count; i = next_.fetch_add(1)) body(i);
    }

    void work() {
        uint64_t seen = 0;
        std::unique_lock<std::mutex> lock(mutex_);
        while (true) {
            start_.wait(lock, [&] { return stop_ || generation_ != seen; });
            if (stop_) return;
            seen = generation_;
            const auto *body = body_;
            const size_t count = count_;
            lock.unlock();
            take(*body, count);
            lock.lock();
            if (--busy_ == 0) done_.notify_one();
        }
    }
};

} // namespace compio::detail

#endif // COMPIO_DETAIL_WORK_POOL_HPP_
