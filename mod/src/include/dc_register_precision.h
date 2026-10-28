#ifndef DC_REGISTER_PRECISION_H
#define DC_REGISTER_PRECISION_H
#include "pgxp_cells.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct PsxRegisterShadow {
    uint32_t value, live, scalar_live;
    int32_t x16, y16;
    uint16_t z, flags;
    float projection[4];
    double scalar;
} PsxRegisterShadow;
typedef struct PsxRegisterPrecision {
    uint32_t generation;
    PsxRegisterShadow reg[34];
} PsxRegisterPrecision;
typedef struct DcAddressPrecision {
    uint32_t generation, value[32], source[32];
    PxCell reg[32];
} DcAddressPrecision;

void psx_pgxp_capture_registers(const uint32_t words[34], PsxRegisterPrecision *state);
int psx_pgxp_restore_registers(const uint32_t words[34], const PsxRegisterPrecision *state);
void dc_pgxp_capture_registers(const uint32_t words[34], DcAddressPrecision *state);
void dc_pgxp_restore_registers(const uint32_t words[34], const DcAddressPrecision *state);
#ifdef __cplusplus
}
#endif
#endif
