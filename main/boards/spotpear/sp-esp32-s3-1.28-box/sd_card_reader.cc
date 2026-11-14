#include "sdkconfig.h"

#ifdef CONFIG_SPOTPEAR_SD_READER

#include "sd_card_reader.h"

#include <dirent.h>
#include <sys/stat.h>

#include <climits>
#include <utility>

#include <driver/gpio.h>
#include <driver/sdmmc_host.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_vfs_fat.h>
#include <mbedtls/base64.h>

#include "cjson_utils.h"
#include "mcp_server.h"
#include "sd_file_reader.h"

namespace {
constexpr const char* kTag = "SpotpearSD";
constexpr size_t kMaxResponseBytes = 16384;

std::string Json(CJsonUniquePtr root) {
    CJsonStringUniquePtr text(root ? cJSON_PrintUnformatted(root.get()) : nullptr);
    return text ? std::string(text.get()) : "{\"error\":\"out_of_memory\"}";
}

std::string Error(const std::string& message) {
    CJsonUniquePtr root(cJSON_CreateObject());
    if (root) {
        cJSON_AddStringToObject(root.get(), "error", message.c_str());
    }
    return Json(std::move(root));
}

std::string JobState(uint32_t id, const char* state) {
    CJsonUniquePtr root(cJSON_CreateObject());
    if (root) {
        cJSON_AddNumberToObject(root.get(), "job_id", id);
        cJSON_AddStringToObject(root.get(), "state", state);
        cJSON_AddStringToObject(root.get(), "poll_tool", "self.sd_card.get_job");
    }
    return Json(std::move(root));
}
}  // namespace

void SpotpearSdCardReader::Start() {
    if (mutex_) {
        return;
    }
    mutex_ = xSemaphoreCreateMutex();
    queue_ = xQueueCreate(kJobCount, sizeof(uint8_t));
    auto& mcp = McpServer::GetInstance();
    mcp.AddTool("self.sd_card.get_status", "Get SD reader status. Does not perform card I/O.",
                PropertyList(), [this](const PropertyList&) -> ReturnValue { return Status(); });
    mcp.AddTool("self.sd_card.mount",
                "Request mounting or retrying the SD card without formatting it. Returns a job_id; "
                "retrieve its result with self.sd_card.get_job.",
                PropertyList(),
                [this](const PropertyList&) -> ReturnValue { return Enqueue(Operation::Mount); });
    mcp.AddTool("self.sd_card.list",
                "List a directory on the SD card, in pages. Paths are relative to the card root. "
                "Returns a job_id; get the actual file list with self.sd_card.get_job.",
                PropertyList({Property("path", kPropertyTypeString, "/").SetMaxLength(192),
                              Property("offset", kPropertyTypeInteger, 0, 0, 4096),
                              Property("limit", kPropertyTypeInteger, 16, 1, 32)}),
                [this](const PropertyList& p) -> ReturnValue {
                    return Enqueue(Operation::List, p["path"].value<std::string>(),
                                   p["offset"].value<int>(), p["limit"].value<int>());
                });
    mcp.AddTool(
        "self.sd_card.read_file",
        "Read up to 4096 bytes of an SD file without changing it. Returns a job_id; use "
        "self.sd_card.get_job to obtain the data. Valid UTF-8 is returned as text, otherwise "
        "data_base64. Follow next_offset until eof. Maximum file size: 2 GiB minus one byte.",
        PropertyList({Property("path", kPropertyTypeString).SetMaxLength(192),
                      Property("offset", kPropertyTypeInteger, 0, 0, INT_MAX),
                      Property("length", kPropertyTypeInteger, 4096, 1, 4096)}),
        [this](const PropertyList& p) -> ReturnValue {
            return Enqueue(Operation::Read, p["path"].value<std::string>(),
                           p["offset"].value<int>(), p["length"].value<int>());
        });
    mcp.AddTool(
        "self.sd_card.get_job",
        "Retrieve an SD job result. A done or failed job is complete. "
        "At most four jobs/results are kept; older completed results may expire.",
        PropertyList({Property("job_id", kPropertyTypeInteger, 1, INT_MAX)}),
        [this](const PropertyList& p) -> ReturnValue { return GetJob(p["job_id"].value<int>()); });
    if (!mutex_ || !queue_ ||
        xTaskCreate([](void* p) { static_cast<SpotpearSdCardReader*>(p)->Run(); }, "sd_reader",
                    8192, this, 1, &worker_) != pdPASS) {
        starting_ = false;
        last_error_ = "SD worker allocation failed";
        ESP_LOGE(kTag, "%s", last_error_.c_str());
    }
}

