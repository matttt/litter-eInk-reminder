#include "display_refresh.h"

#include <string.h>

namespace {

constexpr uint32_t Magic = 0x45504446;  // "EPDF"
constexpr uint16_t Version = 1;

uint32_t hashBytes(const uint8_t *bytes, size_t length, uint32_t hash = 2166136261U)
{
    for (size_t i = 0; i < length; ++i) {
        hash = (hash ^ bytes[i]) * 16777619U;
    }
    return hash;
}

uint32_t checksum(const DisplayRefresh::Snapshot &snapshot)
{
    return hashBytes(reinterpret_cast<const uint8_t *>(&snapshot),
                     offsetof(DisplayRefresh::Snapshot, checksum));
}

bool valid(const DisplayRefresh::Snapshot &snapshot)
{
    return snapshot.magic == Magic && snapshot.version == Version &&
           snapshot.clean <= 1 &&
           snapshot.partialRefreshes <= DisplayRefresh::MaxPartialRefreshes &&
           snapshot.checksum == checksum(snapshot);
}

}  // namespace

namespace DisplayRefresh {

Plan plan(const uint8_t *framebuffer, bool clean, const Snapshot &previous)
{
    Plan result = {};
    result.next.magic = Magic;
    result.next.version = Version;
    result.next.clean = clean;
    result.full = !valid(previous) || !clean || !previous.clean;

    size_t tile = 0;
    for (int y = 0; y < Height; y += TileHeight) {
        for (int x = 0; x < Width; x += TileWidth, ++tile) {
            uint32_t hash = 2166136261U;
            for (int row = y; row < y + TileHeight; ++row) {
                hash = hashBytes(framebuffer + row * Width / 2 + x / 2,
                                 TileWidth / 2, hash);
            }
            result.next.tileHashes[tile] = hash;
        }
    }

    if (!result.full) {
        // Combine adjacent changed tiles into horizontal runs, then merge
        // matching runs vertically. Distant labels stay separate rectangles.
        constexpr int Columns = Width / TileWidth;
        for (int row = 0; row < Height / TileHeight; ++row) {
            for (int column = 0; column < Columns;) {
                size_t index = row * Columns + column;
                if (result.next.tileHashes[index] == previous.tileHashes[index]) {
                    ++column;
                    continue;
                }
                int start = column++;
                while (column < Columns &&
                       result.next.tileHashes[row * Columns + column] !=
                           previous.tileHashes[row * Columns + column]) {
                    ++column;
                }
                Region changed = {start * TileWidth, row * TileHeight,
                                  (column - start) * TileWidth, TileHeight};
                bool merged = false;
                for (size_t i = 0; i < result.regionCount; ++i) {
                    Region &region = result.regions[i];
                    if (region.x == changed.x && region.width == changed.width &&
                        region.y + region.height == changed.y) {
                        region.height += changed.height;
                        merged = true;
                        break;
                    }
                }
                if (!merged) result.regions[result.regionCount++] = changed;
            }
        }
        if (result.regionCount > 0) {
            if (previous.partialRefreshes >= MaxPartialRefreshes) {
                result.full = true;
                result.regionCount = 0;
            } else {
                result.next.partialRefreshes = previous.partialRefreshes + 1;
            }
        } else {
            result.next.partialRefreshes = previous.partialRefreshes;
        }
    }
    result.next.checksum = checksum(result.next);
    return result;
}

void copyRegion(const uint8_t *framebuffer, const Region &region, uint8_t *out)
{
    for (int row = 0; row < region.height; ++row) {
        memcpy(out + row * region.width / 2,
               framebuffer + (region.y + row) * Width / 2 + region.x / 2,
               region.width / 2);
    }
}

}  // namespace DisplayRefresh
