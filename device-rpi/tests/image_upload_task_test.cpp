#include "image_upload_task.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <memory>
#include <thread>

namespace {

using logistics::vision::ImageUploadTask;

void TestCancelDoesNotWaitForWorkerAndDropsItsCompletion() {
    auto release = std::make_shared<std::atomic_bool>(false);
    auto started = std::make_shared<std::atomic_bool>(false);
    auto finished = std::make_shared<std::atomic_bool>(false);
    auto worker_owner = std::make_shared<int>(1);
    std::weak_ptr<int> worker_lifetime = worker_owner;
    ImageUploadTask<int> task;
    assert(task.Start([release, started, finished, worker_owner](std::stop_token) {
        started->store(true, std::memory_order_release);
        while (!release->load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        finished->store(true, std::memory_order_release);
        return 1;
    }));
    worker_owner.reset();
    while (!started->load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    const auto before_cancel = std::chrono::steady_clock::now();
    task.Cancel();
    const auto cancel_duration = std::chrono::steady_clock::now() - before_cancel;

    assert(cancel_duration < std::chrono::milliseconds(100));
    assert(!task.Active());
    assert(!worker_lifetime.expired());
    assert(task.Start([](std::stop_token) { return 2; }));
    while (!task.Ready()) {
        std::this_thread::yield();
    }
    assert(task.Take() == 2);
    release->store(true, std::memory_order_release);
    while (!finished->load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    while (!worker_lifetime.expired()) {
        std::this_thread::yield();
    }
    assert(!task.Active());
}

void TestDestructionDoesNotWaitForWorker() {
    auto release = std::make_shared<std::atomic_bool>(false);
    auto started = std::make_shared<std::atomic_bool>(false);
    auto finished = std::make_shared<std::atomic_bool>(false);
    const auto before_scope = std::chrono::steady_clock::now();
    {
        ImageUploadTask<int> task;
        assert(task.Start([release, started, finished](std::stop_token) {
            started->store(true, std::memory_order_release);
            while (!release->load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            finished->store(true, std::memory_order_release);
            return 1;
        }));
        while (!started->load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    }
    const auto scope_duration = std::chrono::steady_clock::now() - before_scope;

    assert(scope_duration < std::chrono::milliseconds(100));
    release->store(true, std::memory_order_release);
    while (!finished->load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
}

}  // namespace

int main() {
    TestCancelDoesNotWaitForWorkerAndDropsItsCompletion();
    TestDestructionDoesNotWaitForWorker();
    return 0;
}
