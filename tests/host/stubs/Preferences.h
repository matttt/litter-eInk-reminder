#pragma once
#include <cassert>
#include <cstring>
#include <vector>

static std::vector<unsigned char> nvsBlob;
static bool nvsOpenFails = false;
static bool nvsWriteFails = false;
static bool nvsReadFails = false;
static unsigned nvsWrites = 0;
static unsigned nvsOpenHandles = 0;

class Preferences {
    bool opened = false;
    bool readOnly = false;
public:
    bool begin(const char *name, bool read = false) {
        assert(std::strcmp(name, "last-clean") == 0);
        if (nvsOpenFails || (read && nvsBlob.empty())) return false;
        opened = true;
        readOnly = read;
        ++nvsOpenHandles;
        return true;
    }
    void end() {
        assert(opened);
        opened = false;
        --nvsOpenHandles;
    }
    size_t getBytesLength(const char *key) {
        assert(opened && std::strcmp(key, "state") == 0);
        return nvsBlob.size();
    }
    size_t getBytes(const char *key, void *out, size_t length) {
        assert(opened && std::strcmp(key, "state") == 0);
        if (nvsReadFails || nvsBlob.size() > length) return 0;
        std::memcpy(out, nvsBlob.data(), nvsBlob.size());
        return nvsBlob.size();
    }
    size_t putBytes(const char *key, const void *data, size_t length) {
        assert(opened && !readOnly && std::strcmp(key, "state") == 0);
        if (nvsWriteFails) return 0;
        const auto *bytes = static_cast<const unsigned char *>(data);
        nvsBlob.assign(bytes, bytes + length);
        ++nvsWrites;
        return length;
    }
    ~Preferences() { assert(!opened); }
};