std::string SpotpearSdCardReader::Status() {
    if (!mutex_ || xSemaphoreTake(mutex_, 0) != pdTRUE) {
        return Error("SD status busy or worker unavailable; retry later");
    }
    CJsonUniquePtr root(cJSON_CreateObject());
    if (root) {
        cJSON_AddBoolToObject(root.get(), "mounted", mounted_);
        cJSON_AddBoolToObject(root.get(), "mount_in_progress", starting_);
        cJSON_AddBoolToObject(root.get(), "reader_only", true);
        cJSON_AddBoolToObject(root.get(), "auto_format", false);
        cJSON_AddNumberToObject(root.get(), "capacity_bytes", static_cast<double>(capacity_bytes_));
        cJSON_AddStringToObject(root.get(), "last_error", last_error_.c_str());
        cJSON_AddStringToObject(root.get(), "interface", "SDMMC 1-bit");
    }
    xSemaphoreGive(mutex_);
    return Json(std::move(root));
}

std::string SpotpearSdCardReader::Enqueue(Operation operation, const std::string& path,
                                          uint32_t offset, size_t limit) {
    auto normalized = spotpear_sd::NormalizePath(path);
    if (!normalized) {
        return Error(normalized.error());
    }
    if (!worker_ || !mutex_ || xSemaphoreTake(mutex_, 0) != pdTRUE) {
        return Error("SD worker unavailable or busy; retry later");
    }
    int slot = -1;
    for (size_t i = 0; i < jobs_.size(); ++i) {
        if (jobs_[i].id == 0) {
            slot = static_cast<int>(i);
            break;
        }
    }
    if (slot < 0) {
        for (size_t i = 0; i < jobs_.size(); ++i) {
            if (jobs_[i].done && (slot < 0 || jobs_[i].id < jobs_[slot].id)) {
                slot = static_cast<int>(i);
            }
        }
    }
    if (slot < 0) {
        xSemaphoreGive(mutex_);
        return Error("SD queue full; wait for a pending job");
    }
    const uint32_t id = next_id_++;
    if (next_id_ > INT_MAX) {
        next_id_ = 1;
    }
    jobs_[slot] = Job{id, operation, *normalized, offset, limit};
    const auto index = static_cast<uint8_t>(slot);
    if (xQueueSend(queue_, &index, 0) != pdTRUE) {
        jobs_[slot] = Job{};
        xSemaphoreGive(mutex_);
        return Error("SD queue full; retry later");
    }
    xSemaphoreGive(mutex_);
    return JobState(id, "queued");
}

std::string SpotpearSdCardReader::GetJob(uint32_t id) {
    if (!mutex_ || xSemaphoreTake(mutex_, 0) != pdTRUE) {
        return Error("SD status busy or worker unavailable; retry later");
    }
    std::string result;
    for (const auto& job : jobs_) {
        if (job.id == id) {
            result = job.done ? job.response : JobState(id, job.running ? "running" : "queued");
            break;
        }
    }
    xSemaphoreGive(mutex_);
    return result.empty() ? Error("Unknown or expired SD job") : result;
}

