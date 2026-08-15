#include "image_upload_task.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <memory>
#include <string_view>
#include <thread>

namespace {

using logistics::vision::ImageUploadRetryState;
using logistics::vision::ImageUploadTask;

constexpr std::string_view kWorkId = "22a194c3-3e3c-410c-a329-7e8c4ebcac83";

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

void TestUploadIntentSurvivesResultRetryUntilScheduled() {
    ImageUploadRetryState<int> retry;
    assert(retry.Retain(std::string(kWorkId), 42));
    assert(retry.PendingWorkId() == kWorkId);
    assert(retry.PendingIntent() != nullptr && *retry.PendingIntent() == 42);
    assert(!retry.ReadyToSchedule(true, false));
    assert(retry.PendingWorkId() == kWorkId);
    assert(!retry.Retain("7026045c-92ba-4fd9-93dc-6dfa04a5fd30", 7));

    assert(!retry.ReadyToSchedule(false, true));
    assert(retry.ReadyToSchedule(false, false));
    assert(!retry.MarkScheduled("7026045c-92ba-4fd9-93dc-6dfa04a5fd30"));
    assert(retry.MarkScheduled(kWorkId));
    assert(!retry.PendingWorkId().has_value());
    assert(!retry.ReadyToSchedule(false, false));
    assert(!retry.MarkScheduled(kWorkId));
}

}  // namespace

int main() {
    TestCancelDoesNotWaitForWorkerAndDropsItsCompletion();
    TestDestructionDoesNotWaitForWorker();
    TestUploadIntentSurvivesResultRetryUntilScheduled();
    return 0;
}
