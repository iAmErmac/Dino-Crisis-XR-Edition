#include "dc_memcard_snapshot.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define MKDIR(path) _mkdir(path)
#define RMDIR(path) _rmdir(path)
#else
#include <sys/stat.h>
#include <unistd.h>
#define MKDIR(path) mkdir(path, 0755)
#define RMDIR(path) rmdir(path)
#endif

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "line %d: %s\n", __LINE__, #expr); \
        return 1; \
    } \
} while (0)

static const char *path0 = "card-snapshot-fixture/card1.mcd";
static const char *path1 = "card-snapshot-fixture/card2.mcd";
static uint8_t old_base[MEMCARD_SIZE];
static uint8_t new_base[MEMCARD_SIZE];
static DcMemcardSnapshot saved, decoded, dirty_saved, absent;

static int make_fixture_dir(void)
{
    (void)MKDIR("card-snapshot-fixture");
    (void)remove(path0);
    (void)remove(path1);
    (void)remove("blocked_target.tmp");
    (void)RMDIR("blocked_target");
    (void)MKDIR("blocked_target");
    return 1;
}

static int format_with_tail(const char *path, const char *tail)
{
    FILE *file;
    size_t n = strlen(tail);
    if (memcard_format_file(path) != 0) return 0;
    file = fopen(path, "ab");
    if (!file) return 0;
    int ok = fwrite(tail, 1, n, file) == n && fclose(file) == 0;
    if (!ok) fclose(file);
    return ok;
}

static int read_base(const char *path, uint8_t *out)
{
    FILE *file = fopen(path, "rb");
    if (!file) return 0;
    int ok = fread(out, 1, MEMCARD_SIZE, file) == MEMCARD_SIZE;
    if (fclose(file) != 0) ok = 0;
    return ok;
}

static int rewrite_with_tail(const char *path, const uint8_t *base,
                             const char *tail)
{
    FILE *file = fopen(path, "wb");
    size_t n = strlen(tail);
    if (!file) return 0;
    int ok = fwrite(base, 1, MEMCARD_SIZE, file) == MEMCARD_SIZE &&
             fwrite(tail, 1, n, file) == n && fclose(file) == 0;
    if (!ok) fclose(file);
    return ok;
}

static int tail_is(const char *path, const char *tail)
{
    FILE *file = fopen(path, "rb");
    char got[32];
    size_t n = strlen(tail);
    if (!file || n >= sizeof(got) || fseek(file, MEMCARD_SIZE, SEEK_SET) != 0)
        return 0;
    int ok = fread(got, 1, n, file) == n && memcmp(got, tail, n) == 0;
    if (fclose(file) != 0) ok = 0;
    return ok;
}

static int write_sector_value(int card, int sector, uint8_t value)
{
    uint8_t data[MEMCARD_SECTOR_SIZE];
    memset(data, value, sizeof(data));
    return memcard_write_sector(card, sector, data) == 0;
}

static int card_dirty(int card)
{
    int dirty = 0;
    CHECK(memcard_debug_info(card, NULL, NULL, NULL, &dirty) == 0);
    return dirty;
}

