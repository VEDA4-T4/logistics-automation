#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "logistics/contracts/uart/linetracer_commands.h"
#include "logistics/contracts/uart_codec.h"
#include "logistics/contracts/uart_parser.h"

#if defined(__linux__)
#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>

#include <cerrno>
#endif

namespace {

using Clock = std::chrono::steady_clock;

const char* CommandName(std::uint8_t command) {
    switch (command) {
        case UART_CMD_PING:
            return "PING";
        case UART_CMD_LINETRACER_ASSIGN_ROUTE:
            return "ASSIGN_ROUTE";
        case UART_CMD_LINETRACER_STOP_DRIVE:
            return "STOP_DRIVE";
        case UART_CMD_LINETRACER_STATUS_REQUEST:
            return "STATUS_REQUEST";
        case UART_CMD_LINETRACER_RESET_SYSTEM:
            return "RESET_SYSTEM";
        case UART_CMD_LINETRACER_SET_CURRENT_POSITION:
            return "SET_CURRENT_POSITION";
        case UART_CMD_LINETRACER_RESUME_DRIVE:
            return "RESUME_DRIVE";
        case UART_CMD_EMERGENCY_STOP:
            return "EMERGENCY_STOP";
        case UART_CMD_RESPONSE:
            return "RESPONSE";
        case UART_CMD_ACK:
            return "ACK";
        case UART_CMD_EVENT:
            return "EVENT";
        default:
            return "UNKNOWN";
    }
}

const char* StatusName(std::uint8_t status) {
    switch (status) {
        case UART_STATUS_ACK:
            return "ACK";
        case UART_STATUS_NACK:
            return "NACK";
        case UART_STATUS_BUSY:
            return "BUSY";
        case UART_STATUS_SUCCESS:
            return "SUCCESS";
        case UART_STATUS_ERROR:
            return "ERROR";
        default:
            return "UNKNOWN";
    }
}

const char* ErrorName(std::uint8_t error) {
    switch (error) {
        case UART_ERROR_NONE:
            return "NONE";
        case UART_ERROR_INVALID_VERSION:
            return "INVALID_VERSION";
        case UART_ERROR_INVALID_COMMAND:
            return "INVALID_COMMAND";
        case UART_ERROR_INVALID_LENGTH:
            return "INVALID_LENGTH";
        case UART_ERROR_CRC_MISMATCH:
            return "CRC_MISMATCH";
        case UART_ERROR_SEQUENCE:
            return "SEQUENCE";
        case UART_ERROR_TIMEOUT:
            return "TIMEOUT";
        case UART_ERROR_BUSY:
            return "BUSY";
        case UART_ERROR_SENSOR:
            return "SENSOR";
        case UART_ERROR_MOTOR:
            return "MOTOR";
        case UART_ERROR_SERVO:
            return "SERVO";
        case UART_ERROR_EMERGENCY_STOP:
            return "EMERGENCY_STOP";
        case UART_ERROR_UNSUPPORTED_COMMAND:
            return "UNSUPPORTED_COMMAND";
        case UART_ERROR_INVALID_PAYLOAD:
            return "INVALID_PAYLOAD";
        case UART_ERROR_INTERNAL:
            return "INTERNAL";
        default:
            return "UNKNOWN";
    }
}

const char* StateName(std::uint8_t state) {
    switch (state) {
        case UART_LINETRACER_STATE_IDLE:
            return "IDLE";
        case UART_LINETRACER_STATE_LOAD_WAIT:
            return "LOAD_WAIT";
        case UART_LINETRACER_STATE_FOLLOWING_LINE:
            return "FOLLOWING_LINE";
        case UART_LINETRACER_STATE_CORRECTING:
            return "CORRECTING";
        case UART_LINETRACER_STATE_ARRIVED:
            return "ARRIVED";
        case UART_LINETRACER_STATE_UNLOADING:
            return "UNLOADING";
        case UART_LINETRACER_STATE_STOPPED:
            return "STOPPED";
        case UART_LINETRACER_STATE_FAULT:
            return "FAULT";
        case UART_LINETRACER_STATE_EMERGENCY_STOP:
            return "EMERGENCY_STOP";
        default:
            return "UNKNOWN";
    }
}

const char* EventName(std::uint8_t event_id) {
    switch (event_id) {
        case UART_LINETRACER_EVENT_ARRIVED:
            return "ARRIVED";
        case UART_LINETRACER_EVENT_LOAD_DETECTED:
            return "LOAD_DETECTED";
        case UART_LINETRACER_EVENT_UNLOAD_COMPLETE:
            return "UNLOAD_COMPLETE";
        case UART_LINETRACER_EVENT_STATE_CHANGED:
            return "STATE_CHANGED";
        case UART_LINETRACER_EVENT_FAULT:
            return "FAULT";
        case UART_LINETRACER_EVENT_STARTED:
            return "STARTED";
        case UART_LINETRACER_EVENT_HEARTBEAT:
            return "HEARTBEAT";
        default:
            return "UNKNOWN";
    }
}

void PrintHexByte(std::uint8_t value) {
    std::cout << "0x" << std::hex << std::uppercase << std::setw(2) << std::setfill('0')
              << static_cast<unsigned int>(value) << std::dec << std::nouppercase << std::setfill(' ');
}

void PrintRawFrame(const uart_frame_t& frame) {
    std::array<std::uint8_t, UART_MAX_FRAME_SIZE> encoded{};
    std::size_t length = 0U;

    if (uart_encode_frame(&frame, encoded.data(), encoded.size(), &length) != UART_CODEC_OK) {
        return;
    }

    std::cout << " raw=";
    for (std::size_t index = 0U; index < length; ++index) {
        if (index != 0U) {
            std::cout << ' ';
        }
        std::cout << std::hex << std::uppercase << std::setw(2) << std::setfill('0')
                  << static_cast<unsigned int>(encoded[index]);
    }
    std::cout << std::dec << std::nouppercase << std::setfill(' ');
}

void PrintJobRoute(const std::uint8_t* payload) {
    std::cout << " job=" << uart_linetracer_event_job_id(payload)
              << " route=" << static_cast<unsigned int>(payload[UART_LINETRACER_EVENT_ROUTE_ID_INDEX]);
}

[[maybe_unused]] void PrintReceivedFrame(const uart_frame_t& frame, std::optional<double> rtt_ms) {
    std::cout << "[RX] seq=" << static_cast<unsigned int>(frame.sequence) << " command=" << CommandName(frame.command)
              << '(';
    PrintHexByte(frame.command);
    std::cout << ')';

    if (frame.command == UART_CMD_ACK && frame.length == UART_ACK_PAYLOAD_SIZE) {
        std::cout << " status=" << StatusName(frame.payload[UART_ACK_STATUS_INDEX])
                  << " original=" << CommandName(frame.payload[UART_ACK_COMMAND_INDEX])
                  << " payload_length=" << static_cast<unsigned int>(frame.payload[UART_ACK_LENGTH_INDEX]);
    } else if (frame.command == UART_CMD_RESPONSE && frame.length == UART_LINETRACER_STATUS_PAYLOAD_SIZE) {
        std::cout << " status=" << StatusName(frame.payload[UART_RESPONSE_STATUS_INDEX])
                  << " original=" << CommandName(frame.payload[UART_RESPONSE_COMMAND_INDEX])
                  << " error=" << ErrorName(frame.payload[UART_RESPONSE_ERROR_INDEX])
                  << " state=" << StateName(frame.payload[UART_LINETRACER_STATUS_STATE_INDEX]) << " job="
                  << uart_linetracer_read_job_id(frame.payload, UART_LINETRACER_STATUS_JOB_ID_LOW_INDEX,
                                                 UART_LINETRACER_STATUS_JOB_ID_HIGH_INDEX)
                  << " route=" << static_cast<unsigned int>(frame.payload[UART_LINETRACER_STATUS_ROUTE_ID_INDEX])
                  << " load=" << static_cast<unsigned int>(frame.payload[UART_LINETRACER_STATUS_LOAD_STATE_INDEX]);
    } else if (frame.command == UART_CMD_EVENT && frame.length >= UART_EVENT_HEADER_SIZE) {
        const std::uint8_t event_id = frame.payload[UART_EVENT_ID_INDEX];
        std::cout << " event=" << EventName(event_id);

        if (event_id == UART_LINETRACER_EVENT_HEARTBEAT && frame.length == UART_LINETRACER_HEARTBEAT_PAYLOAD_SIZE) {
            std::cout << " state=" << StateName(frame.payload[UART_LINETRACER_HEARTBEAT_STATE_INDEX])
                      << " error=" << ErrorName(frame.payload[UART_LINETRACER_HEARTBEAT_ERROR_INDEX]) << " flags=";
            PrintHexByte(frame.payload[UART_LINETRACER_HEARTBEAT_FLAGS_INDEX]);
            std::cout << " load="
                      << static_cast<unsigned int>(frame.payload[UART_LINETRACER_HEARTBEAT_LOAD_STATE_INDEX])
                      << " uptime_ms=" << uart_linetracer_heartbeat_uptime_ms(frame.payload) << " job="
                      << uart_linetracer_read_job_id(frame.payload, UART_LINETRACER_HEARTBEAT_JOB_ID_LOW_INDEX,
                                                     UART_LINETRACER_HEARTBEAT_JOB_ID_HIGH_INDEX)
                      << " route="
                      << static_cast<unsigned int>(frame.payload[UART_LINETRACER_HEARTBEAT_ROUTE_ID_INDEX]);
        } else if ((event_id == UART_LINETRACER_EVENT_STATE_CHANGED) &&
                   frame.length == UART_LINETRACER_STATE_EVENT_PAYLOAD_SIZE) {
            PrintJobRoute(frame.payload);
            std::cout << " state=" << StateName(frame.payload[UART_LINETRACER_STATE_EVENT_STATE_INDEX]);
        } else if ((event_id == UART_LINETRACER_EVENT_FAULT) &&
                   frame.length == UART_LINETRACER_FAULT_EVENT_PAYLOAD_SIZE) {
            PrintJobRoute(frame.payload);
            std::cout << " error=" << ErrorName(frame.payload[UART_LINETRACER_FAULT_EVENT_ERROR_INDEX]);
        } else if (frame.length == UART_LINETRACER_JOB_EVENT_PAYLOAD_SIZE) {
            PrintJobRoute(frame.payload);
        }
    }

    if (rtt_ms.has_value()) {
        std::cout << " rtt_ms=" << std::fixed << std::setprecision(3) << *rtt_ms << std::defaultfloat;
    }
    PrintRawFrame(frame);
    std::cout << '\n';
}

[[maybe_unused]] std::optional<std::uint32_t> ParseUnsigned(std::string_view text, std::uint32_t maximum) {
    std::uint32_t value = 0U;
    int base = 10;

    if (text.size() > 2U && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        text.remove_prefix(2U);
        base = 16;
    }
    if (text.empty()) {
        return std::nullopt;
    }

    const auto result = std::from_chars(text.data(), text.data() + text.size(), value, base);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || value > maximum) {
        return std::nullopt;
    }
    return value;
}

