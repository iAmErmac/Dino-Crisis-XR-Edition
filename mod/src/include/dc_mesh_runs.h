#ifndef DC_MESH_RUNS_H
#define DC_MESH_RUNS_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct DcMeshRuns {
    uint32_t base[2], count[2];
    uint64_t cut[2];
} DcMeshRuns;
void dc_meshrep_attach_runs(uint32_t header, DcMeshRuns *runs);
#ifdef __cplusplus
}
#endif
#endif
