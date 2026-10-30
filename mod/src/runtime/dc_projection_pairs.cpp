#include "dc_projection_pairs.h"
#include <cstring>
#include <chrono>
#include <cstdlib>

extern "C" int g_dc_pass_id;

extern "C" { int g_dc_pair_track = 0; }
extern "C" { uint32_t g_dc_pair_last = 0; }
#define PR_RUNS 1024
struct PairRun { uint32_t key; int n, reaim; int32_t rt[9], tr[3]; float dn[12]; };
struct PairPass { int pass, nruns, nproj, over, part, lastg; PairRun r[PR_RUNS]; };
static PairPass s_prp[4] = {};
struct PairCount { int pass, n; };
static PairCount s_prc[4] = { { -1, 0 }, { -1, 0 }, { -1, 0 }, { -1, 0 } };
static inline void pair_count(void) {
    PairCount* c = &s_prc[g_dc_pass_id & 3];
    if (c->pass != g_dc_pass_id) { c->pass = g_dc_pass_id; c->n = 0; }
    c->n++;
}

#define PH_SIZE  131072u
#define PH_PROBE 16
struct PairHash { uint32_t fx, fy; int pass; uint32_t tag; };
static PairHash s_ph[PH_SIZE];
static inline uint32_t ph_slot(uint32_t a, uint32_t b) {
    uint32_t k = a * 2654435761u ^ (b * 2246822519u + 0x9E3779B9u);
    k ^= k >> 15; k *= 2246822519u; k ^= k >> 13;
    return k & (PH_SIZE - 1u);
}
static void ph_put(float fx, float fy, uint32_t tag) {
    if (!tag) return;
    uint32_t a, b;
    memcpy(&a, &fx, 4); memcpy(&b, &fy, 4);
    const uint32_t h = ph_slot(a, b);
    const int cur = g_dc_pass_id;
    PairHash* old = nullptr;
    for (uint32_t i = 0; i < PH_PROBE; i++) {
        PairHash* e = &s_ph[(h + i) & (PH_SIZE - 1u)];
        if (!e->tag || (e->fx == a && e->fy == b)) { e->fx = a; e->fy = b; e->pass = cur; e->tag = tag; return; }
        if (!old && cur - e->pass > 2) old = e;
    }
    if (!old) old = &s_ph[h];
    old->fx = a; old->fy = b; old->pass = cur; old->tag = tag;
}
extern "C" uint32_t pgxp_pair_of(float fx, float fy) {
    uint32_t a, b;
    memcpy(&a, &fx, 4); memcpy(&b, &fy, 4);
    const uint32_t h = ph_slot(a, b);
    for (uint32_t i = 0; i < PH_PROBE; i++) {
        const PairHash* e = &s_ph[(h + i) & (PH_SIZE - 1u)];
        if (!e->tag) return 0;
        if (e->fx == a && e->fy == b) return (g_dc_pass_id - e->pass <= 1) ? e->tag : 0u;
    }
    return 0;
}
static uint32_t pair_note(const int16_t rotation[9], const int32_t translation[3], const int16_t* V, int reaim, const float* Dn, const float* Tn) {
    PairPass* P = &s_prp[g_dc_pass_id & 3];
    if (P->pass != g_dc_pass_id) {
        P->pass = g_dc_pass_id; P->nruns = 0; P->nproj = 0; P->over = 0; P->lastg = -1;
        P->part = s_prc[g_dc_pass_id & 3].n > 1;
    }
    P->nproj++;
    int32_t rt[9], tr[3];
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) rt[i * 3 + j] = rotation[i * 3 + j];
        tr[i] = translation[i];
    }
    PairRun* r = (P->lastg >= 0 && P->lastg < P->nruns) ? &P->r[P->lastg] : nullptr;
    if (!r || r->reaim != reaim || memcmp(r->rt, rt, sizeof rt) || memcmp(r->tr, tr, sizeof tr) ||
        (reaim && (memcmp(r->dn, Dn, 9 * sizeof(float)) || memcmp(r->dn + 9, Tn, 3 * sizeof(float))))) {
        if (P->nruns >= PR_RUNS) { P->over = 1; return 0; }
        r = &P->r[P->nruns++];
        P->lastg = (int)(r - P->r);
        memcpy(r->rt, rt, sizeof rt); memcpy(r->tr, tr, sizeof tr); r->reaim = reaim;
        if (reaim) { memcpy(r->dn, Dn, 9 * sizeof(float)); memcpy(r->dn + 9, Tn, 3 * sizeof(float)); }
        r->n = 0; r->key = 2166136261u;
    }
    if (r->n < 3) for (int i = 0; i < 3; i++) r->key = (r->key ^ (uint32_t)(uint16_t)V[i]) * 16777619u;
    r->n++;
    return ((uint32_t)(g_dc_pass_id & 3) << 13) | (uint32_t)((r - P->r) + 1);
}