[[maybe_unused]] void PrintHelp() {
    std::cout << "Commands:\n"
              << "  ping                         Send a link test\n"
              << "  status                       Request current STM32 state\n"
              << "  position <1..3>              Set current destination position\n"
              << "  route <job 1..65535> <1..3> Assign a route\n"
              << "  stop <job 1..65535>          Request a normal stop\n"
              << "  reset                        Request safety reset\n"
              << "  resume                       Request safety-approved resume\n"
              << "  estop                        Send emergency stop\n"
              << "  keepalive on|off             Send PING every second while testing\n"
              << "  help                         Show this help\n"
              << "  quit                         Exit\n";
}

int RunSelfTest() {
    struct TestCase {
        std::uint8_t sequence;
        std::uint8_t command;
        std::vector<std::uint8_t> payload;
        std::vector<std::uint8_t> expected;
    };

    const std::array<TestCase, 3U> cases{ {
        { 0x10U, UART_CMD_PING, {}, { 0xAAU, 0x01U, 0x10U, 0x01U, 0x00U, 0x26U, 0x82U } },
        { 0x12U, UART_CMD_LINETRACER_RESUME_DRIVE, {}, { 0xAAU, 0x01U, 0x12U, 0x45U, 0x00U, 0x4EU, 0x2DU } },
        { 0x17U,
          UART_CMD_LINETRACER_STOP_DRIVE,
          { 0x34U, 0x12U },
          { 0xAAU, 0x01U, 0x17U, 0x41U, 0x02U, 0x34U, 0x12U, 0x54U, 0xA5U } },
    } };

    for (const TestCase& test : cases) {
        uart_frame_t frame{};
        std::array<std::uint8_t, UART_MAX_FRAME_SIZE> encoded{};
        std::size_t encoded_length = 0U;
        uart_parser_t parser{};
        uart_frame_t decoded{};
        uart_parser_result_t parser_result = UART_PARSER_NO_FRAME;

        frame.version = UART_PROTOCOL_VERSION;
        frame.sequence = test.sequence;
        frame.command = test.command;
        frame.length = static_cast<std::uint8_t>(test.payload.size());
        if (!test.payload.empty()) {
            std::memcpy(frame.payload, test.payload.data(), test.payload.size());
        }

        if (uart_encode_frame(&frame, encoded.data(), encoded.size(), &encoded_length) != UART_CODEC_OK ||
            encoded_length != test.expected.size() ||
            !std::equal(test.expected.begin(), test.expected.end(), encoded.begin())) {
            std::cerr << "Self-test encode failure for " << CommandName(test.command) << '\n';
            return 1;
        }

        uart_parser_init(&parser);
        for (std::size_t index = 0U; index < encoded_length; ++index) {
            parser_result = uart_parser_feed(&parser, encoded[index], &decoded);
        }
        if (parser_result != UART_PARSER_FRAME_READY || decoded.sequence != test.sequence ||
            decoded.command != test.command || decoded.length != test.payload.size() ||
            !std::equal(test.payload.begin(), test.payload.end(), decoded.payload)) {
            std::cerr << "Self-test parser failure for " << CommandName(test.command) << '\n';
            return 1;
        }
    }

    std::cout << "UART codec/parser self-test passed (" << cases.size() << " frames)\n";
    return 0;
}