std::expected<std::string, std::string> SpotpearSdCardReader::Mount() {
    xSemaphoreTake(mutex_, portMAX_DELAY);
    mounted_ = false;
    starting_ = true;
    capacity_bytes_ = 0;
    xSemaphoreGive(mutex_);
    if (card_) {
        const auto error = esp_vfs_fat_sdcard_unmount(kMountPoint, card_);
        if (error != ESP_OK) {
            xSemaphoreTake(mutex_, portMAX_DELAY);
            starting_ = false;
            last_error_ = "Cannot unmount previous card: " + std::string(esp_err_to_name(error));
            const auto message = last_error_;
            xSemaphoreGive(mutex_);
            return std::unexpected(message);
        }
        card_ = nullptr;
    }
    // Verified Spotpear pin map: CLK=17, CMD=18, D0=21, D3=13.
    // D3 must remain high in one-bit SD mode. The LCD uses a separate SPI bus.
    gpio_set_direction(GPIO_NUM_13, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_NUM_13, 1);
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_1;
    host.max_freq_khz = 8000;
    host.command_timeout_ms = 1000;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.clk = GPIO_NUM_17;
    slot.cmd = GPIO_NUM_18;
    slot.d0 = GPIO_NUM_21;
    slot.width = 1;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    esp_vfs_fat_sdmmc_mount_config_t config = {};
    config.format_if_mount_failed = false;
    config.max_files = 2;
    config.allocation_unit_size = 16 * 1024;
    config.disk_status_check_enable = true;
    const auto error = esp_vfs_fat_sdmmc_mount(kMountPoint, &host, &slot, &config, &card_);
    xSemaphoreTake(mutex_, portMAX_DELAY);
    mounted_ = error == ESP_OK;
    starting_ = false;
    last_error_ =
        mounted_ ? ""
                 : std::string("Mount failed; no formatting attempted: ") + esp_err_to_name(error);
    capacity_bytes_ = mounted_ ? uint64_t(card_->csd.capacity) * card_->csd.sector_size : 0;
    const auto message = last_error_;
    xSemaphoreGive(mutex_);
    if (error != ESP_OK) {
        card_ = nullptr;
        ESP_LOGW(kTag, "%s", message.c_str());
        return std::unexpected(message);
    }
    ESP_LOGI(kTag, "Mounted SD card for reader tools; no write or format tools registered");
    return Status();
}

std::expected<std::string, std::string> SpotpearSdCardReader::List(const Job& job) {
    const auto full_path = std::string(kMountPoint) + job.path;
    std::unique_ptr<DIR, decltype(&closedir)> dir(opendir(full_path.c_str()), &closedir);
    if (!dir) {
        return std::unexpected("Cannot open directory; path missing or card unavailable");
    }
    CJsonUniquePtr root(cJSON_CreateObject());
    if (!root) {
        return std::unexpected("Cannot allocate directory result");
    }
    auto* entries = cJSON_AddArrayToObject(root.get(), "entries");
    if (!entries) {
        return std::unexpected("Cannot allocate directory entries");
    }
    uint32_t scanned = 0;
    size_t included = 0;
    bool more = false;
    const auto deadline = esp_timer_get_time() + 1000000;
    while (auto* entry = readdir(dir.get())) {
        if (esp_timer_get_time() > deadline) {
            return std::unexpected(
                "Directory scan exceeded one-second budget; try a smaller page offset");
        }
        std::string name(entry->d_name);
        if (name == "." || name == "..") {
            continue;
        }
        if (scanned++ < job.offset) {
            continue;
        }
        if (included >= job.limit) {
            more = true;
            break;
        }
        if (!spotpear_sd::IsUtf8Text(name)) {
            return std::unexpected("Directory contains a non-UTF-8 filename");
        }
        auto* item = cJSON_CreateObject();
        if (!item) {
            return std::unexpected("Cannot allocate directory entry");
        }
        cJSON_AddItemToArray(entries, item);
        cJSON_AddStringToObject(item, "name", name.c_str());
        struct stat info = {};
        const auto child = full_path + (job.path == "/" ? "" : "/") + name;
        const bool accessible = stat(child.c_str(), &info) == 0;
        cJSON_AddBoolToObject(item, "stat_ok", accessible);
        if (accessible) {
            cJSON_AddBoolToObject(item, "directory", S_ISDIR(info.st_mode));
            cJSON_AddNumberToObject(item, "bytes", static_cast<double>(info.st_size));
        }
        ++included;
    }
    cJSON_AddStringToObject(root.get(), "path", job.path.c_str());
    cJSON_AddNumberToObject(root.get(), "next_offset", job.offset + included);
    cJSON_AddBoolToObject(root.get(), "has_more", more);
    return Json(std::move(root));
}

