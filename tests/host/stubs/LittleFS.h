#pragma once
#include "FS.h"
#include "esp_partition.h"
#include <map>

struct TestLittleFS {
    std::map<std::string, std::shared_ptr<std::string>> files;
    bool formatted = false;
    bool mounted = false;
    bool mountFails = false;
    bool formatFails = false;
    unsigned formats = 0;
    unsigned unmounts = 0;
    bool begin(bool formatOnFail, const char *, uint8_t, const char *label)
    {
        assert(!formatOnFail);
        assert(std::strcmp(label, "spiffs") == 0);
        mounted = formatted && !mountFails;
        return mounted;
    }
    bool format()
    {
        ++formats;
        if (formatFails) return false;
        files.clear();
        formatted = true;
        flashBytes[0] = 0;
        return true;
    }
    size_t totalBytes() { return testPartition.size; }
    size_t usedBytes()
    {
        size_t used = 0;
        for (const auto &file : files) used += file.second->size();
        return used;
    }
    bool exists(const char *path) { return files.count(path) != 0; }
    File open(const char *path, const char *mode)
    {
        if (!mounted) return {};
        bool readOnly = std::strcmp(mode, FILE_READ) == 0;
        if ((!exists(path) && readOnly) || (!readOnly && fileWritesFail)) return {};
        auto &data = files[path];
        if (!data) data = std::make_shared<std::string>();
        return File(data, mode);
    }
    void end() { mounted = false; ++unmounts; }
};
static TestLittleFS LittleFS;
