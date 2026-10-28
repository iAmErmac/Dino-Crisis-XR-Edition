#ifndef DC_SCREEN_CAPTURE_H
#define DC_SCREEN_CAPTURE_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Returns 1 when queued, 0 when busy, and -1 for an invalid path. */
int dc_screen_capture_request(const char *path);
int dc_screen_capture_take(char *path, size_t capacity);
#ifdef __cplusplus
}
#endif
#endif
