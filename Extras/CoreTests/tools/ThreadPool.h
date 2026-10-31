// ThreadPool.h — einfacher ParallelFor für Tests und Python (in Unreal übernimmt das ParallelFor)
#pragma once
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

class ThreadPool {
public:
    explicit ThreadPool(unsigned threads = std::max(1u, std::thread::hardware_concurrency())) {
        for (unsigned t = 0; t < threads; ++t)
            workers_.emplace_back([this] { workerLoop(); });
    }
    ~ThreadPool() {
        { std::lock_guard<std::mutex> lk(m_); quit_ = true; ++generation_; }
        cv_.notify_all();
        for (auto& w : workers_) w.join();
    }
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // Führt body(0..count-1) parallel aus und kehrt erst zurück, wenn alle fertig sind
    void parallelFor(int count, const std::function<void(int)>& body) {
        if (count <= 0) return;
        {
            std::lock_guard<std::mutex> lk(m_);
            body_ = &body; count_ = count; next_ = 0; busy_ = (int)workers_.size(); ++generation_;
        }
        cv_.notify_all();
        std::unique_lock<std::mutex> lk(m_);
        done_.wait(lk, [this] { return busy_ == 0; });
        body_ = nullptr;
    }

    // Passt direkt auf FDTDGrid::ParallelFor / fdtd::ParallelFor
    std::function<void(int, const std::function<void(int)>&)> asParallelFor() {
        return [this](int n, const std::function<void(int)>& f) { parallelFor(n, f); };
    }

private:
    void workerLoop() {
        unsigned long long seen = 0;
        for (;;) {
            const std::function<void(int)>* body;
            {
                std::unique_lock<std::mutex> lk(m_);
                cv_.wait(lk, [&] { return generation_ != seen; });
                seen = generation_;
                if (quit_) return;
                body = body_;
            }
            for (int k; (k = next_.fetch_add(1)) < count_;) (*body)(k);
            {
                std::lock_guard<std::mutex> lk(m_);
                if (--busy_ == 0) done_.notify_one();
            }
        }
    }

    std::vector<std::thread> workers_;
    std::mutex m_;
    std::condition_variable cv_, done_;
    const std::function<void(int)>* body_ = nullptr;
    std::atomic<int> next_{ 0 };
    int count_ = 0, busy_ = 0;
    unsigned long long generation_ = 0;
    bool quit_ = false;
};
