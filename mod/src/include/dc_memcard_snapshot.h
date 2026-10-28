#ifndef DC_MEMCARD_SNAPSHOT_H
#define DC_MEMCARD_SNAPSHOT_H

#include <stddef.h>
#include <stdint.h>

#include "memcard.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DC_MEMCARD_SNAPSHOT_MAGIC   0x53434D44u
#define DC_MEMCARD_SNAPSHOT_VERSION 1u
#define DC_MEMCARD_SNAPSHOT_CARDS   2u
#define DC_MEMCARD_SNAPSHOT_HEADER_BYTES 16u
#define DC_MEMCARD_SNAPSHOT_RECORD_BYTES (4u + MEMCARD_SIZE)
#define DC_MEMCARD_SNAPSHOT_WIRE_BYTES \
    (DC_MEMCARD_SNAPSHOT_HEADER_BYTES + \
     DC_MEMCARD_SNAPSHOT_CARDS * DC_MEMCARD_SNAPSHOT_RECORD_BYTES)

typedef struct {
    uint8_t present[DC_MEMCARD_SNAPSHOT_CARDS];
    uint8_t dirty[DC_MEMCARD_SNAPSHOT_CARDS];
    uint8_t data[DC_MEMCARD_SNAPSHOT_CARDS][MEMCARD_SIZE];
} DcMemcardSnapshot;

typedef enum {
    DC_MEMCARD_RESTORE_KEEP_DISK = 0,
    DC_MEMCARD_RESTORE_REWIND = 1
} DcMemcardRestoreMode;

typedef struct {
    int flush_after_rewind;
} DcMemcardRestoreOptions;

size_t dc_memcard_snapshot_wire_bytes(void);
int dc_memcard_snapshot_capture(DcMemcardSnapshot *out);
int dc_memcard_snapshot_state_validate(const DcMemcardSnapshot *snapshot);
int dc_memcard_snapshot_write(const DcMemcardSnapshot *snapshot,
                              uint8_t *wire, size_t capacity);
int dc_memcard_snapshot_read(const uint8_t *wire, size_t size,
                             DcMemcardSnapshot *out);
int dc_memcard_snapshot_validate(const uint8_t *wire, size_t size);

/* KEEP_DISK validates and consumes the card section without touching cards.
 * REWIND applies saved images only to cards currently present at their
 * existing bindings and marks those cards dirty; optional flushing uses
 * memcard's checked atomic publisher and status. */
int dc_memcard_snapshot_restore(const DcMemcardSnapshot *snapshot,
                                DcMemcardRestoreMode mode,
                                const DcMemcardRestoreOptions *options);

#ifdef __cplusplus
}
#endif

#endif
