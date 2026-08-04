#include "logistics/control_center/rtsp_stream_worker.hpp"

#include <QMetaObject>

namespace logistics::control_center {

RtspStreamWorker::RtspStreamWorker(int network_timeout_ms, qsizetype maximum_buffer_size_bytes)
    : stream_(new RtspH264Stream) {
    stream_->setNetworkTimeout(network_timeout_ms);
    stream_->setMaximumBufferSize(maximum_buffer_size_bytes);
    stream_->moveToThread(&thread_);
    QObject::connect(&thread_, &QThread::finished, stream_, &QObject::deleteLater);
    thread_.start();
}

RtspStreamWorker::~RtspStreamWorker() {
    stop();
    thread_.quit();
    thread_.wait();
    stream_ = nullptr;
}

RtspH264Stream* RtspStreamWorker::stream() const noexcept {
    return stream_;
}

void RtspStreamWorker::start(const QUrl& url) {
    auto* const stream = stream_;
    QMetaObject::invokeMethod(stream, [stream, url]() { stream->start(url); }, Qt::QueuedConnection);
}

void RtspStreamWorker::stop() {
    if (stream_ == nullptr || !thread_.isRunning()) {
        return;
    }
    if (QThread::currentThread() == &thread_) {
        stream_->stop();
        return;
    }
    QMetaObject::invokeMethod(stream_, [stream = stream_]() { stream->stop(); }, Qt::BlockingQueuedConnection);
}

}  // namespace logistics::control_center
