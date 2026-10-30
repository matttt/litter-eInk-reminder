#include "../../src/display_refresh.cpp"

#include <cassert>
#include <iostream>
#include <vector>

using namespace DisplayRefresh;

static void change(std::vector<uint8_t> &frame, int x, int y)
{
    frame[y * Width / 2 + x / 2] ^= 0x0F;
}

static void apply(const Plan &update, const std::vector<uint8_t> &frame,
                  std::vector<uint8_t> &panel)
{
    assert(!update.full);
    for (size_t i = 0; i < update.regionCount; ++i) {
        const Region &region = update.regions[i];
        assert(region.x >= 0 && region.y >= 0);
        assert(region.x + region.width <= Width);
        assert(region.y + region.height <= Height);
        assert(region.x % 2 == 0 && region.width % 2 == 0);
        std::vector<uint8_t> packed(region.width * region.height / 2);
        copyRegion(frame.data(), region, packed.data());
        // Simulate the driver's area clear followed by drawing packed rows.
        for (int row = 0; row < region.height; ++row) {
            uint8_t *out = panel.data() + (region.y + row) * Width / 2 + region.x / 2;
            memset(out, 0xFF, region.width / 2);
            memcpy(out, packed.data() + row * region.width / 2, region.width / 2);
        }
    }
    assert(panel == frame);
}

int main()
{
    std::vector<uint8_t> frame(Width * Height / 2, 0xFF);
    auto initial = plan(frame.data(), true, {});
    assert(initial.full && initial.next.partialRefreshes == 0);
    Snapshot previous = initial.next;
    auto update = plan(frame.data(), true, previous);
    assert(!update.full && update.regionCount == 0);

    // A timestamp and today's history marker update separate small areas.
    auto panel = frame;
    change(frame, 14, 99);
    change(frame, 900, 60);
    update = plan(frame.data(), true, previous);
    assert(!update.full && update.regionCount == 2);
    assert(update.next.partialRefreshes == 1);
    apply(update, frame, panel);
    previous = update.next;

    // Erasing old text is a change too, even when the new pixels are white.
    change(frame, 14, 99);
    update = plan(frame.data(), true, previous);
    assert(!update.full && update.regionCount == 1);
    apply(update, frame, panel);

    // Adjacent changed tiles combine horizontally and vertically.
    previous = initial.next;
    std::fill(frame.begin(), frame.end(), 0xFF);
    change(frame, 900, 10);
    change(frame, 900, 100);
    change(frame, 900, 200);
    update = plan(frame.data(), true, previous);
    assert(update.regionCount == 1);
    assert(update.regions[0].x == 840 && update.regions[0].height == 270);
    change(frame, 10, 10);
    change(frame, 130, 10);
    update = plan(frame.data(), true, previous);
    assert(update.regionCount == 2 && update.regions[0].width == 240);

    // Dense, disconnected changes stay within the region array and cover
    // every byte they must update, including both screen edges.
    std::fill(frame.begin(), frame.end(), 0xFF);
    panel = frame;
    for (int row = 0; row < Height / TileHeight; ++row) {
        for (int column = 0; column < Width / TileWidth; ++column) {
            if ((row + column) % 2 == 0) {
                change(frame, column * TileWidth, row * TileHeight);
            }
        }
    }
    change(frame, Width - 1, Height - 1);
    update = plan(frame.data(), true, previous);
    assert(update.regionCount <= TileCount);
    apply(update, frame, panel);

    // An unknown/interrupted frame, corrupt retained data, and either
    // direction of clean/dirty transition must use the full waveform.
    previous = update.next;
    auto invalid = previous;
    invalid.magic = 0;
    assert(plan(frame.data(), true, invalid).full);
    invalid = previous;
    invalid.tileHashes[0] ^= 1;
    assert(plan(frame.data(), true, invalid).full);
    auto dirty = plan(frame.data(), false, previous);
    assert(dirty.full);
    assert(plan(frame.data(), true, dirty.next).full);

    // Unchanged wakes do not consume the periodic full-refresh budget.
    previous = initial.next;
    std::fill(frame.begin(), frame.end(), 0xFF);
    for (unsigned i = 0; i < MaxPartialRefreshes; ++i) {
        change(frame, 10, 10);
        update = plan(frame.data(), true, previous);
        assert(!update.full && update.next.partialRefreshes == i + 1);
        previous = update.next;
    }
    update = plan(frame.data(), true, previous);
    assert(!update.full && update.regionCount == 0);
    assert(update.next.partialRefreshes == MaxPartialRefreshes);
    change(frame, 10, 10);
    update = plan(frame.data(), true, previous);
    assert(update.full && update.next.partialRefreshes == 0);
    std::cout << "Display refresh host tests passed\n";
}
