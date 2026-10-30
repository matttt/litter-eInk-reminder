#pragma once
#include <algorithm>
#include <cassert>
#include <cstring>
#include <vector>

constexpr int ESP_OK = 0;
constexpr int ESP_PARTITION_TYPE_DATA = 1;
constexpr int ESP_PARTITION_SUBTYPE_DATA_SPIFFS = 0x82;
struct esp_partition_t { size_t size; };
static esp_partition_t testPartition{0x360000};
static std::vector<uint8_t> flashBytes(testPartition.size, 0xFF);
static bool partitionMissing = false;
static bool partitionReadFails = false;
inline const esp_partition_t *esp_partition_find_first(int type, int subtype,
                                                      const char *label)
{
    assert(type == ESP_PARTITION_TYPE_DATA);
    assert(subtype == ESP_PARTITION_SUBTYPE_DATA_SPIFFS);
    assert(std::strcmp(label, "spiffs") == 0);
    return partitionMissing ? nullptr : &testPartition;
}
inline int esp_partition_read(const esp_partition_t *, size_t offset,
                              void *buffer, size_t length)
{
    if (partitionReadFails) return -1;
    assert(offset + length <= flashBytes.size());
    std::memcpy(buffer, flashBytes.data() + offset, length);
    return ESP_OK;
}
