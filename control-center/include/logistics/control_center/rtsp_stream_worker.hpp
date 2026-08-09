#pragma once

#include <QThread>
#include <QUrl>

#include "logistics/control_center/rtsp_h264_stream.hpp"

namespace logistics::control_center {

class RtspStreamWorker final {
public:
    RtspStreamWorker(int network_timeout_ms, qsizetype maximum_buffer_size_bytes);
    ~RtspStreamWorker();

    RtspStreamWorker(const RtspStreamWorker&) = delete;
    RtspStreamWorker& operator=(const RtspStreamWorker&) = delete;
    RtspStreamWorker(RtspStreamWorker&&) = delete;
    RtspStreamWorker& operator=(RtspStreamWorker&&) = delete;

    [[nodiscard]] RtspH264Stream* stream() const noexcept;
    void start(const QUrl& url);
    void stop();

private:
    QThread thread_;
    RtspH264Stream* stream_{ nullptr };
};

}  // namespace logistics::control_center