#if defined(__linux__)

class SerialPort final {
public:
    explicit SerialPort(const std::string& device) {
        descriptor_ = ::open(device.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (descriptor_ < 0) {
            error_ = "open(" + device + "): " + std::strerror(errno);
            return;
        }

        termios settings{};
        if (tcgetattr(descriptor_, &settings) != 0) {
            error_ = "tcgetattr: " + std::string(std::strerror(errno));
            Close();
            return;
        }

        cfsetispeed(&settings, B115200);
        cfsetospeed(&settings, B115200);
        settings.c_iflag &= static_cast<tcflag_t>(~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL));
        settings.c_oflag &= static_cast<tcflag_t>(~OPOST);
        settings.c_lflag &= static_cast<tcflag_t>(~(ECHO | ECHONL | ICANON | ISIG | IEXTEN));
        settings.c_cflag &= static_cast<tcflag_t>(~(CSIZE | PARENB | CSTOPB));
#if defined(CRTSCTS)
        settings.c_cflag &= static_cast<tcflag_t>(~CRTSCTS);
#endif
        settings.c_cflag |= static_cast<tcflag_t>(CS8 | CLOCAL | CREAD);
        settings.c_iflag &= static_cast<tcflag_t>(~(IXON | IXOFF | IXANY));
        settings.c_cc[VMIN] = 0;
        settings.c_cc[VTIME] = 0;

        if (tcsetattr(descriptor_, TCSANOW, &settings) != 0) {
            error_ = "tcsetattr: " + std::string(std::strerror(errno));
            Close();
            return;
        }
        (void)tcflush(descriptor_, TCIOFLUSH);
    }

