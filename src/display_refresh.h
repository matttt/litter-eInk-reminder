#pragma once

#include <stddef.h>
#include <stdint.h>

namespace DisplayRefresh {

constexpr int Width = 960;
constexpr int Height = 540;
constexpr int TileWidth = 120;
constexpr int TileHeight = 90;
constexpr size_t TileCount = (Width / TileWidth) * (Height / TileHeight);
constexpr uint8_t MaxPartialRefreshes = 16;

struct Region {
    int x;
    int y;
    int width;
    int height;
};

// Only these small fingerprints survive deep sleep, not the PSRAM framebuffer.
struct Snapshot {
    uint32_t magic;
    uint16_t version;
    uint8_t clean;
    uint8_t partialRefreshes;
    uint32_t tileHashes[TileCount];
    uint32_t checksum;
};

struct Plan {
    bool full;
    size_t regionCount;
    Region regions[TileCount];
    Snapshot next;
};

Plan plan(const uint8_t *framebuffer, bool clean, const Snapshot &previous);
// Pack rows without the full-screen stride, as required by the EPD driver.
void copyRegion(const uint8_t *framebuffer, const Region &region, uint8_t *out);

}  // namespace DisplayRefresh
