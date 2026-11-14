#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <string>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <sdmmc_cmd.h>

// Lifetime matches the board. All card I/O runs on a low-priority worker,
// because MCP callbacks themselves run on Application's main task.
class SpotpearSdCardReader {
public:
    void Start();

private:
    enum class Operation { Mount, List, Read };
    struct Job {
        uint32_t id = 0;
        Operation operation = Operation::Mount;
        std::string path;
        uint32_t offset = 0;
        size_t limit = 0;
        bool running = false;
        bool done = false;
        std::string response;
    };

    static constexpr size_t kJobCount = 4;
    static constexpr const char* kMountPoint = "/sdcard";
    std::array<Job, kJobCount> jobs_;
    SemaphoreHandle_t mutex_ = nullptr;
    QueueHandle_t queue_ = nullptr;
    TaskHandle_t worker_ = nullptr;
    sdmmc_card_t* card_ = nullptr;  // Only accessed by the worker.
    bool mounted_ = false;
    bool starting_ = true;
    uint64_t capacity_bytes_ = 0;
    std::string last_error_;
    uint32_t next_id_ = 1;

    std::string Status();
    std::string Enqueue(Operation operation, const std::string& path = "/", uint32_t offset = 0,
                        size_t limit = 0);
    std::string GetJob(uint32_t id);
    void Run();
    std::expected<std::string, std::string> Mount();
    std::expected<std::string, std::string> List(const Job& job);
    std::expected<std::string, std::string> Read(const Job& job);
};