    ~SerialPort() {
        Close();
    }

    SerialPort(const SerialPort&) = delete;
    SerialPort& operator=(const SerialPort&) = delete;

    bool IsOpen() const {
        return descriptor_ >= 0;
    }

    int Descriptor() const {
        return descriptor_;
    }

    const std::string& Error() const {
        return error_;
    }

    bool WriteAll(const std::uint8_t* data, std::size_t length) {
        std::size_t offset = 0U;

        while (offset < length) {
            const ssize_t written = ::write(descriptor_, data + offset, length - offset);
            if (written > 0) {
                offset += static_cast<std::size_t>(written);
                continue;
            }
            if (written < 0 && errno == EINTR) {
                continue;
            }
            if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                pollfd output{ descriptor_, POLLOUT, 0 };
                if (::poll(&output, 1, 100) > 0) {
                    continue;
                }
            }
            error_ = "write: " + std::string(std::strerror(errno));
            return false;
        }
        return true;
    }

private:
    void Close() {
        if (descriptor_ >= 0) {
            (void)::close(descriptor_);
            descriptor_ = -1;
        }
    }

    int descriptor_{ -1 };
    std::string error_;
};

struct PendingRequest {
    Clock::time_point sent_at{};
    std::uint8_t command{};
};

class UartTestApp final {
public:
    explicit UartTestApp(const std::string& device)
        : serial_(device),
          next_sequence_(static_cast<std::uint8_t>(
              std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count())) {
        uart_parser_init(&parser_);
    }