std::expected<std::string, std::string> SpotpearSdCardReader::Read(const Job& job) {
    auto chunk = spotpear_sd::ReadFile(kMountPoint, job.path, job.offset, job.limit);
    if (!chunk) {
        return std::unexpected(chunk.error());
    }
    CJsonUniquePtr root(cJSON_CreateObject());
    if (!root) {
        return std::unexpected("Cannot allocate file result");
    }
    cJSON_AddStringToObject(root.get(), "path", chunk->path.c_str());
    cJSON_AddNumberToObject(root.get(), "file_bytes", static_cast<double>(chunk->file_size));
    cJSON_AddNumberToObject(root.get(), "offset", chunk->offset);
    cJSON_AddNumberToObject(root.get(), "bytes_read", chunk->bytes.size());
    cJSON_AddNumberToObject(root.get(), "next_offset", chunk->offset + chunk->bytes.size());
    cJSON_AddBoolToObject(root.get(), "eof", chunk->eof);
    if (spotpear_sd::IsUtf8Text(chunk->bytes)) {
        cJSON_AddStringToObject(root.get(), "encoding", "utf-8");
        cJSON_AddStringToObject(root.get(), "text", chunk->bytes.c_str());
    } else {
        std::string encoded(4 * ((chunk->bytes.size() + 2) / 3) + 1, '\0');
        size_t length = 0;
        if (mbedtls_base64_encode(reinterpret_cast<unsigned char*>(encoded.data()), encoded.size(),
                                  &length,
                                  reinterpret_cast<const unsigned char*>(chunk->bytes.data()),
                                  chunk->bytes.size()) != 0) {
            return std::unexpected("Cannot encode binary chunk");
        }
        cJSON_AddStringToObject(root.get(), "encoding", "base64");
        cJSON_AddStringToObject(root.get(), "data_base64", encoded.c_str());
    }
    return Json(std::move(root));
}

void SpotpearSdCardReader::Run() {
    Mount();  // Optional card failure never aborts board startup.
    uint8_t index;
    while (xQueueReceive(queue_, &index, portMAX_DELAY) == pdTRUE) {
        xSemaphoreTake(mutex_, portMAX_DELAY);
        jobs_[index].running = true;
        const Job job = jobs_[index];
        const bool mounted = mounted_;
        xSemaphoreGive(mutex_);
        std::expected<std::string, std::string> result = std::unexpected("SD card is not mounted");
        if (job.operation == Operation::Mount) {
            result = Mount();
        } else if (mounted) {
            result = job.operation == Operation::List ? List(job) : Read(job);
        }
        if (result && result->size() > kMaxResponseBytes) {
            result = std::unexpected("SD result exceeds memory budget; request fewer entries");
        }
        CJsonUniquePtr envelope(cJSON_CreateObject());
        if (envelope) {
            cJSON_AddNumberToObject(envelope.get(), "job_id", job.id);
            cJSON_AddStringToObject(envelope.get(), "state", result ? "done" : "failed");
            if (result) {
                CJsonUniquePtr payload(cJSON_Parse(result->c_str()));
                if (!payload || !cJSON_AddItemToObject(envelope.get(), "result", payload.get())) {
                    cJSON_ReplaceItemInObject(envelope.get(), "state",
                                              cJSON_CreateString("failed"));
                    cJSON_AddStringToObject(envelope.get(), "error", "Cannot allocate SD result");
                } else {
                    payload.release();
                }
            } else {
                cJSON_AddStringToObject(envelope.get(), "error", result.error().c_str());
            }
        }
        auto response = Json(std::move(envelope));
        xSemaphoreTake(mutex_, portMAX_DELAY);
        jobs_[index].response = std::move(response);
        jobs_[index].done = true;
        jobs_[index].running = false;
        xSemaphoreGive(mutex_);
        taskYIELD();
    }
    vTaskDelete(nullptr);
}

#endif  // CONFIG_SPOTPEAR_SD_READER