extern "C" uint32_t dc_pair_run_add(int pass, uint32_t key, const float M[9], const float t[3]) {
    if (!g_dc_pair_track || pass > g_dc_pass_id || g_dc_pass_id - pass > 1) return 0;
    PairPass* P = &s_prp[pass & 3];
    if (P->pass != pass) {
        if (pass != g_dc_pass_id) return 0;
        const PairCount* c = &s_prc[pass & 3];
        P->pass = pass; P->nruns = 0; P->nproj = 0; P->over = 0; P->lastg = -1;
        P->part = c->pass == pass && c->n > 0;
    }
    if (P->nruns >= PR_RUNS) { P->over = 1; return 0; }
    PairRun* r = &P->r[P->nruns++];
    for (int i = 0; i < 9; i++) r->rt[i] = (i % 4 == 0) ? 4096 : 0;
    for (int i = 0; i < 3; i++) r->tr[i] = 0;
    r->reaim = 2;
    memcpy(r->dn, M, 9 * sizeof(float)); memcpy(r->dn + 9, t, 3 * sizeof(float));
    r->n = 0; r->key = key;
    return ((uint32_t)(pass & 3) << 13) | (uint32_t)((r - P->r) + 1);
}

static int s_pair_veto[4] = { -1, -1, -1, -1 };
extern "C" void dc_pair_veto(int passA) { s_pair_veto[passA & 3] = passA; }
extern "C" int dc_pair_vetoed(int passA) { return s_pair_veto[passA & 3] == passA; }

extern "C" int dc_pair_pass_get(int pass, int* nproj) {
    const PairPass* P = &s_prp[pass & 3];
    const PairCount* c = &s_prc[pass & 3];
    if (P->pass != pass || P->part || P->over || c->pass != pass || c->n != P->nproj) return -1;
    if (nproj) *nproj = P->nproj;
    return P->nruns;
}

extern "C" int dc_pair_run_get(int pass, int i, uint32_t* key, int* n, double M[9], double t[3]) {
    const PairPass* P = &s_prp[pass & 3];
    if (P->pass != pass || i < 0 || i >= P->nruns) return 0;
    const PairRun* r = &P->r[i];
    if (key) *key = r->key;
    if (n) *n = r->n;
    double R[9], T[3];
    for (int k = 0; k < 9; k++) R[k] = (double)r->rt[k] / 4096.0;
    for (int k = 0; k < 3; k++) T[k] = (double)r->tr[k];
    if (r->reaim) {
        const float* D = r->dn;
        for (int a = 0; a < 3; a++) {
            for (int b = 0; b < 3; b++)
                M[a * 3 + b] = (double)D[a * 3] * R[b] + (double)D[a * 3 + 1] * R[3 + b] + (double)D[a * 3 + 2] * R[6 + b];
            t[a] = (double)D[a * 3] * T[0] + (double)D[a * 3 + 1] * T[1] + (double)D[a * 3 + 2] * T[2] + (double)D[9 + a];
        }
    } else {
        memcpy(M, R, sizeof R); memcpy(t, T, sizeof T);
    }
    return 1;
}

static void pair_probe_note(void);

uint32_t dc_pair_projection_note(const int16_t rotation[9], const int32_t translation[3],
    const int16_t vertex[3], int reaim, const float delta_r[9], const float delta_t[3])
{
    pair_probe_note();
    pair_count();
    g_dc_pair_last = g_dc_pair_track ? pair_note(rotation, translation, vertex, reaim, delta_r, delta_t) : 0u;
    return g_dc_pair_last;
}

void dc_pair_projection_tag(uint32_t tag, float x, float y) { ph_put(x, y, tag); }

struct PairProbe { int pass = -1, count = 0; double first = 0, last = 0; };
static PairProbe s_probe[4];
extern "C" double dc_pairprobe_now(void)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
static void pair_probe_note(void)
{
    static const bool enabled = [] { const char *value = std::getenv("DC_PAIRPROBE"); return value && *value == '1'; }();
    if (!enabled) return;
    PairProbe &probe = s_probe[g_dc_pass_id & 3];
    const double now = dc_pairprobe_now();
    if (probe.pass != g_dc_pass_id) { probe.pass = g_dc_pass_id; probe.count = 0; probe.first = now; }
    ++probe.count; probe.last = now;
}
extern "C" int dc_pairprobe_get(int pass, double *first, double *last)
{
    const PairProbe &probe = s_probe[pass & 3];
    if (probe.pass != pass) return -1;
    if (first) *first = probe.first;
    if (last) *last = probe.last;
    return probe.count;
}

void dc_pair_projection_reset(void)
{
    std::memset(s_prp, 0, sizeof s_prp);
    std::memset(s_ph, 0, sizeof s_ph);
    for (int i = 0; i < 4; ++i) {
        s_prp[i].pass = -1;
        s_prc[i].pass = -1; s_prc[i].n = 0;
        s_pair_veto[i] = -1;
        s_probe[i] = PairProbe{};
    }
    g_dc_pair_last = 0;
}
