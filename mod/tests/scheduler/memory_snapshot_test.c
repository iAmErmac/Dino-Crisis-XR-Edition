#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "dc_memory_policy.h"
#include "dc_memory_snapshot.h"
#include "test_process.h"

void psx_fatal_halt(const char *message) { (void)message; abort(); }

enum { ARENA = 0x00400000u, MOD = 0x01000000u, MB = 1024 * 1024 };

int main(int argc, char **argv)
{
    test_process_init();
    uint8_t empty[32];
    assert(dc_memory_snapshot_bytes() == sizeof empty);
    assert(dc_memory_snapshot_write(empty, sizeof empty));
    if (argc > 1) {
        assert(!strcmp(argv[1], "zero"));
        assert(!dc_memory_extra_span(MOD, 1));
        assert(!dc_memory_extra_ptr(MOD, 1));
        assert(!dc_node_alloc_extra());
        assert(dc_memory_snapshot_validate(empty, sizeof empty));
        assert(dc_memory_snapshot_read(empty, sizeof empty));
        return 0;
    }
    uint8_t *a = dc_memory_extra_ptr(ARENA, 1);
    uint8_t *b = dc_memory_extra_ptr(ARENA + 8191, 1);
    uint8_t *tail = dc_memory_extra_ptr(ARENA + MB - 1, 1);
    uint8_t *m = dc_memory_extra_ptr(MOD + 23, 1);
    *a = 0x11; *b = 0x22; *tail = 0x33; *m = 0x44;
    uint32_t first = dc_node_alloc_extra(); assert(first);
    memset(dc_memory_extra_ptr((first & 0x1FFFFFFFu) + 0x30, 4), 0, 4);
    size_t size = dc_memory_snapshot_bytes();
    uint8_t *wire = malloc(size), *bad = malloc(size), *again = malloc(size);
    assert(wire && bad && again && size < 6 * 4096);
    assert(dc_memory_snapshot_write(wire, size));
    assert(dc_memory_snapshot_validate(wire, size));
    uint32_t next = dc_node_alloc_extra(); assert(next == first + 0xAC);
    *a = 99; *b = 99; *tail = 99; *m = 99;
    uint8_t *stale = dc_memory_extra_ptr(MOD + MB - 1, 1); *stale = 0x77;
    assert(dc_memory_snapshot_read(wire, size));
    assert(a == dc_memory_extra_ptr(ARENA, 1));
    assert(*a == 0x11 && *b == 0x22 && *tail == 0x33 && *m == 0x44 && *stale == 0);
    assert(dc_memory_snapshot_bytes() == size);
    assert(dc_memory_snapshot_write(again, size)); assert(!memcmp(wire, again, size));
    assert(dc_node_alloc_extra() == next);
    assert(dc_memory_snapshot_read(wire, size));
    memcpy(bad, wire, size); bad[0] = 2;
    assert(!dc_memory_snapshot_read(bad, size));
    memcpy(bad, wire, size); bad[4] ^= 1;
    assert(!dc_memory_snapshot_read(bad, size));
    memcpy(bad, wire, size); memset(bad + 20, 0xFF, 4);
    assert(!dc_memory_snapshot_read(bad, size));
    memcpy(bad, wire, size); memset(bad + 4132, 0, 4);
    assert(!dc_memory_snapshot_read(bad, size));
    for (size_t n = 0; n < size; ++n) assert(!dc_memory_snapshot_validate(wire, n));
    assert(!dc_memory_snapshot_read(wire, size - 1));
    assert(!dc_memory_snapshot_validate(wire, size + 1));
    assert(!dc_memory_snapshot_read(NULL, size));
    assert(dc_memory_snapshot_write(again, size)); assert(!memcmp(wire, again, size));
    memset(bad, 0xA5, size);
    assert(!dc_memory_snapshot_write(bad, size - 1));
    for (size_t n = 0; n < size; ++n) assert(bad[n] == 0xA5);
    uint8_t *alias = dc_memory_extra_ptr(ARENA + 100000, (uint32_t)size);
    memcpy(alias, wire, size);
    assert(dc_memory_snapshot_read(alias, size));
    assert(*alias == 0 && *a == 0x11 && *tail == 0x33);
    assert(dc_memory_snapshot_read(empty, sizeof empty));
    assert(*a == 0 && *b == 0 && *tail == 0 && *m == 0);
    assert(dc_memory_snapshot_bytes() == sizeof empty);
    assert(dc_node_alloc_extra() == first);
    free(again); free(bad); free(wire); return 0;
}
