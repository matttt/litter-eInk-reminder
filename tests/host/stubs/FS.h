#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#define FILE_READ "r"
#define FILE_WRITE "w"
#define FILE_APPEND "a"
static bool fileWritesFail = false;
static size_t writtenBytes = 0;

// File handles share persistent contents but have independent cursors.
class File {
    std::shared_ptr<std::string> contents;
    size_t cursor = 0;
    bool writable = false;
public:
    File() = default;
    File(std::shared_ptr<std::string> data, const char *mode)
        : contents(data), writable(std::strcmp(mode, FILE_READ) != 0)
    {
        if (std::strcmp(mode, FILE_WRITE) == 0) contents->clear();
        if (std::strcmp(mode, FILE_APPEND) == 0) cursor = contents->size();
    }
    explicit operator bool() const { return contents != nullptr; }
    bool isDirectory() const { return false; }
    size_t size() const { return contents ? contents->size() : 0; }
    bool seek(size_t offset) { cursor = offset; return contents && offset <= size(); }
    size_t read(uint8_t *buffer, size_t length)
    {
        if (!contents || cursor > size()) return 0;
        length = std::min(length, size() - cursor);
        std::memcpy(buffer, contents->data() + cursor, length);
        cursor += length;
        return length;
    }
    int read() { uint8_t c; return read(&c, 1) ? c : -1; }
    size_t readBytesUntil(char delimiter, char *buffer, size_t length)
    {
        size_t count = 0;
        while (count < length) {
            int c = read();
            if (c < 0 || c == delimiter) break;
            buffer[count++] = static_cast<char>(c);
        }
        return count;
    }
    size_t write(const uint8_t *buffer, size_t length)
    {
        if (!contents || !writable || fileWritesFail) return 0;
        contents->replace(cursor, length,
                          reinterpret_cast<const char *>(buffer), length);
        cursor += length;
        writtenBytes += length;
        return length;
    }
    size_t write(uint8_t c) { return write(&c, 1); }
    size_t print(const char *text)
    {
        return write(reinterpret_cast<const uint8_t *>(text), std::strlen(text));
    }
    template <typename... Args>
    size_t printf(const char *format, Args... args)
    {
        char buffer[256];
        int length = std::snprintf(buffer, sizeof(buffer), format, args...);
        return length > 0 && static_cast<size_t>(length) < sizeof(buffer)
            ? print(buffer) : 0;
    }
    void flush() {}
    void close() { contents.reset(); }
};
