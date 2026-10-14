#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <map>
#include <cstring>

class Preferences {
public:
    static inline std::map<std::string, std::vector<uint8_t>> s_storage;

    static void clearAllStorage() {
        s_storage.clear();
    }

    bool begin(const char* name, bool readOnly = false) {
        namespace_ = name ? name : "";
        isOpen_ = true;
        return true;
    }

    void end() {
        isOpen_ = false;
    }

    size_t putBytes(const char* key, const void* value, size_t len) {
        if (!isOpen_ || !key || !value || len == 0) return 0;
        std::string fullKey = namespace_ + ":" + key;
        const uint8_t* p = reinterpret_cast<const uint8_t*>(value);
        s_storage[fullKey] = std::vector<uint8_t>(p, p + len);
        return len;
    }

    size_t getBytesLength(const char* key) {
        if (!isOpen_ || !key) return 0;
        auto it = s_storage.find(namespace_ + ":" + key);
        return it == s_storage.end() ? 0 : it->second.size();
    }

    // Like arduino-esp32: returns 0 (and copies nothing) if the blob does not fit in buf
    size_t getBytes(const char* key, void* buf, size_t maxLen) {
        size_t len = getBytesLength(key);
        if (!len || !buf || !maxLen) return len;
        if (len > maxLen) return 0;
        std::memcpy(buf, s_storage[namespace_ + ":" + key].data(), len);
        return len;
    }

    size_t putUInt(const char* key, uint32_t value) {
        return putBytes(key, &value, sizeof(value));
    }

    uint32_t getUInt(const char* key, uint32_t defaultValue = 0) {
        uint32_t value = defaultValue;
        if (getBytesLength(key) == sizeof(value)) getBytes(key, &value, sizeof(value));
        return value;
    }

    bool isKey(const char* key) {
        if (!isOpen_ || !key) return false;
        std::string fullKey = namespace_ + ":" + key;
        return s_storage.find(fullKey) != s_storage.end();
    }

    bool remove(const char* key) {
        if (!isOpen_ || !key) return false;
        std::string fullKey = namespace_ + ":" + key;
        return s_storage.erase(fullKey) > 0;
    }

private:
    std::string namespace_;
    bool isOpen_{false};
};
