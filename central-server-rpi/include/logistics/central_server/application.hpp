#pragma once

namespace logistics::central_server {

class Application final {
public:
    [[nodiscard]] static int Run(int argc, char* argv[]);
};

}  // namespace logistics::central_server
