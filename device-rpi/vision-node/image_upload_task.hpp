#pragma once

#include <chrono>
#include <exception>
#include <future>
#include <optional>
#include <stop_token>
#include <thread>
#include <utility>

namespace logistics::vision {

template <typename Completion>
class ImageUploadTask final {
public:
    ImageUploadTask() = default;
    ImageUploadTask(const ImageUploadTask&) = delete;
    ImageUploadTask& operator=(const ImageUploadTask&) = delete;

    ~ImageUploadTask() {
        Cancel();
    }

    template <typename Operation>
    [[nodiscard]] bool Start(Operation operation) {
        if (Active()) {
            return false;
        }
        std::promise<Completion> promise;
        completion_.emplace(promise.get_future());
        cancellation_.emplace();
        const std::stop_token stop_token = cancellation_->get_token();
        std::thread([promise = std::move(promise), operation = std::move(operation), stop_token]() mutable {
            try {
                promise.set_value(operation(stop_token));
            } catch (...) {
                promise.set_exception(std::current_exception());
            }
        }).detach();
        return true;
    }

    void Cancel() noexcept {
        if (cancellation_.has_value()) {
            cancellation_->request_stop();
        }
        completion_.reset();
        cancellation_.reset();
    }

    [[nodiscard]] bool Active() const noexcept {
        return completion_.has_value();
    }

    [[nodiscard]] bool Ready() const {
        return completion_.has_value() && completion_->wait_for(std::chrono::seconds(0)) == std::future_status::ready;
    }

    [[nodiscard]] Completion Take() {
        Completion result = completion_->get();
        completion_.reset();
        cancellation_.reset();
        return result;
    }

private:
    std::optional<std::future<Completion>> completion_;
    std::optional<std::stop_source> cancellation_;
};

}  // namespace logistics::vision
