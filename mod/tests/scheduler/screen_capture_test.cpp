#include "dc_screen_capture.h"
#include <cassert>
#include <cstring>
#include <string>

int main()
{
    char path[260];
    assert(!dc_screen_capture_take(path, sizeof path));
    assert(dc_screen_capture_request(nullptr) == -1);
    assert(dc_screen_capture_request("") == -1);
    assert(dc_screen_capture_request(std::string(260, 'x').c_str()) == -1);
    assert(dc_screen_capture_request("capture.bmp") == 1);
    assert(dc_screen_capture_request("other.bmp") == 0);
    assert(!dc_screen_capture_take(path, 5));
    assert(dc_screen_capture_take(path, sizeof path));
    assert(!std::strcmp(path, "capture.bmp"));
    assert(!dc_screen_capture_take(path, sizeof path));
    assert(dc_screen_capture_request("other.bmp") == 1);
    assert(dc_screen_capture_take(path, sizeof path));
    assert(!std::strcmp(path, "other.bmp"));
}
