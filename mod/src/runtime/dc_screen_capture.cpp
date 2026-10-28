#include "dc_screen_capture.h"
#include <cstring>
#include <mutex>

namespace {
std::mutex capture_mutex;
char pending_path[260];
}

extern "C" int dc_screen_capture_request(const char *path)
{
    if (!path) return -1;
    size_t length = 0;
    while (length < sizeof pending_path && path[length]) ++length;
    if (!length || length == sizeof pending_path) return -1;
    std::lock_guard<std::mutex> lock(capture_mutex);
    if (pending_path[0]) return 0;
    std::memcpy(pending_path, path, length + 1);
    return 1;
}

extern "C" int dc_screen_capture_take(char *path, size_t capacity)
{
    if (!path || !capacity) return 0;
    std::lock_guard<std::mutex> lock(capture_mutex);
    if (!pending_path[0]) return 0;
    size_t length = std::strlen(pending_path);
    if (capacity <= length) return 0;
    std::memcpy(path, pending_path, length + 1);
    pending_path[0] = 0;
    return 1;
}
