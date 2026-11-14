#include <cassert>
#include <climits>
#include <iostream>
#include <string>

#include "main/boards/spotpear/sp-esp32-s3-1.28-box/sd_file_reader.h"

int main(int argc, char** argv) {
    assert(argc == 2);
    using namespace spotpear_sd;
    const std::string root = argv[1];
    assert(NormalizePath("").value() == "/");
    assert(NormalizePath("///photos//a.jpg/").value() == "/photos/a.jpg");
    assert(NormalizePath("/sdcard/file").value() == "/sdcard/file");
    for (const auto* path : {"../secret", "/a/../b", "/./file", "C:/Windows", "a\\b", "a\nb"}) {
        assert(!NormalizePath(path));
    }
    assert(!NormalizePath(std::string("a\0b", 3)));
    for (const auto* path :
         {".. /secret", ".../secret", "folder./file", " folder/file", "a /file"}) {
        assert(!NormalizePath(path));
    }
    assert(!NormalizePath(std::string(193, 'a')));
    assert(!NormalizePath(std::string(192, 'a')));  // Leading slash is counted.
    assert(NormalizePath(std::string(191, 'a')));
    assert(IsUtf8Text(""));
    assert(IsUtf8Text("Hola\n\tEspa\xc3\xb1\x61"));
    assert(IsUtf8Text("\xf0\x9f\x98\x80"));
    for (const auto& bytes : {std::string("a\0b", 3), std::string("\xc0\xaf"),
                              std::string("\xed\xa0\x80"), std::string("\xf4\x90\x80\x80"),
                              std::string("\xc3"), std::string("\x80"), std::string("\x7f")}) {
        assert(!IsUtf8Text(bytes));
    }
    auto whole = ReadFile(root, "/hello.txt", 0, 4096);
    assert(whole && whole->bytes == "Hola desde SD\n" && whole->eof);
    auto first = ReadFile(root, "hello.txt", 0, 4);
    assert(first && first->bytes == "Hola" && !first->eof);
    auto last = ReadFile(root, "/hello.txt", 4, 4096);
    assert(last && last->bytes == " desde SD\n" && last->offset == 4 && last->eof);
    auto eof = ReadFile(root, "/hello.txt", 14, 1);
    assert(eof && eof->bytes.empty() && eof->eof);
    auto empty = ReadFile(root, "/empty.txt", 0, 4096);
    assert(empty && empty->bytes.empty() && empty->eof);
    auto binary = ReadFile(root, "/binary.bin", 0, 4096);
    assert(binary && binary->bytes == std::string("\0\xff\x01", 3));
    assert(!IsUtf8Text(binary->bytes));
    auto large = ReadFile(root, "/large.bin", 4096, 4096);
    assert(large && large->bytes.size() == 4096 && !large->eof);
    auto tail = ReadFile(root, "/large.bin", 8192, 4096);
    assert(tail && tail->bytes == "x" && tail->eof);
    assert(!ReadFile(root, "/hello.txt", 15, 1));
    assert(!ReadFile(root, "/hello.txt", UINT32_MAX, 1));
    assert(!ReadFile(root, "/hello.txt", 0, 0));
    assert(!ReadFile(root, "/hello.txt", 0, 4097));
    assert(!ReadFile(root, "../hello.txt", 0, 10));
    assert(!ReadFile(root, "/missing", 0, 10));
    assert(!ReadFile(root, "/", 0, 10));
    std::cout << "SD reader path, UTF-8, chunking and read-only fixture checks passed\n";
}
