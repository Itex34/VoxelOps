#include "ThreadPool.hpp"

ThreadPool::ThreadPool(size_t threadCount) {
    if (threadCount == 0) threadCount = 1;

    for (size_t i = 0; i < threadCount; i++) {
        workers.emplace_back([this] { workerLoop(); });
    }
}

ThreadPool::~ThreadPool() {
    stop = true;
    cv.notify_all();

    for (auto& t : workers) {
        if (t.joinable()) t.join();
    }
}

void ThreadPool::enqueue(const std::function<void()>& job) {
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        jobs.push(job);
    }
    cv.notify_one();
}

void ThreadPool::workerLoop() {
    while (true) {
        std::function<void()> job;

        {
            std::unique_lock<std::mutex> lock(queueMutex);

            cv.wait(lock, [&] { return stop || !jobs.empty(); });

            if (stop && jobs.empty()) return;

            job = std::move(jobs.front());
            jobs.pop();
        }

        job();
    }
}
