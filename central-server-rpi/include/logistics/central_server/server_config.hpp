#pragma once

#include <filesystem>
#include <string>

#include "logistics/central_server/database.hpp"
#include "logistics/central_server/http_upload_server.hpp"
#include "logistics/central_server/persistence.hpp"
#include "logistics/central_server/process_orchestrator.hpp"
#include "logistics/central_server/sensor_detection.hpp"

namespace logistics::central_server {

struct ServerConfig final {
    DatabaseConfig database;
    StorageConfig storage;
    HttpUploadServerConfig http;
    ProcessOrchestratorConfig process;
    SensorDetectionConfig sensor_detection;
    std::string qt_client_id{ "control-center" };
};

[[nodiscard]] ServerConfig LoadServerConfig(const std::filesystem::path& path);

}  // namespace logistics::central_server
