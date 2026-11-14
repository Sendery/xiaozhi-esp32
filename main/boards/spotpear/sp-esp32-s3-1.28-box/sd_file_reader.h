#pragma once

#include <sys/stat.h>

#include <algorithm>
#include <array>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <memory>
#include <string>

namespace spotpear_sd {

constexpr size_t kMaxPathBytes = 192;
constexpr size_t kMaxReadBytes = 4096;

// Paths are card-relative, never VFS paths. FAT does not support symlinks.
inline std::expected<std::string, std::string> NormalizePath(const std::string& input) {
    if (input.size() > kMaxPathBytes) {
        return std::unexpected("Path exceeds 192 bytes");
    }
    for (unsigned char ch : input) {
        if (ch < 32 || ch == 127 || ch == '\\' || ch == ':') {
            return std::unexpected("Invalid character in SD path");
        }
    }
    std::string result;
    size_t begin = 0;
    while (begin < input.size()) {
        size_t end = input.find('/', begin);
        if (end == std::string::npos) {
            end = input.size();
        }
        auto part = input.substr(begin, end - begin);
        if (part == "." || part == "..") {
            return std::unexpected("Dot segments are not allowed");
        }
        if (!part.empty()) {
            // FAT and Win32 trim trailing dots/spaces; reject aliases such as
            // ".. " instead of letting their meaning change at the filesystem.
            if (part.front() == ' ' || part.back() == ' ' || part.back() == '.') {
                return std::unexpected("Ambiguous FAT path component");
            }
            result += "/" + part;
        }
        begin = end + 1;
    }
    if (result.size() > kMaxPathBytes) {
        return std::unexpected("Path exceeds 192 bytes");
    }
    return result.empty() ? "/" : result;
}

inline bool IsUtf8Text(const std::string& text) {
    for (size_t i = 0; i < text.size();) {
        const auto ch = static_cast<unsigned char>(text[i++]);
        if (ch < 0x80) {
            if ((ch < 32 && ch != '\n' && ch != '\r' && ch != '\t') || ch == 127) {
                return false;
            }
            continue;
        }
        unsigned extra = 0;
        uint32_t value = 0;
        uint32_t minimum = 0;
        if (ch >= 0xc2 && ch <= 0xdf) {
            extra = 1;
            value = ch & 0x1f;
            minimum = 0x80;
        } else if (ch >= 0xe0 && ch <= 0xef) {
            extra = 2;
            value = ch & 0x0f;
            minimum = 0x800;
        } else if (ch >= 0xf0 && ch <= 0xf4) {
            extra = 3;
            value = ch & 0x07;
            minimum = 0x10000;
        } else {
            return false;
        }
        if (i + extra > text.size()) {
            return false;
        }
        while (extra--) {
            const auto next = static_cast<unsigned char>(text[i++]);
            if ((next & 0xc0) != 0x80) {
                return false;
            }
            value = (value << 6) | (next & 0x3f);
        }
        if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) {
            return false;
        }
    }
    return true;
}

struct FileChunk {
    std::string path;
    std::string bytes;
    uint64_t file_size;
    uint32_t offset;
    bool eof;
};

inline std::expected<FileChunk, std::string> ReadFile(const std::string& root,
                                                      const std::string& path, uint32_t offset,
                                                      size_t length) {
    auto normalized = NormalizePath(path);
    if (!normalized) {
        return std::unexpected(normalized.error());
    }
    if (length == 0 || length > kMaxReadBytes || offset > INT_MAX) {
        return std::unexpected("Invalid read range");
    }
    const auto full_path = root + *normalized;
    struct stat info = {};
    if (stat(full_path.c_str(), &info) != 0) {
        return std::unexpected("File not found or card unavailable");
    }
    if ((info.st_mode & S_IFREG) == 0 || info.st_size < 0 || info.st_size > INT_MAX) {
        return std::unexpected("Expected a regular file no larger than 2 GiB minus one byte");
    }
    const auto size = static_cast<uint64_t>(info.st_size);
    if (offset > size) {
        return std::unexpected("Offset is beyond end of file");
    }
    std::unique_ptr<FILE, decltype(&fclose)> file(fopen(full_path.c_str(), "rb"), &fclose);
    if (!file || fseek(file.get(), static_cast<long>(offset), SEEK_SET) != 0) {
        return std::unexpected("Cannot open or seek file");
    }
    std::string data(std::min<uint64_t>(length, size - offset), '\0');
    const auto read = fread(data.data(), 1, data.size(), file.get());
    if (ferror(file.get()) || read != data.size()) {
        return std::unexpected("Incomplete read: card removed, I/O error, or file changed");
    }
    return FileChunk{*normalized, std::move(data), size, offset, offset + read == size};
}

}  // namespace spotpear_sd