    int Run() {
        if (!serial_.IsOpen()) {
            std::cerr << "Serial initialization failed: " << serial_.Error() << '\n';
            return 1;
        }

        std::cout << "Opened UART at 115200 8N1. Incoming heartbeat/events are shown automatically.\n";
        PrintHelp();
        std::cout << "uart128> " << std::flush;

        auto last_parser_tick = Clock::now();
        next_keepalive_ = last_parser_tick + kKeepalivePeriod;

        while (running_) {
            const std::array<pollfd, 2U> descriptors{ {
                { serial_.Descriptor(), POLLIN, 0 },
                { STDIN_FILENO, POLLIN, 0 },
            } };
            auto mutable_descriptors = descriptors;
            const int poll_result =
                ::poll(mutable_descriptors.data(), static_cast<nfds_t>(mutable_descriptors.size()), kPollPeriodMs);

            const auto now = Clock::now();
            const auto parser_elapsed =
                std::chrono::duration_cast<std::chrono::milliseconds>(now - last_parser_tick).count();
            if (parser_elapsed > 0) {
                const auto bounded_elapsed = static_cast<std::uint32_t>(
                    std::min<std::int64_t>(parser_elapsed, std::numeric_limits<std::uint32_t>::max()));
                const uart_parser_result_t result = uart_parser_tick(&parser_, bounded_elapsed);
                if (result == UART_PARSER_TIMEOUT) {
                    std::cerr << "[RX] partial frame timeout\n";
                }
                last_parser_tick = now;
            }

            if (poll_result < 0) {
                if (errno == EINTR) {
                    continue;
                }
                std::cerr << "poll failed: " << std::strerror(errno) << '\n';
                return 1;
            }

            if ((mutable_descriptors[0].revents & POLLIN) != 0) {
                ReadSerial();
            }
            if ((mutable_descriptors[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
                std::cerr << "Serial device disconnected or reported an error\n";
                return 1;
            }
            if ((mutable_descriptors[1].revents & POLLIN) != 0) {
                std::string line;
                if (!std::getline(std::cin, line)) {
                    running_ = false;
                } else {
                    HandleCommand(line);
                    if (running_) {
                        std::cout << "uart128> " << std::flush;
                    }
                }
            }

            if (keepalive_enabled_ && Clock::now() >= next_keepalive_) {
                (void)SendFrame(UART_CMD_PING, {}, false);
                next_keepalive_ = Clock::now() + kKeepalivePeriod;
            }
        }
        return 0;
    }

private:
    bool SendFrame(std::uint8_t command, const std::vector<std::uint8_t>& payload, bool show_transmit = true) {
        uart_frame_t frame{};
        std::array<std::uint8_t, UART_MAX_FRAME_SIZE> encoded{};
        std::size_t encoded_length = 0U;

        if (payload.size() > UART_MAX_PAYLOAD_SIZE) {
            std::cerr << "Payload is too large\n";
            return false;
        }

        frame.version = UART_PROTOCOL_VERSION;
        frame.sequence = next_sequence_;
        frame.command = command;
        frame.length = static_cast<std::uint8_t>(payload.size());
        if (!payload.empty()) {
            std::memcpy(frame.payload, payload.data(), payload.size());
        }

        const uart_codec_result_t encode_result =
            uart_encode_frame(&frame, encoded.data(), encoded.size(), &encoded_length);
        if (encode_result != UART_CODEC_OK) {
            std::cerr << "Frame encoding failed: " << static_cast<int>(encode_result) << '\n';
            return false;
        }
        if (!serial_.WriteAll(encoded.data(), encoded_length)) {
            std::cerr << "UART write failed: " << serial_.Error() << '\n';
            return false;
        }

        pending_[frame.sequence] = PendingRequest{ Clock::now(), command };
        ++next_sequence_;

        if (show_transmit) {
            std::cout << "[TX] seq=" << static_cast<unsigned int>(frame.sequence) << " command=" << CommandName(command)
                      << '(';
            PrintHexByte(command);
            std::cout << ')';
            PrintRawFrame(frame);
            std::cout << '\n';
        }
        return true;
    }

    void ReadSerial() {
        std::array<std::uint8_t, 256U> buffer{};

        for (;;) {
            const ssize_t received = ::read(serial_.Descriptor(), buffer.data(), buffer.size());
            if (received > 0) {
                for (ssize_t index = 0; index < received; ++index) {
                    uart_frame_t frame{};
                    const uart_parser_result_t result =
                        uart_parser_feed(&parser_, buffer[static_cast<std::size_t>(index)], &frame);
                    if (result == UART_PARSER_FRAME_READY) {
                        HandleFrame(frame);
                    } else if (result < UART_PARSER_NO_FRAME) {
                        std::cerr << "[RX] parser error=" << static_cast<int>(result) << '\n';
                    }
                }
                continue;
            }
            if (received == 0) {
                return;
            }
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return;
            }
            std::cerr << "UART read failed: " << std::strerror(errno) << '\n';
            running_ = false;
            return;
        }
    }

    void HandleFrame(const uart_frame_t& frame) {
        std::optional<double> rtt_ms;

        if ((frame.command == UART_CMD_ACK || frame.command == UART_CMD_RESPONSE) &&
            pending_[frame.sequence].has_value()) {
            rtt_ms =
                std::chrono::duration<double, std::milli>(Clock::now() - pending_[frame.sequence]->sent_at).count();
            pending_[frame.sequence].reset();
        }
        PrintReceivedFrame(frame, rtt_ms);
    }

    void HandleCommand(const std::string& line) {
        std::istringstream input(line);
        std::string command;
        input >> command;

        if (command.empty()) {
            return;
        }
        if (command == "help") {
            PrintHelp();
        } else if (command == "quit" || command == "exit") {
            running_ = false;
        } else if (command == "ping") {
            (void)SendFrame(UART_CMD_PING, {});
        } else if (command == "status") {
            (void)SendFrame(UART_CMD_LINETRACER_STATUS_REQUEST, {});
        } else if (command == "reset") {
            (void)SendFrame(UART_CMD_LINETRACER_RESET_SYSTEM, {});
        } else if (command == "resume") {
            (void)SendFrame(UART_CMD_LINETRACER_RESUME_DRIVE, {});
        } else if (command == "estop") {
            (void)SendFrame(UART_CMD_EMERGENCY_STOP, {});
        } else if (command == "position") {
            HandlePosition(input);
        } else if (command == "route") {
            HandleRoute(input);
        } else if (command == "stop") {
            HandleStop(input);
        } else if (command == "keepalive") {
            HandleKeepalive(input);
        } else {
            std::cerr << "Unknown command. Type 'help'.\n";
        }
    }

    void HandlePosition(std::istringstream& input) {
        std::string position_text;
        input >> position_text;
        const auto position = ParseUnsigned(position_text, UART_LINETRACER_POSITION_MAX);

        if (!position.has_value() || *position < UART_LINETRACER_POSITION_MIN) {
            std::cerr << "Usage: position <1..3>\n";
            return;
        }
        (void)SendFrame(UART_CMD_LINETRACER_SET_CURRENT_POSITION, { static_cast<std::uint8_t>(*position) });
    }

    void HandleRoute(std::istringstream& input) {
        std::string job_text;
        std::string route_text;
        input >> job_text >> route_text;
        const auto job = ParseUnsigned(job_text, UART_LINETRACER_JOB_ID_MAX);
        const auto route = ParseUnsigned(route_text, UART_LINETRACER_ROUTE_MAX);

        if (!job.has_value() || *job < UART_LINETRACER_JOB_ID_MIN || !route.has_value() ||
            *route < UART_LINETRACER_ROUTE_MIN) {
            std::cerr << "Usage: route <job 1..65535> <route 1..3>\n";
            return;
        }

        const std::vector<std::uint8_t> payload{
            static_cast<std::uint8_t>(*job & 0xFFU),
            static_cast<std::uint8_t>((*job >> 8U) & 0xFFU),
            static_cast<std::uint8_t>(*route),
        };
        (void)SendFrame(UART_CMD_LINETRACER_ASSIGN_ROUTE, payload);
    }

    void HandleStop(std::istringstream& input) {
        std::string job_text;
        input >> job_text;
        const auto job = ParseUnsigned(job_text, UART_LINETRACER_JOB_ID_MAX);

        if (!job.has_value() || *job < UART_LINETRACER_JOB_ID_MIN) {
            std::cerr << "Usage: stop <job 1..65535>\n";
            return;
        }

        const std::vector<std::uint8_t> payload{
            static_cast<std::uint8_t>(*job & 0xFFU),
            static_cast<std::uint8_t>((*job >> 8U) & 0xFFU),
        };
        (void)SendFrame(UART_CMD_LINETRACER_STOP_DRIVE, payload);
    }

    void HandleKeepalive(std::istringstream& input) {
        std::string value;
        input >> value;
        if (value == "on") {
            keepalive_enabled_ = true;
            next_keepalive_ = Clock::now();
            std::cout << "Keepalive enabled (PING every 1 second)\n";
        } else if (value == "off") {
            keepalive_enabled_ = false;
            std::cout << "Keepalive disabled\n";
        } else {
            std::cerr << "Usage: keepalive on|off\n";
        }
    }

    static constexpr int kPollPeriodMs = 50;
    static constexpr auto kKeepalivePeriod = std::chrono::seconds(1);

    SerialPort serial_;
    uart_parser_t parser_{};
    std::array<std::optional<PendingRequest>, 256U> pending_{};
    Clock::time_point next_keepalive_{};
    std::uint8_t next_sequence_{};
    bool keepalive_enabled_{ false };
    bool running_{ true };
};

#endif

}  // namespace

int main(int argc, char* argv[]) {
    if (argc >= 2 && std::string_view(argv[1]) == "--self-test") {
        return RunSelfTest();
    }

#if defined(__linux__)
    const std::string device = (argc >= 2) ? argv[1] : "/dev/serial0";
    return UartTestApp(device).Run();
#else
    (void)argc;
    (void)argv;
    std::cerr << "logistics_linetracer_uart_test requires Linux termios support.\n";
    return 2;
#endif
}
