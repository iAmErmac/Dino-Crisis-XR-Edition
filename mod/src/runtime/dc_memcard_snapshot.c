#include "dc_memcard_snapshot.h"

#include "pst_wire.h"

#include <stdlib.h>
#include <string.h>

static int valid_flag(uint8_t value)
{
    return value <= 1u;
}

size_t dc_memcard_snapshot_wire_bytes(void)
{
    return (size_t)DC_MEMCARD_SNAPSHOT_WIRE_BYTES;
}

int dc_memcard_snapshot_state_validate(const DcMemcardSnapshot *snapshot)
{
    if (!snapshot) return 0;
    for (int card = 0; card < (int)DC_MEMCARD_SNAPSHOT_CARDS; ++card) {
        if (!valid_flag(snapshot->present[card]) ||
            !valid_flag(snapshot->dirty[card])) return 0;
        if (snapshot->present[card] &&
            (snapshot->data[card][0] != 'M' ||
             snapshot->data[card][1] != 'C')) return 0;
    }
    return 1;
}

int dc_memcard_snapshot_capture(DcMemcardSnapshot *out)
{
    if (!out) return 0;
    memset(out, 0, sizeof(*out));
    for (int card = 0; card < (int)DC_MEMCARD_SNAPSHOT_CARDS; ++card) {
        int present = memcard_is_present(card);
        int dirty = 0;
        if (memcard_debug_info(card, NULL, NULL, &present, &dirty) != 0)
            return 0;
        out->present[card] = present ? 1u : 0u;
        out->dirty[card] = dirty ? 1u : 0u;
        if (present && memcard_export_raw(card, out->data[card]) != 0)
            return 0;
    }
    return dc_memcard_snapshot_state_validate(out);
}

static int wire_state(PstW *writer, const DcMemcardSnapshot *snapshot)
{
    if (!pst_w_u32(writer, DC_MEMCARD_SNAPSHOT_MAGIC) ||
        !pst_w_u32(writer, DC_MEMCARD_SNAPSHOT_VERSION) ||
        !pst_w_u32(writer, DC_MEMCARD_SNAPSHOT_CARDS) ||
        !pst_w_u32(writer, MEMCARD_SIZE)) return 0;
    for (int card = 0; card < (int)DC_MEMCARD_SNAPSHOT_CARDS; ++card) {
        if (!pst_w_u8(writer, snapshot->present[card]) ||
            !pst_w_u8(writer, snapshot->dirty[card]) ||
            !pst_w_u16(writer, 0u) ||
            !pst_w_bytes(writer, snapshot->data[card], MEMCARD_SIZE))
            return 0;
    }
    return 1;
}

int dc_memcard_snapshot_write(const DcMemcardSnapshot *snapshot,
                              uint8_t *wire, size_t capacity)
{
    if (!dc_memcard_snapshot_state_validate(snapshot) ||
        !wire || capacity < dc_memcard_snapshot_wire_bytes()) return 0;
    PstW writer;
    pst_w_init(&writer, wire, capacity);
    if (!wire_state(&writer, snapshot) ||
        writer.written != dc_memcard_snapshot_wire_bytes()) return 0;
    return 1;
}

static int read_state(PstR *reader, DcMemcardSnapshot *snapshot)
{
    uint32_t magic, version, cards, card_bytes;
    if (!pst_r_u32(reader, &magic) || !pst_r_u32(reader, &version) ||
        !pst_r_u32(reader, &cards) || !pst_r_u32(reader, &card_bytes) ||
        magic != DC_MEMCARD_SNAPSHOT_MAGIC ||
        version != DC_MEMCARD_SNAPSHOT_VERSION ||
        cards != DC_MEMCARD_SNAPSHOT_CARDS || card_bytes != MEMCARD_SIZE)
        return 0;
    memset(snapshot, 0, sizeof(*snapshot));
    for (int card = 0; card < (int)DC_MEMCARD_SNAPSHOT_CARDS; ++card) {
        uint16_t reserved;
        if (!pst_r_u8(reader, &snapshot->present[card]) ||
            !pst_r_u8(reader, &snapshot->dirty[card]) ||
            !pst_r_u16(reader, &reserved) || reserved != 0u ||
            !pst_r_bytes(reader, snapshot->data[card], MEMCARD_SIZE))
            return 0;
    }
    return dc_memcard_snapshot_state_validate(snapshot);
}

int dc_memcard_snapshot_read(const uint8_t *wire, size_t size,
                             DcMemcardSnapshot *out)
{
    if (!wire || !out || size != dc_memcard_snapshot_wire_bytes()) return 0;
    DcMemcardSnapshot *decoded = (DcMemcardSnapshot *)malloc(sizeof(*decoded));
    if (!decoded) return 0;
    PstR reader;
    pst_r_init(&reader, wire, size);
    int ok = read_state(&reader, decoded) && reader.p == reader.end;
    if (ok) memcpy(out, decoded, sizeof(*out));
    free(decoded);
    return ok;
}

int dc_memcard_snapshot_validate(const uint8_t *wire, size_t size)
{
    DcMemcardSnapshot *decoded;
    if (!wire || size != dc_memcard_snapshot_wire_bytes()) return 0;
    decoded = (DcMemcardSnapshot *)malloc(sizeof(*decoded));
    if (!decoded) return 0;
    int ok = dc_memcard_snapshot_read(wire, size, decoded);
    free(decoded);
    return ok;
}

int dc_memcard_snapshot_restore(const DcMemcardSnapshot *snapshot,
                                DcMemcardRestoreMode mode,
                                const DcMemcardRestoreOptions *options)
{
    if (!dc_memcard_snapshot_state_validate(snapshot)) return 0;
    if (mode == DC_MEMCARD_RESTORE_KEEP_DISK) return 1;
    if (mode != DC_MEMCARD_RESTORE_REWIND) return 0;

    for (int card = 0; card < (int)DC_MEMCARD_SNAPSHOT_CARDS; ++card) {
        /* Presence is host/device state. Never rebind it from a snapshot. */
        if (!snapshot->present[card] || !memcard_is_present(card)) continue;
        for (int sector = 0; sector < MEMCARD_SECTORS; ++sector) {
            if (memcard_write_sector(card, sector,
                    snapshot->data[card] + sector * MEMCARD_SECTOR_SIZE) != 0)
                return 0;
        }
    }

    if (options && options->flush_after_rewind) {
        for (int card = 0; card < (int)DC_MEMCARD_SNAPSHOT_CARDS; ++card) {
            if (!snapshot->present[card] || !memcard_is_present(card)) continue;
            memcard_flush(card);
            if (memcard_last_flush_result(card) != 0) return 0;
        }
    }
    return 1;
}