int main(void)
{
    const MemcardSlotConfig slots[2] = {
        { path0, 1 }, { path1, 1 }
    };
    DcMemcardRestoreOptions no_flush = { 0 };
    DcMemcardRestoreOptions flush = { 1 };
    uint8_t *wire;
    uint8_t *wire_bad;
    uint8_t current[MEMCARD_SIZE];
    uint8_t file_before[MEMCARD_SIZE];

    CHECK(make_fixture_dir());
    CHECK(format_with_tail(path0, "EXTENSION-A"));
    CHECK(format_with_tail(path1, "EXTENSION-B"));
    memcard_set_extension_codec(NULL, NULL);
    memcard_init_slots(NULL, slots);
    CHECK(memcard_is_present(0) && memcard_is_present(1));

    CHECK(write_sector_value(0, 10, 0x11));
    memcard_flush(0);
    CHECK(memcard_last_flush_result(0) == 0 && !card_dirty(0));
    CHECK(memcard_export_raw(0, old_base) == 0);
    CHECK(dc_memcard_snapshot_capture(&saved));
    CHECK(saved.present[0] == 1 && saved.dirty[0] == 0);

    wire = (uint8_t *)malloc(dc_memcard_snapshot_wire_bytes());
    wire_bad = (uint8_t *)malloc(dc_memcard_snapshot_wire_bytes());
    CHECK(wire && wire_bad);
    CHECK(dc_memcard_snapshot_write(&saved, wire,
                                    dc_memcard_snapshot_wire_bytes()));
    CHECK(dc_memcard_snapshot_validate(wire,
                                       dc_memcard_snapshot_wire_bytes()));
    CHECK(dc_memcard_snapshot_read(wire,
                                   dc_memcard_snapshot_wire_bytes(), &decoded));
    CHECK(!memcmp(&saved, &decoded, sizeof(saved)));

    CHECK(write_sector_value(0, 10, 0x22));
    memcard_flush(0);
    CHECK(memcard_last_flush_result(0) == 0 && !card_dirty(0));
    CHECK(memcard_export_raw(0, new_base) == 0);
    CHECK(memcmp(old_base, new_base, sizeof(old_base)) != 0);
    CHECK(rewrite_with_tail(path0, new_base, "EXTENSION-NEW"));
    CHECK(memcard_reload_bound() == 0);
    CHECK(tail_is(path0, "EXTENSION-NEW"));
    CHECK(dc_memcard_snapshot_restore(&decoded,
                                      DC_MEMCARD_RESTORE_KEEP_DISK,
                                      NULL));
    CHECK(memcard_export_raw(0, current) == 0);
    CHECK(!memcmp(current, new_base, sizeof(current)) && !card_dirty(0));

    CHECK(dc_memcard_snapshot_restore(&decoded,
                                      DC_MEMCARD_RESTORE_REWIND,
                                      &no_flush));
    CHECK(memcard_export_raw(0, current) == 0);
    CHECK(!memcmp(current, old_base, sizeof(current)) && card_dirty(0));
    CHECK(read_base(path0, file_before) && !memcmp(file_before, new_base,
                                                    sizeof(file_before)));
    CHECK(tail_is(path0, "EXTENSION-NEW"));

    CHECK(memcard_reload_bound() == 0);
    CHECK(memcard_export_raw(0, current) == 0 && !memcmp(current, new_base,
                                                          sizeof(current)));
    CHECK(dc_memcard_snapshot_restore(&decoded,
                                      DC_MEMCARD_RESTORE_REWIND,
                                      &flush));
    CHECK(memcard_last_flush_result(0) == 0 && !card_dirty(0));
    CHECK(read_base(path0, file_before) && !memcmp(file_before, old_base,
                                                    sizeof(file_before)));
    CHECK(tail_is(path0, "EXTENSION-NEW"));

    CHECK(write_sector_value(0, 10, 0x33));
    memcard_flush(0);
    CHECK(memcard_last_flush_result(0) == 0);
    CHECK(read_base(path0, new_base));
    CHECK(memcard_rebind_path(0, "blocked_target") == 0);
    CHECK(dc_memcard_snapshot_restore(&decoded,
                                      DC_MEMCARD_RESTORE_REWIND,
                                      &flush) == 0);
    CHECK(memcard_last_flush_result(0) == -1 && card_dirty(0));
    CHECK(read_base(path0, file_before) && !memcmp(file_before, new_base,
                                                    sizeof(file_before)));

    CHECK(memcard_rebind_path(0, path0) == 0);
    CHECK(memcard_reload_bound() == 0);
    CHECK(memcard_export_raw(0, current) == 0 && !memcmp(current, new_base,
                                                          sizeof(current)));
    absent = decoded;
    absent.present[0] = 0;
    absent.dirty[0] = 0;
    memset(absent.data[0], 0, sizeof(absent.data[0]));
    CHECK(dc_memcard_snapshot_state_validate(&absent));
    CHECK(dc_memcard_snapshot_restore(&absent,
                                      DC_MEMCARD_RESTORE_REWIND,
                                      NULL));
    CHECK(memcard_is_present(0) && memcard_export_raw(0, current) == 0 &&
          !memcmp(current, new_base, sizeof(current)));
    CHECK(memcard_rebind_path(0, NULL) == 0 && memcard_reload_bound() == 0);
    CHECK(!memcard_is_present(0));
    CHECK(dc_memcard_snapshot_restore(&decoded,
                                      DC_MEMCARD_RESTORE_REWIND,
                                      &no_flush));
    CHECK(!memcard_is_present(0));
    CHECK(memcard_rebind_path(0, path0) == 0 && memcard_reload_bound() == 0);
    CHECK(memcard_is_present(0));

    CHECK(write_sector_value(0, 11, 0x44));
    CHECK(dc_memcard_snapshot_capture(&dirty_saved));
    CHECK(dirty_saved.dirty[0] == 1);
    CHECK(memcard_reload_bound() == 0);

    memset(&decoded, 0xA5, sizeof(decoded));
    memcpy(wire_bad, wire, dc_memcard_snapshot_wire_bytes());
    wire_bad[0] ^= 1u;
    CHECK(!dc_memcard_snapshot_validate(wire_bad,
                                        dc_memcard_snapshot_wire_bytes()));
    CHECK(!dc_memcard_snapshot_read(wire_bad,
                                    dc_memcard_snapshot_wire_bytes(), &decoded));
    CHECK(decoded.present[0] == 0xA5 && decoded.dirty[0] == 0xA5);
    CHECK(memcard_export_raw(0, current) == 0);
    DcMemcardSnapshot invalid = saved;
    invalid.present[0] = 2;
    CHECK(!dc_memcard_snapshot_restore(&invalid,
                                       DC_MEMCARD_RESTORE_REWIND,
                                       &flush));
    CHECK(memcard_export_raw(0, file_before) == 0);
    CHECK(!memcmp(file_before, current, sizeof(file_before)));

    free(wire_bad);
    free(wire);
    (void)remove(path0);
    (void)remove(path1);
    (void)RMDIR("blocked_target");
    (void)RMDIR("card-snapshot-fixture");
    puts("memcard snapshot: wire, retention, rewind, current presence and atomic flush passed");
    return 0;
}
