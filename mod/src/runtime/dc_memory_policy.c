#include "dc_memory_policy.h"
#include "cpu_state.h"
#include "dc_shot.h"
#include "psx_memory.h"
#include "dc_pak.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#define RAM_SIZE 0x200000u
#define PSX_HOT_INLINE static inline
static uint8_t *ram, *scratchpad;
extern void psx_fatal_halt(const char *reason);
static _Noreturn void policy_fail(const char *reason) { psx_fatal_halt(reason); abort(); }
float g_dc_cam_eye[3], g_dc_cam_tgt[3];
int dc_coll_count(void);
int dc_cut_camera(void);
int dc_room_current(void);
int dc_scripted_now(void);
int dc_scripted_edge(void);
extern uint8_t *memory_get_ram_ptr(void);
extern uint8_t *memory_get_scratchpad_ptr(void);
extern uint32_t psx_read_word(uint32_t);
extern void psx_write_word(uint32_t,uint32_t);
extern void psx_write_half(uint32_t,uint16_t);
extern void psx_write_byte(uint32_t,uint8_t);
void dc_memory_policy_bind(void) {
    ram = memory_get_ram_ptr(); scratchpad = memory_get_scratchpad_ptr();
    if (!ram || !scratchpad) policy_fail("memory: missing backing storage");
    if (memory_get_ram_bytes() != RAM_SIZE) policy_fail("memory: game requires 2 MiB console RAM");
}
extern void dc_ots_last_yaw_write(int* yaw, int* age);
extern void debug_server_trace_write_check(uint32_t phys, uint32_t old_val,
                                           uint32_t new_val, uint8_t width);
extern void debug_server_trace_mmio_write(uint32_t addr, uint32_t val, uint8_t width);
extern CPUState *debug_cpu_ptr;
extern uint32_t g_debug_last_store_pc;

extern int card_data_writes_check(uint32_t phys, uint32_t value, uint8_t width);

extern int g_wtrace_any;
extern volatile int g_card_data_armed;
#define DC_AMMO_LO 0xB94CEu
#define DC_AMMO_HI 0xB94D7u

static inline int dc_ammo_range(uint32_t phys, uint32_t w) {
    return (phys + w > DC_AMMO_LO) && (phys <= DC_AMMO_HI);
}

static inline int dc_drawlist_on(void) {
    static int en = -1;
    if (en < 0) { const char *e = getenv("DC_DRAWLIST_WATCH"); en = (e && *e == '1') ? 1 : 0; }
    return en;
}

static uint32_t s_secvis_lo = 0;
static uint8_t  s_secvis_game[36];
static uint8_t  s_secvis_have[36];
static int      s_secvis_loads = -1;

static uint32_t s_svh_tick = 1;
static uint32_t s_svh_last[36]; static uint16_t s_svh_run[36]; static uint32_t s_svh_grp[36];
static inline void dc_secvis_watch(uint32_t phys, uint32_t val, uint32_t n) {
    if (!s_secvis_lo || phys + n <= s_secvis_lo || phys >= s_secvis_lo + 36u * 136u) return;
    for (uint32_t b = 0; b < n; b++) {
        uint32_t a = phys + b;
        if (a < s_secvis_lo) continue;
        uint32_t off = a - s_secvis_lo;
        if (off % 136u != 48u) continue;
        uint32_t k = off / 136u;
        if (k < 36u) { s_secvis_game[k] = (uint8_t)(val >> (8u * b)); s_secvis_have[k] = 1;
                       if (s_svh_last[k] != s_svh_tick) {
                           s_svh_run[k] = (s_svh_last[k] + 1u == s_svh_tick && s_svh_run[k] < 60000u) ? (uint16_t)(s_svh_run[k] + 1u) : 1u;
                           s_svh_last[k] = s_svh_tick; } }
    }
}

static int union_handler_driven(int k, uint32_t gp) {
    static int en = -1, hrun = 8;
    if (en < 0) { const char *e = getenv("DC_UNION_HANDLER"); en = (e && *e == '0') ? 0 : 1;
                  e = getenv("DC_UNION_HANDLER_RUN"); if (e) hrun = atoi(e); if (hrun < 2) hrun = 2; }
    if (!en || k < 0 || k >= 36 || gp < 0x100000u) return 0;
    if (s_svh_grp[k] && s_svh_grp[k] != gp) s_svh_grp[k] = 0;
    if (!s_svh_grp[k] && s_svh_run[k] >= (uint16_t)hrun && s_svh_tick - s_svh_last[k] <= 1u) {
        s_svh_grp[k] = gp;
        fprintf(stderr, "fullroom: slot %d grp %06X - the game rewrites its visibility every frame (%u frames running): "
                        "an object handler, not the shot cull; left to the game%c", k, gp, (unsigned)s_svh_run[k], 10);
    }
    return s_svh_grp[k] == gp;
}

int dc_secvis_forced(int k) {
    extern int g_savestate_loads;
    if (s_secvis_loads != g_savestate_loads) {
        s_secvis_loads = g_savestate_loads; memset(s_secvis_have, 0, sizeof s_secvis_have);
    }
    if (!s_secvis_lo || k < 0 || k >= 36 || !s_secvis_have[k]) return 0;
    uint32_t fa = s_secvis_lo + 136u * (uint32_t)k + 48u;
    if (fa >= RAM_SIZE) return 0;
    return ((ram[fa] & 2u) && !(s_secvis_game[k] & 2u)) ? 1 : 0;
}

static void mat3_mul(const float a[9], const float b[9], float o[9]);
static void mat3_vec(const float m[9], const float v[3], float o[3]);
static void v3cross(const float a[3], const float b[3], float o[3]);
static void v3norm(float v[3]);

uint64_t s_frame_count_ref(void){ extern uint64_t s_frame_count; return s_frame_count; }

static float s_nots_domR[9]; static float s_nots_domT[3]; static int s_nots_domhave = 0;
static int   s_nots_pair_gen = 0;
static int   s_nots_pairskip = 0;

int dc_nots_pairskip_peek(void) { return s_nots_pairskip; }

#define PAIRSET_MAX 6
long g_cut_event = 0;

static uint64_t s_pairset_f = ~0ull;
static float s_pairset_prevR[PAIRSET_MAX][9], s_pairset_prevT[PAIRSET_MAX][3];
static float s_pairset_curR[PAIRSET_MAX][9],  s_pairset_curT[PAIRSET_MAX][3];
static int   s_pairset_prevn = 0, s_pairset_curn = 0;
static void camdbg_pair(const float R9[9], const float T3[3], const char *what) {
    static int en = -1;
    if (en < 0) { const char *e = getenv("DC_CAMDBG"); en = (e && *e == '1') ? 1 : 0; }
    if (!en) return;
    extern uint64_t s_frame_count;
    fprintf(stderr, "cam f=%llu %-10s R0=%.3f,%.3f,%.3f T=%.0f,%.0f,%.0f%c",
            (unsigned long long)s_frame_count, what,
            R9[0], R9[1], R9[2], T3[0], T3[1], T3[2], 10);
}

static void nots_cutlock_rebase(const float Rn[9]);

#define PAIRHIST 12
static float    s_ph_R[PAIRHIST][9], s_ph_T[PAIRHIST][3];
static uint64_t s_ph_f[PAIRHIST];
static int      s_ph_n = 0, s_ph_next = 0;
static int pair_close(const float *Ra, const float *Ta, const float *Rb, const float *Tb) {
    float dr = 0.f, dt = 0.f;
    for (int i = 0; i < 9; i++) { float e = Ra[i] - Rb[i]; if (e < 0) e = -e; if (e > dr) dr = e; }
    for (int i = 0; i < 3; i++) { float e = Ta[i] - Tb[i]; if (e < 0) e = -e; if (e > dt) dt = e; }
    return dr <= 0.10f && dt <= 300.f;
}
static int cut_window(void) {

    static int win = -1;
    if (win < 0) { const char *e = getenv("DC_CUT_WINDOW"); win = e ? atoi(e) : 1; if (win < 1) win = 1; }
    return win;
}
static int pair_recent(const float R9[9], const float T3[3]) {
    extern uint64_t s_frame_count;
    const int win = cut_window();
    if (win <= 1) return 0;
    for (int k = 0; k < s_ph_n; k++)
        if (s_frame_count - s_ph_f[k] <= (uint64_t)win && pair_close(R9, T3, s_ph_R[k], s_ph_T[k])) return 1;
    return 0;
}
static void pair_recent_note(const float R9[9], const float T3[3]) {
    extern uint64_t s_frame_count;
    int k = 0;
    for (; k < s_ph_n; k++) if (pair_close(R9, T3, s_ph_R[k], s_ph_T[k])) break;
    if (k == s_ph_n) {
        if (s_ph_n < PAIRHIST) k = s_ph_n++;
        else { k = s_ph_next; s_ph_next = (s_ph_next + 1) % PAIRHIST; }
    }
    for (int i = 0; i < 9; i++) s_ph_R[k][i] = R9[i];
    for (int i = 0; i < 3; i++) s_ph_T[k][i] = T3[i];
    s_ph_f[k] = s_frame_count;
}

void dc_nots_set_pair(const float R9[9], const float T3[3]) {
    camdbg_pair(R9, T3, "publish");

    float n0=R9[0]*R9[0]+R9[1]*R9[1]+R9[2]*R9[2];
    float n1=R9[3]*R9[3]+R9[4]*R9[4]+R9[5]*R9[5];
    if (n0<0.85f || n0>1.15f || n1<0.85f || n1>1.15f) return;

    { static float tmax = -1.f;
      if (tmax < 0.f) { const char *e = getenv("DC_PAIR_TMAX"); tmax = e ? (float)atof(e) : 2000000.f; if (tmax < 0.f) tmax = 0.f; }
      if (tmax > 0.f && (fabsf(T3[0]) > tmax || fabsf(T3[1]) > tmax || fabsf(T3[2]) > tmax)) { camdbg_pair(R9, T3, "  notcam"); return; } }
    const int recent = pair_recent(R9, T3);
    pair_recent_note(R9, T3);
    if (s_nots_domhave) {

        float d = 0.f;
        for (int i = 0; i < 9; i++) { float e = R9[i]-s_nots_domR[i]; if (e<0) e=-e; if (e>d) d=e; }
        float dt = 0.f;
        for (int i = 0; i < 3; i++) { float e = T3[i]-s_nots_domT[i]; if (e<0) e=-e; if (e>dt) dt=e; }

        if (d < 0.0001f && dt < 0.5f) { camdbg_pair(R9, T3, "  same"); return; }

        int seen_last_frame = 0;
        {
            static int sec = -1;
            if (sec < 0) { const char *e = getenv("DC_PAIR_SECTIONS"); sec = (e && *e == '0') ? 0 : 1; }
            extern uint64_t s_frame_count;
            if (sec) {
                if (s_pairset_f != s_frame_count) {
                    s_pairset_f = s_frame_count;
                    for (int k = 0; k < s_pairset_curn; k++) {
                        for (int i = 0; i < 9; i++) s_pairset_prevR[k][i] = s_pairset_curR[k][i];
                        for (int i = 0; i < 3; i++) s_pairset_prevT[k][i] = s_pairset_curT[k][i];
                    }
                    s_pairset_prevn = s_pairset_curn; s_pairset_curn = 0;
                }
                for (int k = 0; k < s_pairset_prevn && !seen_last_frame; k++) {
                    float dr = 0.f, dtt = 0.f;
                    for (int i = 0; i < 9; i++) { float e = R9[i]-s_pairset_prevR[k][i]; if (e<0) e=-e; if (e>dr) dr=e; }
                    for (int i = 0; i < 3; i++) { float e = T3[i]-s_pairset_prevT[k][i]; if (e<0) e=-e; if (e>dtt) dtt=e; }
                    if (dr <= 0.10f && dtt <= 300.f) seen_last_frame = 1;
                }
                if (s_pairset_curn < PAIRSET_MAX) {
                    for (int i = 0; i < 9; i++) s_pairset_curR[s_pairset_curn][i] = R9[i];
                    for (int i = 0; i < 3; i++) s_pairset_curT[s_pairset_curn][i] = T3[i];
                    s_pairset_curn++;
                }
            }
        }
        if ((d > 0.10f || dt > 300.f) && !seen_last_frame && !recent) {
            g_cut_event++;
            camdbg_pair(R9, T3, "  CUT");

            { static int cps = -1;
              if (cps < 0) { const char *ecp = getenv("DC_CUT_PAIRSKIP"); cps = (ecp && *ecp == '1') ? 1 : 0; }
              if (cps && !dc_scripted_now()) s_nots_pairskip = 1; }

            nots_cutlock_rebase(R9);
        }
    }
    for (int i = 0; i < 9; i++) s_nots_domR[i] = R9[i];
    for (int i = 0; i < 3; i++) s_nots_domT[i] = T3[i];
    s_nots_domhave = 1;
    s_nots_pair_gen++;
}

int dc_nots_get_pairf(float R9[9], float T3[3]) {
    if (!s_nots_domhave) return 0;
    for (int i = 0; i < 9; i++) R9[i] = s_nots_domR[i];
    for (int i = 0; i < 3; i++) T3[i] = s_nots_domT[i];
    return 1;
}

int dc_nots_pairsrc_site(void) {
    static int v = -1;
    if (v < 0) {
        const char* e = getenv("DC_NOTS_PAIRSRC");

        v = (e && *e == 'h') ? 0 : 1;
    }
    return v;
}

static float s_nots_pR[9]; static float s_nots_pT[3]; static int s_nots_phave = 0;
void dc_nots_rtps_tick(void) {
    static int en=-1; if(en<0){ const char*e=getenv("DC_NATIVE_OTS"); en=(e&&*e=='1')?1:0; }
    if (!en) return;
    extern uint64_t s_frame_count;
    static uint64_t lf = ~0ull;
    if (s_frame_count == lf) return;
    lf = s_frame_count;
    float R[9];
    for(int i=0;i<9;i++) R[i]=(float)(int16_t)(ram[0xBF9F4+i*2]|(ram[0xBF9F4+i*2+1]<<8))/4096.0f;
    float l0=R[0]*R[0]+R[1]*R[1]+R[2]*R[2];
    if (l0<0.85f || l0>1.15f) return;
    for(int i=0;i<9;i++) s_nots_pR[i]=R[i];
    s_nots_pT[0]=(float)(int32_t)((uint32_t)ram[0xBFA08]|((uint32_t)ram[0xBFA09]<<8)|((uint32_t)ram[0xBFA0A]<<16)|((uint32_t)ram[0xBFA0B]<<24));
    s_nots_pT[1]=(float)(int32_t)((uint32_t)ram[0xBFA0C]|((uint32_t)ram[0xBFA0D]<<8)|((uint32_t)ram[0xBFA0E]<<16)|((uint32_t)ram[0xBFA0F]<<24));
    s_nots_pT[2]=(float)(int32_t)((uint32_t)ram[0xBFA10]|((uint32_t)ram[0xBFA11]<<8)|((uint32_t)ram[0xBFA12]<<16)|((uint32_t)ram[0xBFA13]<<24));
    s_nots_phave = 1;
}

static float s_fc_R[9], s_fc_T[3]; static int s_fc_have = 0, s_fc_bad = 0;
static const float *fixcam_pair(const float **T) {
    static int src = -1;
    if (src < 0) { const char *e = getenv("DC_FIXCAM_PAIR"); src = e ? atoi(e) : 1; }
    if (src == 1) {
        const uint32_t ctx = ((uint32_t)scratchpad[0] | ((uint32_t)scratchpad[1] << 8) | ((uint32_t)scratchpad[2] << 16) |
                              ((uint32_t)scratchpad[3] << 24)) & 0x1FFFFFu;
        if (ctx >= 0x1000u && ctx + 0xB4u + 0x40u < 0x200000u) {
            const uint8_t *c = ram + ctx + 0xB4u;
            float R[9], tr[3], eye[3], tg[3];
            for (int i = 0; i < 9; i++) R[i] = (float)(int16_t)(c[i * 2] | (c[i * 2 + 1] << 8)) / 4096.0f;
            for (int i = 0; i < 3; i++) {
                tr[i]  = (float)(int32_t)((uint32_t)c[20 + i * 4] | ((uint32_t)c[21 + i * 4] << 8) |
                                          ((uint32_t)c[22 + i * 4] << 16) | ((uint32_t)c[23 + i * 4] << 24));
                eye[i] = (float)(int16_t)(c[0x20 + i * 2] | (c[0x21 + i * 2] << 8));
                tg[i]  = (float)(int16_t)(c[0x30 + i * 2] | (c[0x31 + i * 2] << 8));
            }
            const float d0 = tg[0] - eye[0], d1 = tg[1] - eye[1], d2 = tg[2] - eye[2];
            const float dl = sqrtf(d0 * d0 + d1 * d1 + d2 * d2), fl = sqrtf(R[6] * R[6] + R[7] * R[7] + R[8] * R[8]);
            const int ok = dl > 100.f && fl > 0.9f && fl < 1.1f && (R[6] * d0 + R[7] * d1 + R[8] * d2) > 0.995f * dl * fl;
            if (ok || ++s_fc_bad > 8) {
                for (int i = 0; i < 9; i++) s_fc_R[i] = R[i];
                for (int k = 0; k < 3; k++) s_fc_T[k] = -(R[k * 3] * tg[0] + R[k * 3 + 1] * tg[1] + R[k * 3 + 2] * tg[2]) + tr[k];
                s_fc_have = 1;
                if (ok) s_fc_bad = 0;
            }
            if (s_fc_have) { *T = s_fc_T; return s_fc_R; }
        }
    }
    if (src == 2 && s_nots_domhave) { *T = s_nots_domT; return s_nots_domR; }
    if (s_nots_phave) { *T = s_nots_pT; return s_nots_pR; }
    return NULL;
}

int dc_fixed_cam_get(float R9[9], float T3[3]) {
    const float *T; const float *R = fixcam_pair(&T);
    if (!R) return 0;
    for (int i = 0; i < 9; i++) R9[i] = R[i];
    for (int i = 0; i < 3; i++) T3[i] = T[i];
    return 1;
}
int dc_fixed_cam_heading(float *h) {
    const float *T0; const float *R = fixcam_pair(&T0);
    if (!R) return 0;
    const float fx = R[6] - R[3], fz = R[8] - R[5];
    if (fx * fx + fz * fz < 1e-4f) return 0;
    *h = atan2f(fx, fz);
    return 1;
}

int dc_fixed_cam_project(float x, float y, float z, float *sx, float *sy) {
    const float *T; const float *R = fixcam_pair(&T);
    if (!R) return 0;
    const float vx = R[0] * x + R[1] * y + R[2] * z + T[0];
    const float vy = R[3] * x + R[4] * y + R[5] * z + T[1];
    const float vz = R[6] * x + R[7] * y + R[8] * z + T[2];
    if (vz < 1.f) return 0;
    *sx = vx / vz; *sy = vy / vz;
    return 1;
}

static float dc_items_ground_y(float x, float y, float z, int* found);
static float dc_room_lowest_y(int* ntri);
static uint32_t dc_camfb_gen(void);

#define ITEMOV_MAX 256
static struct { uint32_t hash; int16_t ax, az; int dy_up; int flat; int ry_off; int rx_off, rz_off; } s_itemov[ITEMOV_MAX];
static int s_itemov_n = 0, s_itemov_loaded = 0;
static void itemov_save(void);
static void itemov_save_atexit(void) { if (s_itemov_n > 0) itemov_save(); }
static void itemov_load(void) {
    if (s_itemov_loaded) return;
    s_itemov_loaded = 1;

    (void)itemov_save_atexit;
    DcMemFile* f = dc_mf_open("dc_items_user.cfg");
    if (!f) return;
    char line[128];
    while (dc_mf_gets(line, sizeof line, f) && s_itemov_n < ITEMOV_MAX) {
        unsigned int h; int ax, az, dy, fl, ro = 0, rxo = 0, rzo = 0;
        int got = sscanf(line, "%x,%d,%d,%d,%d,%d,%d,%d", &h, &ax, &az, &dy, &fl, &ro, &rxo, &rzo);
        if (got >= 5) {
            s_itemov[s_itemov_n].hash = h;
            s_itemov[s_itemov_n].ax = (int16_t)ax; s_itemov[s_itemov_n].az = (int16_t)az;
            s_itemov[s_itemov_n].dy_up = dy; s_itemov[s_itemov_n].flat = fl;
            s_itemov[s_itemov_n].ry_off = (got >= 6) ? ro : 0;
            s_itemov[s_itemov_n].rx_off = (got >= 7) ? rxo : 0;
            s_itemov[s_itemov_n].rz_off = (got >= 8) ? rzo : 0;
            s_itemov_n++;
        }
    }
    dc_mf_close(f);
    fprintf(stderr, "items: %d user overrides loaded\n", s_itemov_n);
}
static void itemov_save(void) {
    FILE* f = NULL;
    if (!f) return;
    fprintf(f, "# hash,authoredX,authoredZ,dy_up,flat(-1 auto),ry_off,tiltX,tiltZ\n");
    for (int i = 0; i < s_itemov_n; i++)
        fprintf(f, "%08X,%d,%d,%d,%d,%d,%d,%d\n", s_itemov[i].hash,
                s_itemov[i].ax, s_itemov[i].az, s_itemov[i].dy_up, s_itemov[i].flat,
                s_itemov[i].ry_off, s_itemov[i].rx_off, s_itemov[i].rz_off);
    fclose(f);
}

static const struct { uint32_t hash; int dy_up; } s_item_model_def[] = {
    { 0x60BCE682u, -150 },
};
static int itemov_model_dy(uint32_t hash) {
    for (unsigned i = 0; i < sizeof(s_item_model_def)/sizeof(s_item_model_def[0]); i++)
        if (s_item_model_def[i].hash == hash) return s_item_model_def[i].dy_up;
    return 0;
}
static int itemov_find(uint32_t hash, int16_t x, int16_t z, int make) {
    for (int i = 0; i < s_itemov_n; i++)
        if (s_itemov[i].hash == hash &&
            (int)(s_itemov[i].ax - x) * (s_itemov[i].ax - x) +
            (int)(s_itemov[i].az - z) * (s_itemov[i].az - z) <= 96 * 96) return i;
    if (!make || s_itemov_n >= ITEMOV_MAX) return -1;
    s_itemov[s_itemov_n].hash = hash; s_itemov[s_itemov_n].ax = x; s_itemov[s_itemov_n].az = z;
    s_itemov[s_itemov_n].dy_up = 0; s_itemov[s_itemov_n].flat = -1; s_itemov[s_itemov_n].ry_off = 0;
    s_itemov[s_itemov_n].rx_off = 0; s_itemov[s_itemov_n].rz_off = 0;
    return s_itemov_n++;
}

static int16_t s_it_keep[36]; static uint8_t s_it_have[36];
static int16_t s_it_pvx[36], s_it_pvy[36], s_it_pvz[36]; static uint8_t s_it_stab[36];
static int16_t s_it_gy[36]; static uint8_t s_it_gnd[36], s_it_flat[36];
static uint32_t s_it_hash[36]; static int16_t s_it_ax[36], s_it_az[36], s_it_rx0[36], s_it_rz0[36];
static int16_t s_it_ay[36];
static uint32_t s_it_gen[36];

static int s_its_init = 0, s_its_en = 1, s_its_angle = -1, s_its_drop = 0, s_its_ground = 1, s_its_lift = 8, s_its_flaten = 1, s_its_flat_n = 0;
static const uint32_t *s_its_flat_hash = NULL;
#define IGC_MAX 16
static struct { uint32_t model; int16_t x, z, ay, gy; uint8_t gnd, done; uint32_t gen0, last_gen; } s_igc[IGC_MAX];
static int s_igc_n = 0; static uint32_t s_igc_key = 0;

static int igc_find_live(uint32_t mp, int16_t x, int16_t z) {
    mp &= 0x1FFFFFu;
    for (int i = 0; i < s_igc_n; i++)
        if (s_igc[i].done && s_igc[i].model == mp && s_igc[i].x == x && s_igc[i].z == z &&
            (uint32_t)(dc_camfb_gen() - s_igc[i].last_gen) < 60u) return i;
    return -1;
}

int dc_items_ghost_pose(uint32_t key, uint32_t model, int x, int z, int kind, int *oy, int *orx, int *ory, int *orz) {
    extern int g_dc_items_orig;
    if (!s_its_init || !s_its_en || g_dc_items_orig) return -1;
    uint8_t *r = memory_get_ram_ptr(); if (!r) return -1;
    const uint32_t ma = model & 0x1FFFFFu;
    if (ma <= 0x1000u || ma + 36u >= 0x200000u) return -1;
    if (key != s_igc_key) { s_igc_key = key; s_igc_n = 0; }
    const uint32_t gen = dc_camfb_gen();
    int c = -1;
    for (int i = 0; i < s_igc_n; i++) if (s_igc[i].model == ma && s_igc[i].x == (int16_t)x && s_igc[i].z == (int16_t)z) { c = i; break; }
    if (c < 0) {
        if (s_igc_n >= IGC_MAX) return -1;
        c = s_igc_n++;
        memset(&s_igc[c], 0, sizeof s_igc[c]);
        s_igc[c].model = ma; s_igc[c].x = (int16_t)x; s_igc[c].z = (int16_t)z; s_igc[c].gen0 = gen;
    }
    s_igc[c].last_gen = gen;
    if (!s_igc[c].done) {
        if ((uint32_t)(gen - s_igc[c].gen0) < 2u) return 0;
        s_igc[c].ay = (int16_t)s_its_drop; s_igc[c].gy = 0; s_igc[c].gnd = 0; s_igc[c].done = 1;
    }
    if (s_its_ground && !s_igc[c].gnd) {
        static float gmax = -1.f; static int us = -1;
        if (gmax < 0.f) { const char *e = getenv("DC_ITEMS_GROUND_MAX"); gmax = e ? (float)atof(e) : 200.f; }
        if (us < 0) { const char *e = getenv("DC_ITEMS_UNSINK_SAVE"); us = (e && *e == '0') ? 0 : 1; }
        int f = 0;
        float by = dc_items_ground_y((float)x, (float)s_igc[c].ay, (float)z, &f);
        if (f && by - (float)s_igc[c].ay > gmax) { f = 0; s_igc[c].gnd = 1; s_igc[c].gy = 0; }
        if (!f && !s_igc[c].gnd && us) {
            int f2 = 0;
            const float by2 = dc_items_ground_y((float)x, (float)s_igc[c].ay - 400.f, (float)z, &f2);
            if (f2 && by2 < (float)s_igc[c].ay - 1.f) { s_igc[c].ay = (int16_t)by2; by = by2; f = 1; }
        }
        if (f) { s_igc[c].gy = (int16_t)(by - (float)s_its_lift); s_igc[c].gnd = 1; }
    }
    uint32_t h = 2166136261u;
    for (int b = 12; b < 36; b++) { h ^= r[ma + (uint32_t)b]; h *= 16777619u; }
    int flat = 0;
    if (s_its_flaten && s_its_flat_hash)
        for (int k = 0; k < s_its_flat_n; k++) if (h == s_its_flat_hash[k]) { flat = 1; break; }
    const int ov = itemov_find(h, (int16_t)x, (int16_t)z, 0);
    if (ov >= 0 && s_itemov[ov].flat >= 0) flat = s_itemov[ov].flat;
    int keep = (s_its_angle >= 0) ? s_its_angle : 0;
    if (ov >= 0 && s_itemov[ov].ry_off) keep = (keep + s_itemov[ov].ry_off) & 0xFFF;
    const int yoff = (ov >= 0) ? s_itemov[ov].ry_off : 0, zoff = (ov >= 0) ? s_itemov[ov].rz_off : 0;
    const int xoff = (ov >= 0) ? s_itemov[ov].rx_off : 0;
    const int rx0 = 0, rz0 = (kind < 11) ? 3072 : 0;
    *ory = flat ? (zoff & 0xFFF) : keep;
    *orz = flat ? (yoff & 0xFFF) : (zoff ? ((rz0 + zoff) & 0xFFF) : rz0);
    *orx = flat ? ((1024 + xoff) & 0xFFF) : (xoff ? ((rx0 + xoff) & 0xFFF) : rx0);
    const int dyu = (ov >= 0) ? s_itemov[ov].dy_up : itemov_model_dy(h);
    *oy = (int)(int16_t)((s_igc[c].gnd ? s_igc[c].gy : s_igc[c].ay) - dyu);
    return 1;
}

static void items_restore_authored(void) {
    uint8_t *r = memory_get_ram_ptr(); uint8_t *sp = memory_get_scratchpad_ptr();
    if (!r || !sp) return;
    uint32_t gsp = ((uint32_t)sp[0] | ((uint32_t)sp[1] << 8) | ((uint32_t)sp[2] << 16) | ((uint32_t)sp[3] << 24)) & 0x1FFFFFu;
    if (gsp < 0x1000u || gsp > 0x1F0000u) return;
    uint32_t base = gsp + 31964u; int n = 0;
    for (int i = 0; i < 36; i++) {
        if (!s_it_have[i]) continue;
        uint32_t a = base + (uint32_t)i * 136u;
        if (a + 64u >= 0x200000u) break;
        int16_t x = (int16_t)(r[a+32] | (r[a+33] << 8)), z = (int16_t)(r[a+36] | (r[a+37] << 8));
        if ((r[a+48] & 1u) && (int8_t)r[a+46] == 2 && x == s_it_ax[i] && z == s_it_az[i]) {
            r[a+34] = (uint8_t)(s_it_ay[i] & 0xFF);  r[a+35] = (uint8_t)((s_it_ay[i] >> 8) & 0xFF);
            r[a+40] = (uint8_t)(s_it_rx0[i] & 0xFF); r[a+41] = (uint8_t)((s_it_rx0[i] >> 8) & 0xFF);
            r[a+44] = (uint8_t)(s_it_rz0[i] & 0xFF); r[a+45] = (uint8_t)((s_it_rz0[i] >> 8) & 0xFF);
            n++;
        }
        s_it_have[i] = 0; s_it_stab[i] = 0; s_it_gnd[i] = 0;
    }
    fprintf(stderr, "items: 'Original' look - %d items got their original height and tilt back, the game drives them from here\n", n);
}

static uint8_t s_ou_have[36], s_ou_stab[36];
static int16_t s_ou_x[36], s_ou_y[36], s_ou_z[36], s_ou_y0[36], s_ou_rx[36], s_ou_rz[36];
static int16_t s_ou_px[36], s_ou_py[36], s_ou_pz[36];
static int16_t it_rd16(const uint8_t *r, uint32_t a) { return (int16_t)(r[a] | (r[a+1] << 8)); }
static void it_wr16(uint8_t *r, uint32_t a, int v) { r[a] = (uint8_t)(v & 0xFF); r[a+1] = (uint8_t)((v >> 8) & 0xFF); }

static int item_model_yext(const uint8_t *r, uint32_t mp, int rx, int rz, float *lo, float *hi) {
    uint32_t ma = mp & 0x1FFFFFu;
    if (ma < 0x1000u || ma + 12u >= 0x200000u) return 0;
    uint32_t pb = ((uint32_t)r[ma+4] | ((uint32_t)r[ma+5] << 8) | ((uint32_t)r[ma+6] << 16) | ((uint32_t)r[ma+7] << 24)) & 0x1FFFFFu;
    int na = r[ma+8] | (r[ma+9] << 8), nb = r[ma+10] | (r[ma+11] << 8);
    if (na != 0 || nb <= 0 || nb > 512 || pb < 0x1000u || pb + (uint32_t)nb * 52u >= 0x200000u) return 0;
    const float k = 6.2831853f / 4096.f;
    float cz = cosf((float)rz * k), sz = sinf((float)rz * k), cx = cosf((float)rx * k), sx = fabsf(sinf((float)rx * k));
    float l = -1e9f, h = 1e9f;
    for (int q = 0; q < nb; q++) {
        const uint8_t *p = r + pb + (uint32_t)q * 52u;
        if ((p[39] & 0xFC) != 0x3C) return 0;
        for (int v = 0; v < 4; v++) {
            float x = (float)it_rd16(p, (uint32_t)v * 8u), y = (float)it_rd16(p, (uint32_t)v * 8u + 2u);
            float z = (float)it_rd16(p, (uint32_t)v * 8u + 4u);
            for (int sg = -1; sg <= 1; sg += 2) {
                float y1 = cz * y + (float)sg * sz * x, x1 = cz * x - (float)sg * sz * y;
                float rr = sqrtf(x1 * x1 + z * z) * sx;
                if (cx * y1 + rr > l) l = cx * y1 + rr;
                if (cx * y1 - rr < h) h = cx * y1 - rr;
            }
        }
    }
    *lo = l; *hi = h;
    return 1;
}
static void items_orig_unsink(void) {
    static int en = -1, dmax = 400, logi = 1;
    if (en < 0) {
        const char *e = getenv("DC_ITEMS_ORIG_UNSINK"); en = (e && *e == '0') ? 0 : 1;
        e = getenv("DC_ITEMS_ORIG_UNSINK_MAX"); if (e) dmax = atoi(e);
        e = getenv("DC_ITEMS_LOG"); logi = (e && *e == '0') ? 0 : 1;
    }
    if (!en) return;
    uint8_t *r = memory_get_ram_ptr(); uint8_t *sp = memory_get_scratchpad_ptr();
    if (!r || !sp) return;
    uint32_t gsp = ((uint32_t)sp[0] | ((uint32_t)sp[1] << 8) | ((uint32_t)sp[2] << 16) | ((uint32_t)sp[3] << 24)) & 0x1FFFFFu;
    if (gsp < 0x1000u || gsp > 0x1F0000u) return;
    extern int dc_cut_camera(void);
    int dc_camfb_count(void);
    int scene = dc_cut_camera();
    uint32_t base = gsp + 31964u;
    for (int i = 0; i < 36; i++) {
        uint32_t a = base + (uint32_t)i * 136u;
        if (a + 64u >= 0x200000u) break;
        if (!(r[a+48] & 1u) || (int8_t)r[a+46] != 2) { s_ou_have[i] = 0; s_ou_stab[i] = 0; continue; }
        int16_t x = it_rd16(r, a+32), y = it_rd16(r, a+34), z = it_rd16(r, a+36);
        int16_t rx = it_rd16(r, a+40), rz = it_rd16(r, a+44);
        if (s_ou_have[i]) {
            if (x != s_ou_x[i] || z != s_ou_z[i]) { s_ou_have[i] = 0; s_ou_stab[i] = 0; }
            else if (y != s_ou_y[i]) {
                if (y == s_ou_y0[i] && !scene) { it_wr16(r, a+34, s_ou_y[i]); continue; }
                s_ou_have[i] = 0; s_ou_stab[i] = 0;
            }
            else if (rx != s_ou_rx[i] || rz != s_ou_rz[i]) {
                if (s_ou_y0[i] != s_ou_y[i]) it_wr16(r, a+34, s_ou_y0[i]);
                s_ou_have[i] = 0; s_ou_stab[i] = 0; continue;
            } else continue;
        }
        if (scene) continue;
        if (x != s_ou_px[i] || y != s_ou_py[i] || z != s_ou_pz[i]) {
            s_ou_px[i] = x; s_ou_py[i] = y; s_ou_pz[i] = z; s_ou_stab[i] = 0; continue; }
        if (s_ou_stab[i] < 3) { s_ou_stab[i]++; continue; }
        float lo, hi;
        uint32_t mp = (uint32_t)r[a+60] | ((uint32_t)r[a+61] << 8) | ((uint32_t)r[a+62] << 16) | ((uint32_t)r[a+63] << 24);
        if (!item_model_yext(r, mp, rx, rz, &lo, &hi)) {
            if (logi) fprintf(stderr, "item: orig slot=%d model %08X not recognized - placed as the game places it\n", i, mp);
        } else {
            int f = 0;
            float by = dc_items_ground_y((float)x, (float)y + hi, (float)z, &f);
            if (!f && dc_camfb_count() == 0) continue;
            if (f && (float)y + lo > by + 1.f) {
                int ny = (int)floorf(by - lo) - 2;
                if ((float)y + hi >= by)
                    { if (logi) fprintf(stderr, "item: orig slot=%d entirely below the surface %.0f - left alone\n", i, by); }
                else if (y - ny > dmax)
                    { if (logi) fprintf(stderr, "item: orig slot=%d sunk by %d > %d - left alone\n", i, y - ny, dmax); }
                else {
                    it_wr16(r, a+34, ny);
                    if (logi) fprintf(stderr, "item: 'Original' look - slot=%d sunk by %.0f (model Y %.0f..%.0f, "
                                      "surface %.0f): Y %d -> %d\n", i, (float)y + lo - by, hi, lo, by, y, ny);
                    s_ou_have[i] = 1; s_ou_x[i] = x; s_ou_z[i] = z; s_ou_y0[i] = y; s_ou_y[i] = (int16_t)ny;
                    s_ou_rx[i] = rx; s_ou_rz[i] = rz;
                    continue;
                }
            }
        }
        s_ou_have[i] = 1; s_ou_x[i] = x; s_ou_z[i] = z; s_ou_y0[i] = y; s_ou_y[i] = y; s_ou_rx[i] = rx; s_ou_rz[i] = rz;
    }
}

static void items_orig_unsink_undo(void) {
    uint8_t *r = memory_get_ram_ptr(); uint8_t *sp = memory_get_scratchpad_ptr();
    int n = 0;
    if (r && sp) {
        uint32_t gsp = ((uint32_t)sp[0] | ((uint32_t)sp[1] << 8) | ((uint32_t)sp[2] << 16) | ((uint32_t)sp[3] << 24)) & 0x1FFFFFu;
        if (gsp >= 0x1000u && gsp <= 0x1F0000u)
            for (int i = 0; i < 36; i++) {
                uint32_t a = gsp + 31964u + (uint32_t)i * 136u;
                if (!s_ou_have[i] || s_ou_y0[i] == s_ou_y[i] || a + 64u >= 0x200000u) continue;
                if ((r[a+48] & 1u) && (int8_t)r[a+46] == 2 && it_rd16(r, a+32) == s_ou_x[i] &&
                    it_rd16(r, a+36) == s_ou_z[i] && it_rd16(r, a+34) == s_ou_y[i]) { it_wr16(r, a+34, s_ou_y0[i]); n++; }
            }
    }
    for (int i = 0; i < 36; i++) { s_ou_have[i] = 0; s_ou_stab[i] = 0; }
    if (n) fprintf(stderr, "items: 'Modern' look - %d raised items got the game's height back\n", n);
}
static int s_it_ping[36];

int g_dc_glass_3d = -1;
void dc_glass_tint_tick(void) {
    enum { G_ORIG = 0x92020006u, G_PATCH = 0x00001021u };
    static uint32_t site = 0; static int state = -1, last_room = -9999; static uint64_t next_scan = 0;
    extern uint64_t s_frame_count; extern int dc_room_effective(void);
    extern int g_dc_orig_mode; extern int dc_nots_active(void);
    if (g_dc_glass_3d < 0) { const char *e = getenv("DC_GLASS_3D"); g_dc_glass_3d = (e && *e == '0') ? 0 : 1; }
#define GW(a) ((uint32_t)ram[(a)] | ((uint32_t)ram[(a) + 1] << 8) | ((uint32_t)ram[(a) + 2] << 16) | ((uint32_t)ram[(a) + 3] << 24))
#define GSIG(a) ((GW(a) == G_ORIG || GW(a) == G_PATCH) && GW((a) + 8) == 0x10400009u && GW((a) + 12) == 0x3C0200F0u && \
                 GW((a) + 16) == 0x34420140u && GW((a) + 20) == 0x24030140u)
    const int room = dc_room_effective();
    if (room != last_room) { last_room = room; site = 0; next_scan = 0; }
    if (site && !GSIG(site)) site = 0;
    if (!site && s_frame_count >= next_scan) {
        next_scan = s_frame_count + 120;
        for (uint32_t a = 0x000B0000u; a + 24u < 0x00140000u; a += 4u)
            if (GSIG(a)) { site = a; break; }
        if (site) fprintf(stderr, "glass: the 'glass over the whole screen' object in room overlay %d - flag read at 0x%08X%c",
                          room, 0x80000000u | site, 10);
    }
    if (!site) return;
    const int want = g_dc_glass_3d && !g_dc_orig_mode && dc_nots_active();
    const uint32_t cur = GW(site), need = want ? G_PATCH : G_ORIG;
    if (cur != need) {
        ram[site] = (uint8_t)need; ram[site + 1] = (uint8_t)(need >> 8); ram[site + 2] = (uint8_t)(need >> 16); ram[site + 3] = (uint8_t)(need >> 24);
    }
    if (want != state) {
        state = want;
        fprintf(stderr, "glass: %s%c", want ? "our camera - the window glass stays in its place, not over the whole screen"
                                            : "as in the game - over the whole screen when its camera is behind the glass", 10);
    }
#undef GSIG
#undef GW
}

void dc_actor_fall_fix(void) {
    static int en = -1, voiden = 1, vkind = 0x12, hangen = 1;
    if (en < 0) { const char *e = getenv("DC_ACTOR_FALL_FIX"); en = (e && *e == '0') ? 0 : 1;
                  e = getenv("DC_ACTOR_FALL_VOID"); voiden = (e && *e == '0') ? 0 : 1;
                  e = getenv("DC_ACTOR_FALL_HANG"); hangen = (e && *e == '0') ? 0 : 1;
                  e = getenv("DC_ACTOR_FALL_KIND"); if (e && *e) vkind = (int)strtol(e, NULL, 0) & 0xFF; }
    if (!en) return;
    extern uint64_t gpu_flip_seq(void);
    static struct { int on; float g; uint64_t flip; } s_hang[10];
    static uint8_t s_void_w[10], s_void_off[10];
    uint8_t *r = memory_get_ram_ptr(); if (!r) return;
    if (r[0xB7878] != 3) return;

    #define AFF_W(ad) ((uint32_t)r[(ad)] | ((uint32_t)r[(ad) + 1] << 8) | ((uint32_t)r[(ad) + 2] << 16) | ((uint32_t)r[(ad) + 3] << 24))
    if (AFF_W(0xEC028u) != 0x03E00008u || AFF_W(0xEF2A0u) != 0x862301F4u || AFF_W(0xEF4C4u) != 0x2463B7DCu) return;
    #undef AFF_W

    int fan_skip = 0;
    {
        static int fan_en = -1;
        if (fan_en < 0) { const char *e = getenv("DC_FAN_DESPAWN"); fan_en = (e && *e == '0') ? 0 : 1; }
        const uint32_t a = 0xB7ACCu + 2u * 608u;
        extern int dc_cut_camera(void);
        if (fan_en && r[0xC12F0] == 7 && r[0xC12F1] == 4 && (r[a + 48] & 1u) && r[a + 80] == 0x12 &&
            (int16_t)(r[a + 280] | (r[a + 281] << 8)) <= 0 && r[0xB92C8] != 4 && !dc_cut_camera()) {
            #define FD_W(ad) ((uint32_t)r[(ad)] | ((uint32_t)r[(ad) + 1] << 8) | ((uint32_t)r[(ad) + 2] << 16) | ((uint32_t)r[(ad) + 3] << 24))
            const uint32_t b3 = FD_W(0x9CB48u + 3u * 8u) & 0x1FFFFFu;
            if (b3 > 0x1000u && b3 + 8u < 0x200000u && (r[b3 + (35u >> 3)] & (1u << (35 & 7)))) {
                static int said = 0;
                int ntri = 0;
                int deep = (int)dc_room_lowest_y(&ntri) + 6000;
                if (deep < 8000) deep = 8000;
                if (deep > 30000) deep = 30000;
                r[a + 48] &= (uint8_t)~2u;
                uint32_t pp = FD_W(a + 76u) & 0x1FFFFFu;
                for (int k = 0; k < 24 && pp > 0x1000u && pp + 56u < 0x200000u; k++) { r[pp + 48] &= (uint8_t)~2u; pp = FD_W(pp + 52u) & 0x1FFFFFu; }
                r[a + 34] = (uint8_t)(deep & 0xFF); r[a + 35] = (uint8_t)((deep >> 8) & 0xFF);
                for (int k = 64; k < 70; k++) r[a + k] = 0;
                r[a + 486] = 0; r[a + 487] = 0;
                fan_skip = 1;
                if (!said) { said = 1;
                    fprintf(stderr, "fandespawn: ST407 - the pteranodon killed by the fan (actor 2, state %d/%d/%d) is hidden under the room "
                                    "after the scene (DC_FAN_DESPAWN=0 off)\n", r[a + 60], r[a + 61], r[a + 62]); }
            }
            #undef FD_W
        }
    }
    for (int i = 0; i < 10; i++) {
        const uint32_t a = 0xB7ACCu + (uint32_t)i * 608u;
        if (fan_skip && i == 2) continue;
        if (!(r[a + 48] & 1u) || r[a + 60] != 2 || r[a + 61] != 2 || r[a + 62] != 3) {
            s_hang[i].on = 0; s_void_w[i] = 0; s_void_off[i] = 0;
            continue;
        }
        const int16_t vx = (int16_t)(r[a + 64] | (r[a + 65] << 8)), vy = (int16_t)(r[a + 66] | (r[a + 67] << 8));
        const int16_t vz = (int16_t)(r[a + 68] | (r[a + 69] << 8));
        const int x = (int16_t)(r[a + 32] | (r[a + 33] << 8)), y = (int16_t)(r[a + 34] | (r[a + 35] << 8));
        const int z = (int16_t)(r[a + 36] | (r[a + 37] << 8));
        if (voiden && r[a + 80] == (uint8_t)vkind) {
            int anyf = 0, ntri = 0;

            static int skyy = 1;
            if (skyy > 0) { const char *e = getenv("DC_ACTOR_FALL_SKY"); skyy = (e && *e) ? atoi(e) : -12000; if (skyy > 0) skyy = -12000; }
            (void)dc_items_ground_y((float)x, (float)(skyy + 40), (float)z, &anyf);
            const float low = dc_room_lowest_y(&ntri);
            if (!anyf && ntri >= 64) {
                int deep = (int)low + 6000;
                if (deep < 8000) deep = 8000;
                if (deep > 30000) deep = 30000;
                if (s_void_off[i]) continue;
                if (s_void_w[i] && y != deep && !vx && !vy && !vz) {
                    s_void_off[i] = 1;
                    fprintf(stderr, "actorfall: actor %d (0x%06X) - the game put the body back on its floor (Y %d, ours %d): its floor exists in this "
                                    "column, no longer moved under the room\n", i, (unsigned)(0x80000000u | a), y, deep);
                    continue;
                }
                if (y != deep || vx || vy || vz) {
                    s_void_w[i] = 1;
                    r[a + 34] = (uint8_t)(deep & 0xFF); r[a + 35] = (uint8_t)((deep >> 8) & 0xFF);
                    for (int k = 64; k < 70; k++) r[a + k] = 0;
                    r[a + 486] = 0; r[a + 487] = 0;
                    static int nlogv = 0;
                    if (nlogv < 20) { nlogv++;
                        fprintf(stderr, "actorfall: actor %d (0x%06X, kind %02X) in 2/2/3, speed %d,%d,%d, Y %d - column (%d,%d) has no "
                                        "surface (shaft/drop): moved under the room, Y %d (room bottom %.0f), stays (DC_ACTOR_FALL_VOID=0 - "
                                        "as before)\n", i, (unsigned)(0x80000000u | a), r[a + 80], vx, vy, vz, y, x, z, deep, low); }
                }
                continue;
            }
        }

        if (hangen && r[a + 80] == (uint8_t)vkind) {
            const int probe_h = y + 2309;
            if (!s_hang[i].on && !vx && !vy && !vz && !r[a + 486] && !r[a + 487]) {
                int fnd = 0, nt = 0;
                float g = dc_items_ground_y((float)x, (float)probe_h, (float)z, &fnd);
                if (!fnd) {
                    const float low = dc_room_lowest_y(&nt);
                    if (nt >= 64 && low > (float)probe_h && low - (float)probe_h <= 8000.f) { g = low; fnd = 2; }
                }
                if (fnd && g - (float)probe_h > 64.f) {
                    s_hang[i].on = 1; s_hang[i].g = g; s_hang[i].flip = gpu_flip_seq();
                    r[a + 66] = 15; r[a + 67] = 0;
                    fprintf(stderr, "actorfall: actor %d (0x%06X) in 2/2/3 hangs without speed (Y %d, +500 %d, animation frame %d) - to the "
                                    "%s %.0f still %.0f: falls by itself (DC_ACTOR_FALL_HANG=0 - as before)\n", i, (unsigned)(0x80000000u | a), y,
                            (int16_t)(r[a + 500] | (r[a + 501] << 8)), (int)(r[a + 96] | (r[a + 97] << 8)),
                            fnd == 2 ? "room bottom" : "surface", g, g - (float)probe_h);
                    continue;
                }
            }
            if (s_hang[i].on) {
                int vyy = vy;
                const uint64_t f = gpu_flip_seq();
                if (f != s_hang[i].flip) {
                    s_hang[i].flip = f;
                    vyy += 15; if (vyy > 400) vyy = 400; if (vyy < 15) vyy = 15;
                    r[a + 66] = (uint8_t)(vyy & 0xFF); r[a + 67] = (uint8_t)((vyy >> 8) & 0xFF);
                }
                if ((float)probe_h + (float)vyy + 16.f >= s_hang[i].g) {
                    const int ny = (int)s_hang[i].g - 2309;
                    if (ny >= -32768 && ny <= 32767) {
                        r[a + 34] = (uint8_t)(ny & 0xFF); r[a + 35] = (uint8_t)((ny >> 8) & 0xFF);
                        for (int k = 64; k < 70; k++) r[a + k] = 0;
                        r[a + 486] = 0; r[a + 487] = 0;
                        r[a + 60] = 2; r[a + 61] = 3; r[a + 62] = 0; r[a + 63] = 0;
                        fprintf(stderr, "actorfall: actor %d (0x%06X) landed: Y %d (surface %.0f), state 2/3/0 - hitting the ground and "
                                        "the death pose (HP %d)\n", i, (unsigned)(0x80000000u | a), ny, s_hang[i].g,
                                (int16_t)(r[a + 280] | (r[a + 281] << 8)));
                    }
                    s_hang[i].on = 0;
                }
                continue;
            }
        }
        if (!vx && !vy && !vz) continue;
        const int probe = y + 2309;
        int found = 0;
        const float g = dc_items_ground_y((float)x, (float)probe, (float)z, &found);
        if (!found) continue;
        const int step = (vy > 0 ? vy : -vy) + 16;
        if ((float)probe + (float)step < g) continue;
        const int ny = (int)g - 2309;
        if (ny < -32768 || ny > 32767) continue;
        r[a + 34] = (uint8_t)(ny & 0xFF); r[a + 35] = (uint8_t)((ny >> 8) & 0xFF);
        for (int k = 64; k < 70; k++) r[a + k] = 0;
        r[a + 486] = 0; r[a + 487] = 0;
        r[a + 60] = 2; r[a + 61] = 3; r[a + 62] = 0; r[a + 63] = 0;
        { static int nlog = 0;
          if (nlog < 20) { nlog++;
              fprintf(stderr, "actorfall: actor %d (0x%06X) in 2/2/3 was falling at speed %d,%d,%d (Y %d) - put on the floor %.0f: Y %d, "
                              "state 2/3/0 (HP %d, +500 %d)\n", i, (unsigned)(0x80000000u | a), vx, vy, vz, y, g, ny,
                      (int16_t)(r[a + 280] | (r[a + 281] << 8)), (int16_t)(r[a + 500] | (r[a + 501] << 8))); } }
    }
}

typedef struct { uint32_t gp; int16_t x, z, rx, ry, rz, ours; uint8_t used, fights, gave_up, moves, mech_said; } TiltRec;
static TiltRec s_tilt[36];
static int tilt_floor_on(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("DC_TILT_UNSINK_FLOOR"); v = (e && *e == '0') ? 0 : 1; }
    return v;
}

static int tilt_flush_on(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("DC_TILT_UNSINK_FLUSH"); v = (e && *e == '0') ? 0 : 1; }
    return v;
}

static int tilt_script_y(const uint8_t *r, int k, uint32_t gp, int16_t x, int16_t z, int16_t *ys) {
    const uint32_t scr = ((uint32_t)r[0x100014] | ((uint32_t)r[0x100015] << 8) | ((uint32_t)r[0x100016] << 16) |
                          ((uint32_t)r[0x100017] << 24)) & 0x1FFFFFu;
    if (scr < 0x100000u || scr + 64u >= RAM_SIZE) return 0;
    const uint32_t end = (scr + 0xC000u < RAM_SIZE - 32u) ? scr + 0xC000u : RAM_SIZE - 32u;
    #define TS_S16(ad) ((int16_t)(r[(ad)] | (r[(ad) + 1] << 8)))
    for (uint32_t p = scr; p < end; p++) {
        if (r[p] == 0x22 && r[p + 1] == 0x03 && r[p + 2] == (uint8_t)k && r[p + 3] == 0 && r[p + 4] == 0x36 && r[p + 5] == 0 &&
            TS_S16(p + 6) == x && TS_S16(p + 10) == z) { *ys = TS_S16(p + 8); return 1; }
        if (r[p] == 0x23 && r[p + 1] == (uint8_t)k && TS_S16(p + 12) == x && TS_S16(p + 16) == z &&
            (((uint32_t)r[p + 8] | ((uint32_t)r[p + 9] << 8) | ((uint32_t)r[p + 10] << 16) | ((uint32_t)r[p + 11] << 24)) & 0x1FFFFFu) == gp) {
            *ys = TS_S16(p + 14); return 1; }
    }
    #undef TS_S16
    return 0;
}
void dc_tilt_unsink(void) {
    static int en = -1;
    if (en < 0) { const char *e = getenv("DC_TILT_UNSINK"); en = (e && *e == '0') ? 0 : 1; }
    if (!en) return;
    uint8_t *r = memory_get_ram_ptr(); if (!r) return;

    { extern int g_savestate_loads; static int ld = -1;
      if (ld != g_savestate_loads) { ld = g_savestate_loads; if (tilt_floor_on()) memset(s_tilt, 0, sizeof s_tilt); } }
    if (r[0xB7878] != 3) return;
    const uint32_t S = (uint32_t)(scratchpad[0] | (scratchpad[1] << 8) | (scratchpad[2] << 16) |
                                  ((uint32_t)scratchpad[3] << 24)) & 0x1FFFFFFFu;
    if (S == 0 || S + 0x7CDCu + 36u * 136u >= RAM_SIZE) return;
    #define TU_U32(ad) ((uint32_t)r[(ad)] | ((uint32_t)r[(ad) + 1] << 8) | ((uint32_t)r[(ad) + 2] << 16) | ((uint32_t)r[(ad) + 3] << 24))
    #define TU_S16(ad) ((int16_t)(r[(ad)] | (r[(ad) + 1] << 8)))
    for (int k = 0; k < 36; k++) {
        const uint32_t e = S + 0x7CDCu + 136u * (uint32_t)k;
        TiltRec *t = &s_tilt[k];
        if (!(TU_U32(e + 48u) & 1u)) { t->used = 0; continue; }
        const uint32_t gp = TU_U32(e + 60u) & 0x1FFFFFu;
        if (gp < 0x10000u || gp + 12u >= RAM_SIZE) continue;
        const int16_t x = TU_S16(e + 32u), y = TU_S16(e + 34u), z = TU_S16(e + 36u);
        const int16_t rx = TU_S16(e + 40u), ry = TU_S16(e + 42u), rz = TU_S16(e + 44u);
        if (!(rx & 0xFFF) && !(rz & 0xFFF)) continue;
        const int same = t->used && t->gp == gp && t->x == x && t->z == z && t->rx == rx && t->ry == ry && t->rz == rz;

        if (tilt_floor_on()) {
            if (t->used && t->gp == gp) { if (!same && t->moves < 255) t->moves++; }
            else { t->moves = 0; t->mech_said = 0; }
            if (t->moves >= 3) {
                if (!t->mech_said) { t->mech_said = 1;
                    fprintf(stderr, "tilt unsink: slot %d grp %06X at (%d,%d,%d) turned (%d,%d,%d) - the script moves/turns it (%d changes): a mechanism, left to the game%c",
                            k, gp, x, y, z, rx, ry, rz, t->moves, 10); }
                t->used = 1; t->gp = gp; t->x = x; t->z = z; t->rx = rx; t->ry = ry; t->rz = rz; t->ours = y;
                continue;
            }
        }
        if (same && (t->ours == y || t->gave_up)) continue;
        if (same && t->ours != y) {
            if (++t->fights > 4) {
                t->gave_up = 1;
                fprintf(stderr, "tilt unsink: slot %d grp %06X - the game keeps resetting its height, left to the game%c", k, gp, 10);
                continue;
            }
        } else { t->fights = 0; t->gave_up = 0; }
        t->used = 1; t->gp = gp; t->x = x; t->z = z; t->rx = rx; t->ry = ry; t->rz = rz; t->ours = y;
        const uint32_t tri = TU_U32(gp) & 0x1FFFFFu, quad = TU_U32(gp + 4u) & 0x1FFFFFu;
        const int nt = r[gp + 8] | (r[gp + 9] << 8), nq = r[gp + 10] | (r[gp + 11] << 8);
        if (nt + nq <= 0 || nt + nq > 400) continue;
        if ((nt && tri + (uint32_t)nt * 40u >= RAM_SIZE) || (nq && quad + (uint32_t)nq * 52u >= RAM_SIZE)) continue;
        const float K = 6.28318530718f / 4096.f;
        const float cx = cosf(rx * K), sx = sinf(rx * K), cy = cosf(ry * K), sy = sinf(ry * K), cz = cosf(rz * K), sz = sinf(rz * K);

        const float m10 = sx * sy * cz + cx * sz, m11 = -sx * sy * sz + cx * cz, m12 = -sx * cy;
        float y0lo = 1e9f, y0hi = -1e9f, xlo = 1e9f, xhi = -1e9f, zlo = 1e9f, zhi = -1e9f, yrhi = -1e9f, yrlo = 1e9f;
        for (int pass = 0; pass < 2; pass++) {
            const int n = pass ? nq : nt, nv = pass ? 4 : 3; const uint32_t base = pass ? quad : tri, st = pass ? 52u : 40u;
            for (int p = 0; p < n; p++) for (int v = 0; v < nv; v++) {
                const uint32_t a = base + (uint32_t)p * st + (uint32_t)v * 8u;
                const float vx = (float)TU_S16(a), vy = (float)TU_S16(a + 2u), vz = (float)TU_S16(a + 4u);
                if (vy < y0lo) y0lo = vy; if (vy > y0hi) y0hi = vy;
                if (vx < xlo) xlo = vx; if (vx > xhi) xhi = vx;
                if (vz < zlo) zlo = vz; if (vz > zhi) zhi = vz;
                const float yr = m10 * vx + m11 * vy + m12 * vz;
                if (yr > yrhi) yrhi = yr;
                if (yr < yrlo) yrlo = yr;
            }
        }
        if (y0hi < -40.f || y0hi > 5.f) continue;
        if (xhi - xlo > 3000.f || zhi - zlo > 3000.f || y0hi - y0lo > 3000.f) continue;
        if (yrhi <= 30.f) continue;
        const int raise = (int)ceilf(yrhi + 3.f);
        if (tilt_flush_on() && yrlo >= -5.f) {
            int16_t ys = 0;
            if (tilt_script_y(r, k, gp, x, z, &ys) && abs((int)ys - raise - (int)y) <= 2) {
                r[e + 34u] = (uint8_t)(ys & 0xFF); r[e + 35u] = (uint8_t)((ys >> 8) & 0xFF);
                t->ours = ys;
                fprintf(stderr, "tilt unsink: slot %d grp %06X at (%d,%d,%d) turned (%d,%d,%d) lies flush with the floor (top %.0f) - "
                                "a floor plate, not a tipped item; our earlier lift of %d undone: back to the script's Y %d (DC_TILT_UNSINK_FLUSH=0 off)%c",
                        k, gp, x, y, z, rx, ry, rz, yrlo, raise, ys, 10);
            } else if (!t->mech_said) { t->mech_said = 1;
                fprintf(stderr, "tilt unsink: slot %d grp %06X at (%d,%d,%d) turned (%d,%d,%d) lies flush with the floor (top %.0f, %.0f below) - "
                                "a floor plate, not a tipped item: not lifted (DC_TILT_UNSINK_FLUSH=0 off)%c", k, gp, x, y, z, rx, ry, rz, yrlo, yrhi, 10); }
            continue;
        }
        if (tilt_floor_on()) {
            int fnd = 0;
            const float fl = dc_items_ground_y((float)x, (float)y - 60.f, (float)z, &fnd);
            if (!fnd) { t->used = 0; continue; }
            const float above = fl - (float)y;
            if (fabsf(above - (float)raise) <= 3.f) continue;
            if (fabsf(above) > 8.f) {
                int16_t ys = 0;
                if (tilt_script_y(r, k, gp, x, z, &ys) && ys > y) {
                    const int d = (int)ys - (int)y, m = (d + raise / 2) / raise;
                    if (m >= 1 && m <= 80 && abs(d - m * raise) <= 2 * m) {
                        r[e + 34u] = (uint8_t)(ys & 0xFF); r[e + 35u] = (uint8_t)((ys >> 8) & 0xFF);
                        t->ours = ys;
                        fprintf(stderr, "tilt unsink: slot %d grp %06X at (%d,%d,%d) turned (%d,%d,%d) hangs %.0f above the surface below "
                                        "- a mechanism part, not lifted; it carried %d earlier lift(s) of %d (old rule, saved): back to the script's Y %d%c",
                                k, gp, x, y, z, rx, ry, rz, above, m, raise, ys, 10);
                        continue;
                    }
                }
                { static int nlog = 0;
                  if (nlog < 12) { nlog++;
                      fprintf(stderr, "tilt unsink: slot %d grp %06X at (%d,%d,%d) turned (%d,%d,%d) hangs %.0f above the surface below "
                                      "- a mechanism part, left to the game (DC_TILT_UNSINK_FLOOR=0 - as before)%c", k, gp, x, y, z, rx, ry, rz, above, 10); } }
                continue;
            }
        }
        const int ny = (int)y - raise;
        if (ny < -32768) continue;
        r[e + 34u] = (uint8_t)(ny & 0xFF); r[e + 35u] = (uint8_t)((ny >> 8) & 0xFF);
        t->ours = (int16_t)ny;
        fprintf(stderr, "tilt unsink: slot %d grp %06X at (%d,%d,%d) turned (%d,%d,%d) - the model stands on its point (bottom %.0f) "
                        "but tipped over reaches %.0f below the floor: raised to Y %d (DC_TILT_UNSINK=0 off)%c",
                k, gp, x, y, z, rx, ry, rz, y0hi, yrhi, ny, 10);
    }
    #undef TU_U32
    #undef TU_S16
}
void dc_items_still(void) {
    static int en = -1, angle = -1, drop = 0, ground = -1, lift = 8, logi = 1, flaten = 1;
    static uint32_t flat_hash[24]; static int flat_n = 0;
    if (en < 0) {
        const char *e = getenv("DC_ITEM_STILL"); en = (e && *e == '0') ? 0 : 1;
        e = getenv("DC_ITEM_ANGLE"); if (e) angle = atoi(e) & 0xFFF;
        e = getenv("DC_ITEM_DROP");  if (e) drop = atoi(e);
        e = getenv("DC_ITEMS_GROUND"); ground = (e && *e == '0') ? 0 : 1;
        e = getenv("DC_ITEMS_LIFT"); if (e) lift = atoi(e);
        e = getenv("DC_ITEMS_LOG");  logi = (e && *e == '0') ? 0 : 1;
        itemov_load();

        flat_hash[flat_n++] = 0xB2380ECCu;
        e = getenv("DC_ITEMS_FLAT");
        if (e && e[0] == '0' && !e[1]) flaten = 0;
        else if (e) { const char* p = e;
            while (*p && flat_n < 24) {
                uint32_t h = (uint32_t)strtoul(p, (char**)&p, 16);
                if (h) flat_hash[flat_n++] = h;
                while (*p == ',' || *p == ' ') p++;
            } }
    }
    s_its_init = 1; s_its_en = en; s_its_angle = angle; s_its_drop = drop; s_its_ground = ground; s_its_lift = lift;
    s_its_flaten = flaten; s_its_flat_hash = flat_hash; s_its_flat_n = flat_n;
    if (!en) return;
    { extern int g_dc_items_orig; static int prev_orig = 0;
      if (g_dc_items_orig) { if (!prev_orig) { prev_orig = 1; items_restore_authored(); } items_orig_unsink(); return; }
      if (prev_orig) items_orig_unsink_undo();
      prev_orig = 0; }

    int scene_rot_only = 0, in_scene = 0;
    { extern int dc_cut_camera(void);
      if (dc_cut_camera()) {
          static int pin_in_scene = -1, full = -1;
          if (pin_in_scene < 0) { const char *e = getenv("DC_ITEM_SCENE_PIN"); pin_in_scene = (e && *e == '0') ? 0 : 1; }
          if (full < 0) { const char *e = getenv("DC_ITEM_SCENE_FULL"); full = (e && *e == '0') ? 0 : 1; }
          if (!pin_in_scene) return;
          scene_rot_only = full ? 0 : 1;
          in_scene = 1;
      } }
    uint8_t *r = memory_get_ram_ptr(); if (!r) return;

    uint8_t *sp = memory_get_scratchpad_ptr(); if (!sp) return;
    uint32_t gsp = ((uint32_t)sp[0] | ((uint32_t)sp[1] << 8) | ((uint32_t)sp[2] << 16) | ((uint32_t)sp[3] << 24)) & 0x1FFFFFu;
    if (gsp < 0x1000u || gsp > 0x1F0000u) return;
    uint32_t base = gsp + 31964u;
    int16_t *keep = s_it_keep; uint8_t *have = s_it_have;
    int16_t *pvx = s_it_pvx, *pvy = s_it_pvy, *pvz = s_it_pvz; uint8_t *stab = s_it_stab;
    int16_t *gy = s_it_gy; uint8_t *gnd = s_it_gnd, *flat = s_it_flat;
    for (int i = 0; i < 36; i++) {
        uint32_t a = base + (uint32_t)i * 136u;
        if (a + 64u >= 0x200000u) break;
        uint32_t fl = (uint32_t)r[a+48] | ((uint32_t)r[a+49] << 8) |
                      ((uint32_t)r[a+50] << 16) | ((uint32_t)r[a+51] << 24);
        int type = (int8_t)r[a+46];
        if (!(fl & 1u) || type != 2) { have[i] = 0; stab[i] = 0; gnd[i] = 0; continue; }
        int16_t x  = (int16_t)(r[a+32] | (r[a+33] << 8));
        int16_t y  = (int16_t)(r[a+34] | (r[a+35] << 8));
        int16_t z  = (int16_t)(r[a+36] | (r[a+37] << 8));
        int16_t ry = (int16_t)(r[a+42] | (r[a+43] << 8));
        if (scene_rot_only && (!have[i] || x != pvx[i] || z != pvz[i])) continue;

        if (in_scene && !scene_rot_only && have[i] && y != pvy[i]) {
            have[i] = 0; stab[i] = 0; gnd[i] = 0;
            if (logi) fprintf(stderr, "item: slot=%d moved by the scene script (y %d, ours %d) - let go until it stands again%c", i, y, pvy[i], 10);
        }

        if (have[i] && (x != pvx[i] || z != pvz[i])) { have[i] = 0; stab[i] = 0; gnd[i] = 0; }
        if (!have[i]) {

            if (x != pvx[i] || y != pvy[i] || z != pvz[i]) stab[i] = 0;
            else if (stab[i] < 250) stab[i]++;
            pvx[i] = x; pvy[i] = y; pvz[i] = z;
            if (stab[i] < 3) continue;
            have[i] = 1;
            keep[i] = (angle >= 0) ? (int16_t)angle : ry;
            gnd[i] = 0; gy[i] = y; flat[i] = 0; s_it_ping[i] = 0;

            const int igi = igc_find_live((uint32_t)r[a+60] | ((uint32_t)r[a+61] << 8) | ((uint32_t)r[a+62] << 16) | ((uint32_t)r[a+63] << 24), x, z);
            if (igi >= 0 && angle < 0) keep[i] = 0;
            s_it_gen[i] = dc_camfb_gen();
            s_it_ax[i] = x; s_it_az[i] = z;
            s_it_rx0[i] = (int16_t)(r[a+40] | (r[a+41] << 8));
            s_it_rz0[i] = (int16_t)(r[a+44] | (r[a+45] << 8));
            {
                uint32_t mp = (uint32_t)r[a+60] | ((uint32_t)r[a+61] << 8) |
                              ((uint32_t)r[a+62] << 16) | ((uint32_t)r[a+63] << 24);
                uint32_t ma = mp & 0x1FFFFFu, h = 2166136261u;
                if (ma > 0x1000u && ma + 36u < 0x200000u) {
                    for (int b = 12; b < 36; b++) { h ^= r[ma + (uint32_t)b]; h *= 16777619u; }
                } else h = 0;
                s_it_hash[i] = h;
                if (flaten) { for (int k = 0; k < flat_n; k++)
                                  if (h && h == flat_hash[k]) { flat[i] = 1; break; } }
                {
                    int ov = itemov_find(h, x, z, 0);
                    if (ov >= 0) {
                        if (s_itemov[ov].flat >= 0) flat[i] = (uint8_t)s_itemov[ov].flat;
                        if (s_itemov[ov].ry_off)
                            keep[i] = (int16_t)((keep[i] + s_itemov[ov].ry_off) & 0xFFF);
                    }
                }
                if (logi) fprintf(stderr, "item: slot=%d model=%08X pos=(%d,%d,%d) flat=%d\n",
                                  i, h, x, y, z, flat[i]);
            }
            if (drop) { y = (int16_t)(y + drop);
                        r[a+34] = (uint8_t)(y & 0xFF); r[a+35] = (uint8_t)((y >> 8) & 0xFF);
                        pvy[i] = y; }
            s_it_ay[i] = y;
            if (igi >= 0) {
                s_it_ay[i] = s_igc[igi].ay; gy[i] = s_igc[igi].gy; gnd[i] = s_igc[igi].gnd;
                if (logi) fprintf(stderr, "item: slot=%d takes the pose of its ghost (y %d, grounded %d)%c", i, (int)(gnd[i] ? gy[i] : s_it_ay[i]), gnd[i], 10);
            }
        }

        int fresh = 1;
        { static int gf = -1;
          if (gf < 0) { const char *e = getenv("DC_ITEMS_GROUND_FRESH"); gf = (e && *e == '0') ? 0 : 1; }
          if (gf && (uint32_t)(dc_camfb_gen() - s_it_gen[i]) < 2u) fresh = 0; }
        if (ground && !gnd[i] && !scene_rot_only && fresh) {
            int f = 0;
            float by = dc_items_ground_y((float)x, (float)s_it_ay[i], (float)z, &f);

            { static float gmax = -1.f;
              if (gmax < 0.f) { const char *e = getenv("DC_ITEMS_GROUND_MAX"); gmax = e ? (float)atof(e) : 200.f; }
              if (f && by - (float)s_it_ay[i] > gmax) {
                  if (logi) fprintf(stderr, "item: slot=%d ground %d -> %.0f too far (> %.0f), left as authored%c", i, pvy[i], by, gmax, 10);
                  f = 0; gnd[i] = 1; }
            }

            if (!f && !gnd[i]) {
                static int us = -1;
                if (us < 0) { const char *e = getenv("DC_ITEMS_UNSINK_SAVE"); us = (e && *e == '0') ? 0 : 1; }
                int f2 = 0;
                const float by2 = us ? dc_items_ground_y((float)x, (float)s_it_ay[i] - 400.f, (float)z, &f2) : 0.f;
                if (f2 && by2 < (float)s_it_ay[i] - 1.f) {
                    if (logi) fprintf(stderr, "item: slot=%d was under the floor (y %d, floor %.0f above it) - put back on it%c", i, s_it_ay[i], by2, 10);
                    s_it_ay[i] = (int16_t)by2; by = by2; f = 1;
                }
            }
            if (f) { gy[i] = (int16_t)(by - (float)lift); gnd[i] = 1;
                     if (logi) fprintf(stderr, "item: slot=%d grounded y %d -> %d\n",
                                       i, pvy[i], gy[i]); }
        }
        if (!scene_rot_only) {
            int ov = itemov_find(s_it_hash[i], s_it_ax[i], s_it_az[i], 0);
            int dyu = (ov >= 0) ? s_itemov[ov].dy_up : itemov_model_dy(s_it_hash[i]);
            int uy = (gnd[i] ? gy[i] : s_it_ay[i]) - dyu;
            if (s_it_ping[i] > 0) { s_it_ping[i]--; uy -= 300; }
            int16_t cy = (int16_t)(r[a+34] | (r[a+35] << 8));
            if (cy != (int16_t)uy) { r[a+34] = (uint8_t)(uy & 0xFF);
                                     r[a+35] = (uint8_t)((uy >> 8) & 0xFF); }
            pvy[i] = (int16_t)uy;
        }
        {
            int ovr = itemov_find(s_it_hash[i], s_it_ax[i], s_it_az[i], 0);
            int yoff = (ovr >= 0) ? s_itemov[ovr].ry_off : 0;

            int zoff = (ovr >= 0) ? s_itemov[ovr].rz_off : 0;
            int16_t want_ry = flat[i] ? (int16_t)(zoff & 0xFFF) : keep[i];
            int16_t want_rz = flat[i] ? (int16_t)(yoff & 0xFFF)
                                      : (zoff ? (int16_t)((s_it_rz0[i] + zoff) & 0xFFF) : s_it_rz0[i]);
            int16_t rz = (int16_t)(r[a+44] | (r[a+45] << 8));
            if (ry != want_ry) { r[a+42] = (uint8_t)(want_ry & 0xFF); r[a+43] = (uint8_t)((want_ry >> 8) & 0xFF); }
            if (rz != want_rz) { r[a+44] = (uint8_t)(want_rz & 0xFF); r[a+45] = (uint8_t)((want_rz >> 8) & 0xFF); }
        }
        {
            int16_t rx = (int16_t)(r[a+40] | (r[a+41] << 8));
            int ovx = itemov_find(s_it_hash[i], s_it_ax[i], s_it_az[i], 0);
            int xoff = (ovx >= 0) ? s_itemov[ovx].rx_off : 0;
            int16_t want = flat[i] ? (int16_t)((1024 + xoff) & 0xFFF)
                                   : (xoff ? (int16_t)((s_it_rx0[i] + xoff) & 0xFFF) : s_it_rx0[i]);
            if (rx != want) { r[a+40] = (uint8_t)(want & 0xFF); r[a+41] = (uint8_t)((want >> 8) & 0xFF); }
        }
        pvx[i] = x; pvz[i] = z;
    }
}

typedef struct { int slot; unsigned int hash; int x, y, z; int gnd, flat, dy_up; int rx, ry, rz; } DcItemView;
int dc_items_view(DcItemView* out, int max) {
    int n = 0;
    for (int i = 0; i < 36 && n < max; i++) {
        if (!s_it_have[i]) continue;
        out[n].slot = i; out[n].hash = s_it_hash[i];
        out[n].x = s_it_ax[i]; out[n].y = s_it_pvy[i]; out[n].z = s_it_az[i];
        out[n].gnd = s_it_gnd[i]; out[n].flat = s_it_flat[i];
        int ov = itemov_find(s_it_hash[i], s_it_ax[i], s_it_az[i], 0);
        out[n].dy_up = (ov >= 0) ? s_itemov[ov].dy_up : 0;
        out[n].rx = (ov >= 0) ? s_itemov[ov].rx_off : 0;
        out[n].ry = (ov >= 0) ? s_itemov[ov].ry_off : 0;
        out[n].rz = (ov >= 0) ? s_itemov[ov].rz_off : 0;
        n++;
    }
    return n;
}
void dc_items_nudge(int slot, int dy_up) {
    if (slot < 0 || slot >= 36 || !s_it_have[slot]) return;
    int ov = itemov_find(s_it_hash[slot], s_it_ax[slot], s_it_az[slot], 1);
    if (ov < 0) return;
    s_itemov[ov].dy_up += dy_up;
    itemov_save();
}
void dc_items_set_flat(int slot, int on) {
    if (slot < 0 || slot >= 36 || !s_it_have[slot]) return;
    int ov = itemov_find(s_it_hash[slot], s_it_ax[slot], s_it_az[slot], 1);
    if (ov < 0) return;
    s_itemov[ov].flat = on ? 1 : 0;
    s_it_flat[slot] = on ? 1 : 0;
    itemov_save();
}
void dc_items_ping(int slot) {
    if (slot < 0 || slot >= 36 || !s_it_have[slot]) return;
    s_it_ping[slot] = 45;
}
void dc_items_rotate(int slot, int dyaw) {
    if (slot < 0 || slot >= 36 || !s_it_have[slot]) return;
    int ov = itemov_find(s_it_hash[slot], s_it_ax[slot], s_it_az[slot], 1);
    if (ov < 0) return;
    s_itemov[ov].ry_off = (s_itemov[ov].ry_off + dyaw) & 0xFFF;
    s_it_keep[slot] = (int16_t)((s_it_keep[slot] + dyaw) & 0xFFF);
    itemov_save();
}

void dc_items_tilt(int slot, int axis, int d) {
    if (slot < 0 || slot >= 36 || !s_it_have[slot]) return;
    int ov = itemov_find(s_it_hash[slot], s_it_ax[slot], s_it_az[slot], 1);
    if (ov < 0) return;
    if (axis == 0) s_itemov[ov].rx_off = (s_itemov[ov].rx_off + d) & 0xFFF;
    else           s_itemov[ov].rz_off = (s_itemov[ov].rz_off + d) & 0xFFF;
    itemov_save();
}

int dc_item_spin_step(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("DC_ITEM_SPIN"); v = (e && *e == '1') ? 64 : 0; }
    { extern int g_dc_items_orig; if (g_dc_items_orig) return 64; }
    return v;
}

int g_aim_has_target = 0;
extern int dc_fire_recent(void);

int g_dc_showall_force  = 1;
int g_dc_fullroom_force = 1;

uint8_t  g_dc_union_claim[90];
uint32_t g_dc_union_claim_grp[90];
int      g_dc_union_claim_on = 0;
int      g_dc_union_claim_ready = 0;

void dc_laser_log_tick(void) {
    static int en = -1;
    if (en < 0) { const char *e = getenv("DC_LASER_LOG"); en = (e && *e == '1') ? 1 : 0; }
    if (!en) return;
    static int div = 0;
    if ((div++ % 30) != 0) return;
    const uint8_t *r = memory_get_ram_ptr(); if (!r) return;
    uint32_t S = (uint32_t)(scratchpad[0] | (scratchpad[1] << 8) |
                            (scratchpad[2] << 16) | ((uint32_t)scratchpad[3] << 24)) & 0x1FFFFFFFu;
    if (S == 0 || S + 0x9100u >= RAM_SIZE) return;
    uint32_t bank = (uint32_t)(r[0x9CB48] | (r[0x9CB49] << 8) | (r[0x9CB4A] << 16) |
                               ((uint32_t)r[0x9CB4B] << 24)) & 0x1FFFFFFFu;
    int f0C = -1, f0D = -1;
    if (bank && bank + 4u < RAM_SIZE) {
        f0C = (r[bank + (0x0C >> 3)] >> (0x0C & 7)) & 1;
        f0D = (r[bank + (0x0D >> 3)] >> (0x0D & 7)) & 1;
    }
    uint32_t tbl = S + 0x7CDCu;
    uint32_t st[8]; uint32_t gp[8];
    for (int i = 0; i < 8; i++) {
        uint32_t e = tbl + 136u * (uint32_t)(i + 2);
        st[i] = (e + 64u < RAM_SIZE)
              ? (uint32_t)(r[e+48] | (r[e+49]<<8) | (r[e+50]<<16) | ((uint32_t)r[e+51]<<24)) : 0;
        gp[i] = (e + 64u < RAM_SIZE)
              ? ((uint32_t)(r[e+60] | (r[e+61]<<8) | (r[e+62]<<16) | ((uint32_t)r[e+63]<<24)) & 0x1FFFFFFFu) : 0;
    }
    static uint32_t prev[8]; static int have = 0; static long n = 0;
    int changed = !have;
    for (int i = 0; i < 8; i++) if (st[i] != prev[i]) changed = 1;
    if (!changed && (n % 20) != 0) { n++; return; }
    for (int i = 0; i < 8; i++) prev[i] = st[i];
    have = 1; n++;
    fprintf(stderr, "laser: f0C=%d f0D=%d force(sa=%d fr=%d) sect2..9:"
                    " %d/%06X %d/%06X %d/%06X %d/%06X %d/%06X %d/%06X %d/%06X %d/%06X%c",
            f0C, f0D, g_dc_showall_force, g_dc_fullroom_force,
            st[0]&3u, gp[0], st[1]&3u, gp[1], st[2]&3u, gp[2], st[3]&3u, gp[3],
            st[4]&3u, gp[4], st[5]&3u, gp[5], st[6]&3u, gp[6], st[7]&3u, gp[7], 10);
}

int dc_bloodprobe_on(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("DC_BLOODPROBE"); v = (e && *e == '1') ? 1 : 0; }
    return v;
}

int dc_blood_mult(void) {
    static int ov = -2, npist = -1, nspread = -1;
    if (ov == -2) { const char *e = getenv("DC_BLOOD_MULT"); ov = e ? atoi(e) : -1;
                    e = getenv("DC_BLOOD_MULT_PISTOL");  npist   = e ? atoi(e) : 1;
                    e = getenv("DC_BLOOD_MULT_SPREAD"); nspread = e ? atoi(e) : 5;
                    if (npist < 1) npist = 1; if (npist > 8) npist = 8;
                    if (nspread < 1) nspread = 1; if (nspread > 8) nspread = 8; }
    if (ov > 0) return ov > 8 ? 8 : ov;
    { extern int dc_dart_now(void); if (dc_dart_now()) return 1; }
    const uint8_t *r = memory_get_ram_ptr();
    int wpn = r ? (r[0xB94CC] & 0xF0) : 0x20;
    if (wpn == 0x10 || wpn == 0x30) {

        static uint32_t q = 0x2545F491u;
        q ^= q << 13; q ^= q >> 17; q ^= q << 5;
        static int lo = -1;
        if (lo < 0) { const char *e = getenv("DC_BLOOD_MULT_MIN"); lo = e ? atoi(e) : 2;
                      if (lo < 1) lo = 1; }
        int hi = nspread < lo ? lo : nspread;
        return lo + (int)(q % (uint32_t)(hi - lo + 1));
    }
    return npist;
}
int dc_blood_pool_room(void) {
    const uint8_t *r = memory_get_ram_ptr();
    return r ? (r[0xC0A84] < 40) : 0;
}
uint32_t dc_rng_seed_get(void) {
    const uint8_t *r = memory_get_ram_ptr();
    return r ? (uint32_t)(r[0xB56EA] | (r[0xB56EB] << 8)) : 0;
}
void dc_rng_seed_set(uint32_t s) {
    uint8_t *r = memory_get_ram_ptr(); if (!r) return;
    r[0xB56EA] = (uint8_t)s; r[0xB56EB] = (uint8_t)(s >> 8);
}

static struct { uint32_t slot; int x, y, z; uint64_t vb; int done; } s_bwatch[4];
static int s_bwatch_n = 0;
void dc_blood_watch_note(uint32_t slot, int x, int y, int z) {
    extern uint64_t s_frame_count;
    if (!dc_bloodprobe_on()) return;
    int i = s_bwatch_n++ & 3;
    s_bwatch[i].slot = slot; s_bwatch[i].x = x; s_bwatch[i].y = y; s_bwatch[i].z = z;
    s_bwatch[i].vb = s_frame_count; s_bwatch[i].done = 0;
}
void dc_blood_watch_tick(void) {
    extern uint64_t s_frame_count;
    if (!dc_bloodprobe_on()) return;
    const uint8_t *r = memory_get_ram_ptr(); if (!r) return;
    for (int i = 0; i < 4; i++) {
        if (s_bwatch[i].done || !s_bwatch[i].vb) continue;
        if (s_frame_count - s_bwatch[i].vb < 3) continue;
        uint32_t s = s_bwatch[i].slot;
        int x = (int16_t)(r[s+32] | (r[s+33] << 8));
        int y = (int16_t)(r[s+34] | (r[s+35] << 8));
        int z = (int16_t)(r[s+36] | (r[s+37] << 8));
        uint32_t node = (uint32_t)(r[s+76] | (r[s+77]<<8) | (r[s+78]<<16) | ((uint32_t)r[s+79]<<24));
        fprintf(stderr, "bwatch slot=%05X wrote=%d,%d,%d now=%d,%d,%d %s node=%08X%c",
                s, s_bwatch[i].x, s_bwatch[i].y, s_bwatch[i].z, x, y, z,
                (x == s_bwatch[i].x && y == s_bwatch[i].y && z == s_bwatch[i].z)
                    ? "KEPT" : "OVERWRITTEN", node, 10);
        s_bwatch[i].done = 1;
    }
}
void dc_blood_place(uint32_t slot, uint32_t tmpl, uint32_t actor) {
    (void)tmpl;
    static int en = -1;
    if (en < 0) { const char *e = getenv("DC_BLOOD_AT_HIT"); en = (e && *e == '0') ? 0 : 1; }
    if (!en) return;
    extern float    g_aim_hit_pt[3];
    extern uint32_t g_aim_hit_actor;
    extern uint64_t g_aim_hit_vb;
    extern uint64_t s_frame_count;
    uint8_t *r = memory_get_ram_ptr(); if (!r) return;
    uint32_t s = slot & 0x1FFFFFu, a = actor & 0x1FFFFFu;
    if (s + 124u >= 0x200000u || a + 40u >= 0x200000u) return;
    if (!g_aim_hit_vb || s_frame_count - g_aim_hit_vb > 30) return;
    if (g_aim_hit_actor != a) return;
    int ax = (int16_t)(r[a+32] | (r[a+33] << 8));
    int ay = (int16_t)(r[a+34] | (r[a+35] << 8));
    int az = (int16_t)(r[a+36] | (r[a+37] << 8));
    float dx = g_aim_hit_pt[0] - (float)ax;
    float dy = g_aim_hit_pt[1] - (float)ay;
    float dz = g_aim_hit_pt[2] - (float)az;
    float d  = sqrtf(dx*dx + dy*dy + dz*dz);
    static float maxd = -1.f, maxd_trex = -1.f;
    if (maxd < 0.f) { const char *e = getenv("DC_BLOOD_MAXD"); maxd = e ? (float)atof(e) : 1500.f;
                      e = getenv("DC_TREX_BLOOD_MAXD"); maxd_trex = e ? (float)atof(e) : 16000.f; }

    extern int dc_actor_trex_model(const uint8_t *r, uint32_t a);
    const float lim = dc_actor_trex_model(r, a) ? maxd_trex : maxd;
    if (d > lim) {
        if (dc_bloodprobe_on())
            fprintf(stderr, "bplace SKIP d=%.0f pt=%.0f,%.0f,%.0f actor=%d,%d,%d%c",
                    (double)d, (double)g_aim_hit_pt[0], (double)g_aim_hit_pt[1],
                    (double)g_aim_hit_pt[2], ax, ay, az, 10);
        return;
    }

    static uint32_t rs = 0x9E3779B9u;
    #define BR_NEXT() (rs ^= rs << 13, rs ^= rs >> 17, rs ^= rs << 5, rs)
    static float jit = -1.f, smin, smax; static int scale_on = -1, piece_on = -1;
    if (jit < 0.f) { const char *e = getenv("DC_BLOOD_JITTER"); jit = e ? (float)atof(e) : 500.f;
                     if (jit < 0.f) jit = 0.f;
                     e = getenv("DC_BLOOD_SCALE_MIN"); smin = e ? (float)atof(e) : 0.70f;
                     e = getenv("DC_BLOOD_SCALE_MAX"); smax = e ? (float)atof(e) : 1.30f;
                     if (smax < smin) smax = smin;
                     e = getenv("DC_BLOOD_SCALE"); scale_on = (e && *e == '0') ? 0 : 1;
                     e = getenv("DC_BLOOD_PIECE"); piece_on = (e && *e == '0') ? 0 : 1; }

    float jw = jit;
    { int wpn = r[0xB94CC] & 0xF0;
      extern int dc_dart_now(void);
      if ((wpn != 0x10 && wpn != 0x30) || dc_dart_now()) {
          static float jp = -1.f;
          if (jp < 0.f) { const char *e = getenv("DC_BLOOD_JITTER_PISTOL"); jp = e ? (float)atof(e) : 0.f;
                          if (jp < 0.f) jp = 0.f; }
          jw = jp;
      } }
    float j0 = ((float)(BR_NEXT() & 0xFFFF) / 32768.f - 1.f) * jw;
    float j1 = ((float)(BR_NEXT() & 0xFFFF) / 32768.f - 1.f) * jw;
    float j2 = ((float)(BR_NEXT() & 0xFFFF) / 32768.f - 1.f) * jw;
    int px = (int)lroundf(g_aim_hit_pt[0] + j0);
    int py = (int)lroundf(g_aim_hit_pt[1] + j1);
    int pz = (int)lroundf(g_aim_hit_pt[2] + j2);
    if (scale_on) {
        float k = smin + (smax - smin) * ((float)(BR_NEXT() & 0xFFFF) / 65535.f);
        int w = (int16_t)(r[s+96] | (r[s+97] << 8));
        int h = (int16_t)(r[s+98] | (r[s+99] << 8));
        int w2 = (int)lroundf((float)w * k), h2 = (int)lroundf((float)h * k);
        if (w > 0 && w2 < 1) w2 = 1;
        if (h > 0 && h2 < 1) h2 = 1;
        r[s+96] = (uint8_t)w2; r[s+97] = (uint8_t)(w2 >> 8);
        r[s+98] = (uint8_t)h2; r[s+99] = (uint8_t)(h2 >> 8);
    }
    if (piece_on) {
        static const uint8_t pcs[2] = { 3, 5 };
        r[s+47] = pcs[BR_NEXT() & 1u];
    }
    if (px < -32000) px = -32000; if (px > 32000) px = 32000;
    if (py < -32000) py = -32000; if (py > 32000) py = 32000;
    if (pz < -32000) pz = -32000; if (pz > 32000) pz = 32000;
    r[s+32] = (uint8_t)px;        r[s+33] = (uint8_t)(px >> 8);
    r[s+34] = (uint8_t)py;        r[s+35] = (uint8_t)(py >> 8);
    r[s+36] = (uint8_t)pz;        r[s+37] = (uint8_t)(pz >> 8);
    r[s+76] = r[s+77] = r[s+78] = r[s+79] = 0;
    if (dc_bloodprobe_on())
        fprintf(stderr, "bplace slot=%05X -> %d,%d,%d (d=%.0f from actor %05X) size=%d,%d piece=%02X%c",
                s, px, py, pz, (double)d, a,
                (int)(int16_t)(r[s+96] | (r[s+97] << 8)),
                (int)(int16_t)(r[s+98] | (r[s+99] << 8)), r[s+47], 10);

    { extern void dc_blood_watch_note(uint32_t slot, int x, int y, int z);
      dc_blood_watch_note(s, px, py, pz); }
}

static float s_blast_k[10];
static unsigned long long s_blast_vb[10];
void dc_blast_victim(int slot, float k) {
    extern uint64_t s_frame_count;
    if (slot < 0 || slot >= 10) return;
    s_blast_k[slot] = k;
    s_blast_vb[slot] = s_frame_count ? s_frame_count : 1;
}
static int blast_recent(int slot) {
    extern uint64_t s_frame_count;
    return slot >= 0 && slot < 10 && s_blast_vb[slot] && (unsigned long long)s_frame_count - s_blast_vb[slot] < 120ull;
}

static unsigned long long s_gl_direct_vb[10];
unsigned long long dc_grenade_direct_vb(int slot) { return (slot >= 0 && slot < 10) ? s_gl_direct_vb[slot] : 0ull; }

uint16_t dc_focus_damage_half(uint32_t phys, uint16_t val) {
    extern int   g_opt_focus_mode;
    extern float dc_focus_damage_scale(void);
    extern void  dc_dmg_note(int slot, int oldhp, int newhp, float scale);
    if (phys < 0xB7ACCu) return val;
    uint32_t off = phys - 0xB7ACCu;
    if (off >= 10u * 608u || (off % 608u) != 280u) return val;
    const uint8_t *r = memory_get_ram_ptr(); if (!r) return val;
    int oldhp = (int16_t)(r[phys] | (r[phys + 1] << 8));
    int newhp = (int16_t)val;
    int dmg = oldhp - newhp;
    if (dmg <= 0) return val;

    int mine = 1;
    { extern int dc_nots_active(void); if (!dc_nots_active()) mine = 0; }
    { extern int dc_cut_camera(void); if (dc_cut_camera()) mine = 0; }
    if (r[0xB92C8] != 1) mine = 0;
    if (r[0xB92C9] != 6 && !dc_fire_recent()) mine = 0;

    float dist = 0.f;
    int rx = 0, rz = 0, ex = 0, ez = 0;
    {
      rx = (int16_t)(r[0xB92AC] | (r[0xB92AD] << 8));
      rz = (int16_t)(r[0xB92B0] | (r[0xB92B1] << 8));
      ex = (int16_t)(r[phys - 280 + 32] | (r[phys - 280 + 33] << 8));
      ez = (int16_t)(r[phys - 280 + 36] | (r[phys - 280 + 37] << 8));
      float dx = (float)(ex - rx), dz = (float)(ez - rz);
      dist = sqrtf(dx * dx + dz * dz); }
    extern float dc_damage_mult(int weapon, float dist);
    extern int dc_dart_now(void);
    const int dart = dc_dart_now();
    float k = (mine && g_opt_focus_mode >= 2 && !dart)
                  ? dc_damage_mult((int)r[0xB94CC], dist) : 1.0f;
    {
        const int slot = (int)(off / 608u);
        if (blast_recent(slot) && s_blast_k[slot] > 0.f) {
            k *= s_blast_k[slot];
            static long n = 0;
            if (++n <= 200) fprintf(stderr, "blastdmg: slot %d hp %d dmg %d x%.2f%c", slot, oldhp, dmg, (double)s_blast_k[slot], 10);
            s_blast_k[slot] = 0.f;
        } else if ((r[0xB94CC] & 0xF0) == 0x30) {
            extern uint64_t s_frame_count;
            s_gl_direct_vb[slot] = s_frame_count ? s_frame_count : 1;
        } }

    { static long n = 0;
      if (++n < 300)
          fprintf(stderr, "dmgcalc: wpn=%02X dist=%.0f k=%.2f dmg=%d mine=%d dart=%d slot=%d%c",
                  (unsigned)r[0xB94CC], (double)dist, (double)k, dmg, mine, dart,
                  (int)(off / 608u), 10); }
    if (dart) { static long n = 0;
        if (++n <= 200) fprintf(stderr, "dartdmg: slot %d hp %d -> %d (dart kind %d: the game's own number)%c",
                                (int)(off / 608u), oldhp, newhp, r[0xB94D1], 10); }
    int nd = dmg;
    if (k < 0.999f || k > 1.001f) {
        nd = (int)(dmg * k + 0.5f);
        if (nd < 1) nd = 1;
        if (oldhp - nd < -30000) nd = oldhp + 30000;
        { static long n = 0;
          if (++n < 200)
              fprintf(stderr, "focusdmg: slot %d hp %d -> %d (was %d, x%.2f)%c",
                      (int)(off / 608u), oldhp, oldhp - nd, newhp, (double)k, 10); }
    }
    dc_dmg_note((int)(off / 608u), oldhp, oldhp - nd, k);
    return (uint16_t)(int16_t)(oldhp - nd);
}

int g_aim_game_pick = -1, g_aim_our_pick = -1;
unsigned long long g_aim_pick_frame = 0;

static uint32_t s_shot_params = 0;
unsigned long long g_shot_log_vb = 0;
int g_presdump_req = 0;
int g_shot_victim = -1;
unsigned long long g_shot_victim_frame = 0;

int g_shot_victims[3] = { -1, -1, -1 };

int dc_recent_shot_victim(int slot) {
    extern uint64_t s_frame_count;
    if (slot < 0 || slot >= 10) return 0;
    if (blast_recent(slot)) return 1;
    if ((unsigned long long)s_frame_count - g_shot_victim_frame >= 90ull) return 0;
    if (g_shot_victim == slot) return 1;
    for (int i = 0; i < 3; i++) if (g_shot_victims[i] == slot) return 1;
    return 0;
}

float g_dc_aim_xerr = 0.f;
float g_dc_aim_face_yaw = 0.f;
unsigned long long g_dc_aim_xerr_vb = 0;
unsigned long long g_shot_turn_vb = 0;
int g_shot_turn_nv = 0;
int dc_aim_turning(float *err_deg) {
    static int en = -1; static float tol = 15.f;
    if (en < 0) { const char *e = getenv("DC_SHOT_FACING"); en = (e && *e == '0') ? 0 : 1;
                  e = getenv("DC_SHOT_FACING_TOL"); if (e) tol = (float)atof(e);
                  fprintf(stderr, "shotfacing: %s, tolerance %.1f deg (DC_SHOT_FACING=0 - the shot always goes to the crosshair)%c",
                          en ? "on - while Regina is still turning the shot goes along her facing" : "off", (double)tol, 10); }
    extern uint64_t s_frame_count;
    const float d = g_dc_aim_xerr * (180.f / 3.14159265f);
    if (err_deg) *err_deg = d;
    if (!en || !g_dc_aim_xerr_vb || (unsigned long long)s_frame_count - g_dc_aim_xerr_vb > 3ull) return 0;
    return fabsf(d) > tol;
}
void dc_shot_list_begin(uint32_t player, uint32_t params) { (void)player; s_shot_params = params; }

int dc_free_aim_nolock(void) {
    static int en = -1;
    if (en < 0) { const char *e = getenv("DC_FREE_AIM_NOLOCK"); en = (e && *e == '0') ? 0 : 1; }
    if (!en || !ram) return 0;
    extern int dc_nots_active(void); extern int dc_cut_camera(void);
    const int v = dc_nots_active() && !dc_cut_camera() && ram[0xB92C8] == 1;
    { static int lv = -1;
      if (v != lv) { lv = v;
          fprintf(stderr, "freeaim: %s (DC_FREE_AIM_NOLOCK=0 - the game's lock-on as before)%c",
                  v ? "our aim - the game's lock-on target search answers 'none', a press always fires, the crosshair picks the victim"
                    : "the game's own lock-on", 10); } }
    return v;
}






int dc_shot_gate_miss(uint32_t actor) {
    static int en = -1; static float tpsr = 300.f;
    { static int ht = -1;
      if (ht < 0) { const char *e = getenv("DC_HITTRACE"); ht = (e && *e == '1') ? 1 : 0; }
      if (ht) {
          extern uint64_t s_frame_count;
          static uint64_t lastf = ~0ull; static uint32_t lasta = 0;
          if (s_frame_count != lastf || actor != lasta) {
              lastf = s_frame_count; lasta = actor;
              const uint8_t *rr = memory_get_ram_ptr();
              uint32_t aa = actor & 0x1FFFFFFFu;
              int slot = (aa >= 0xB7ACCu && aa < 0xB7ACCu + 10u*608u) ? (int)((aa - 0xB7ACCu) / 608u) : -1;
              fprintf(stderr, "hittrace: f=%llu victim slot %d | B94B0 now=%d | game scan pick=%d | our pick=%d (f=%llu)%c",
                      (unsigned long long)s_frame_count, slot, rr ? rr[0xB94B0] : -1,
                      g_aim_game_pick, g_aim_our_pick, g_aim_pick_frame, 10);
              extern volatile uint32_t g_fn_ring2[]; extern volatile uint32_t g_fn_ring2_pos;
              uint32_t pos = g_fn_ring2_pos;
              char buf[1024]; int n = 0;
              for (int k = 1; k <= 48 && n < 960; k++)
                  n += snprintf(buf + n, sizeof buf - n, " %08X", g_fn_ring2[(pos - (uint32_t)k) & 4095u]);
              fprintf(stderr, "hittrace: fn (newest first):%s%c", buf, 10);
              {
                const uint32_t want[4] = { 0x8004D034u, 0x80054F50u, 0x80052174u, 0x80052124u };
                int at[4] = { -1, -1, -1, -1 }, cnt[4] = { 0, 0, 0, 0 };
                for (int k = 1; k <= 4000; k++) {
                    uint32_t fa = g_fn_ring2[(pos - (uint32_t)k) & 4095u];
                    for (int q = 0; q < 4; q++) if (fa == want[q]) { if (at[q] < 0) at[q] = k; cnt[q]++; }
                }
                fprintf(stderr, "hittrace: last entries back: 8004D034@%d(x%d) 80054F50@%d(x%d) scan80052174@%d(x%d) 80052124@%d | player+452=%02X +537=%d%c",
                        at[0], cnt[0], at[1], cnt[1], at[2], cnt[2], at[3],
                        rr ? rr[0xB9450] : 0, rr ? rr[0xB94A5] : 0, 10); }
          } } }
    if (en < 0) { const char *e = getenv("DC_AIM_TPS"); en = (e && *e == '0') ? 0 : 1;
                  e = getenv("DC_AIM_TPS_R"); if (e) tpsr = (float)atof(e); }

    { static int dbg = -1;
      if (dbg < 0) { const char *e2 = getenv("DC_AIM_DBG"); dbg = (e2 && *e2 == '1') ? 1 : 0; }
      if (dbg) { static long n = 0;
          if (++n < 300) {
              const uint8_t *rr = memory_get_ram_ptr();
              extern int dc_nots_active(void); extern int dc_cut_camera(void);
              uint32_t aa = actor & 0x1FFFFFFFu;
              fprintf(stderr, "gateprobe: actor=%08X slotok=%d nots=%d cut=%d mode=%d act=%d firerecent=%d en=%d%c",
                      actor,
                      (aa >= 0xB7ACCu && aa < 0xB7ACCu + 10u*608u && ((aa - 0xB7ACCu) % 608u) == 0u),
                      dc_nots_active(), dc_cut_camera(),
                      rr ? rr[0xB92C8] : -1, rr ? rr[0xB92C9] : -1,
                      dc_fire_recent(), en, 10);
          } } }
    if (!en) return 0;
    { extern int dc_nots_active(void); if (!dc_nots_active()) return 0; }
    { extern int dc_cut_camera(void); if (dc_cut_camera()) return 0; }
    uint8_t *r = memory_get_ram_ptr(); if (!r) return 0;
    if (r[0xB92C8] != 1) return 0;

    if (r[0xB92C9] != 6 && !dc_fire_recent()) return 0;
    uint32_t a = actor & 0x1FFFFFFFu;
    if (a < 0xB7ACCu || a >= 0xB7ACCu + 10u * 608u ||
        ((a - 0xB7ACCu) % 608u) != 0u) return 0;
    {
        extern uint64_t s_frame_count;
        const int slot = (int)((a - 0xB7ACCu) / 608u);
        if (blast_recent(slot)) return 0;
        if ((unsigned long long)s_frame_count - g_shot_victim_frame < 90ull)
            for (int i = 0; i < 3; i++) if (g_shot_victims[i] == slot) return 0; }

    if ((r[0xB94CC] & 0xF0) == 0x30) {
        static long n = 0;
        if (++n <= 200) fprintf(stderr, "missgate: hit (the grenade itself decided, slot %d)%c", (int)((a - 0xB7ACCu) / 608u), 10);
        return 0;
    }
    {
      extern uint64_t s_frame_count;
      if (g_shot_victim >= 0 && a == 0xB7ACCu + (uint32_t)g_shot_victim * 608u &&
          (unsigned long long)s_frame_count - g_shot_victim_frame < 90ull) {
          static long n = 0;
          if (++n <= 5000) fprintf(stderr, "missgate: hit (the shot's own victim, slot %d) f=%llu%c", g_shot_victim,
                                   (unsigned long long)s_frame_count, 10);
          return 0; } }
    float cy, cp, cd;
    { void dc_nots_cam_get(float*, float*, float*); dc_nots_cam_get(&cy, &cp, &cd); }
    extern float g_dc_cam_eye[3];
    float px = g_dc_cam_eye[0], pz = g_dc_cam_eye[2];
    float fx = sinf(cy), fz = cosf(cy);
    float ex = (float)(int16_t)(r[a+32] | (r[a+33] << 8));
    float ez = (float)(int16_t)(r[a+36] | (r[a+37] << 8));
    float dx = ex - px, dz = ez - pz;
    float ahead = dx * fx + dz * fz;
    float lat = fabsf(dx * fz - dz * fx);

    extern int dc_aim_geom_hit(float rad);
    extern int dc_aim_geom_ray(uint32_t actor);
    static int rayen = -1; static float core = -1.f;
    if (rayen < 0) { const char *e = getenv("DC_AIM_RAY"); rayen = (e && *e == '0') ? 0 : 1;
                     e = getenv("DC_AIM_CORE"); core = e ? (float)atof(e) : 0.f;
                     if (core < 0.f) core = 0.f; }
    int ray  = rayen ? dc_aim_geom_ray(a) : 0;
    int geom = rayen ? ray : dc_aim_geom_hit(14.f);
    int miss = (ahead < 0.f) || (!geom && (core <= 0.f || lat > core));
    { static long n = 0;
      if (++n < 200)
          fprintf(stderr, "missgate: %s ray=%d geom=%d lat=%.0f ahead=%.0f slot=%d%c",
                  miss ? "MISS" : "hit", ray, geom, lat, ahead, (int)((a - 0xB7ACCu) / 608u), 10); }
    (void)tpsr;
    return miss;
}
void dc_aim_retarget(CPUState *cpu) {
    static int en = -1; static float cone = 0.85f;
    (void)cpu;
    g_aim_has_target = 0;
    if (en < 0) { const char *e = getenv("DC_AIM_CROSSHAIR"); en = (e && *e == '0') ? 0 : 1;
                  e = getenv("DC_AIM_CONE"); if (e) cone = (float)atof(e); }
    if (!en) return;
    { extern int dc_nots_active(void); if (!dc_nots_active()) return; }
    { extern int dc_cut_camera(void); if (dc_cut_camera()) return; }
    uint8_t *r = memory_get_ram_ptr(); if (!r) return;

    int firing = dc_fire_recent();
    if (r[0xB92C8] != 1) return;
    if (r[0xB92C9] != 6 && !firing) return;
    { extern uint64_t s_frame_count; g_aim_game_pick = r[0xB94B0]; g_aim_pick_frame = s_frame_count; }
    r[0xB94CD] = 0;
    float cy, cp, cd; { void dc_nots_cam_get(float*, float*, float*); dc_nots_cam_get(&cy, &cp, &cd); }

    extern float g_dc_cam_eye[3];
    float px = g_dc_cam_eye[0];
    float pz = g_dc_cam_eye[2];
    float fx = sinf(cy), fz = cosf(cy);

    static int tps = -1; static float tpsr = 300.f;
    if (tps < 0) { const char *e = getenv("DC_AIM_TPS"); tps = (e && *e == '0') ? 0 : 1;
                   e = getenv("DC_AIM_TPS_R"); if (e) tpsr = (float)atof(e); }
    int best = -1; float bestscore = -1e9f, bestlat = 1e9f;
    int spare = -1;
    for (int i = 0; i < 10; i++) {
        uint32_t a = 0xB7ACCu + (uint32_t)i * 608u;
        uint32_t alive = (uint32_t)r[a+48] | ((uint32_t)r[a+49] << 8) |
                         ((uint32_t)r[a+50] << 16) | ((uint32_t)r[a+51] << 24);
        if (!(alive & 1u)) { if (spare < 0) spare = i; continue; }
        int hp = (int16_t)(r[a+280] | (r[a+281] << 8));
        if (hp <= 0) { if (spare < 0) spare = i; continue; }
        float ex = (float)(int16_t)(r[a+32] | (r[a+33] << 8));
        float ez = (float)(int16_t)(r[a+36] | (r[a+37] << 8));
        float dx = ex - px, dz = ez - pz;
        float d = sqrtf(dx*dx + dz*dz);
        if (d < 1.f) continue;
        if (tps) {
            float ahead = dx * fx + dz * fz;
            if (ahead < 0.f) continue;
            float lat = fabsf(dx * fz - dz * fx);
            if (lat > tpsr) continue;
            float score = -lat - d * 0.05f;
            if (score > bestscore) { bestscore = score; best = i; bestlat = lat; }
        } else {
            float dot = (dx * fx + dz * fz) / d;
            if (dot < cone) continue;
            float score = dot - d * 0.00002f;
            if (score > bestscore) { bestscore = score; best = i; }
        }
    }

    int pick_on = 0;
    { static int pick = -1;
      if (pick < 0) { const char *e = getenv("DC_AIM_PICK"); pick = (e && *e == '0') ? 0 : 1; }
      if (pick) {
          extern uint32_t dc_aim_geom_first(float *depth);
          float dep = 0.f;
          uint32_t a = dc_aim_geom_first(&dep);
          pick_on = 1; best = -1; bestlat = 0.f; bestscore = 0.f;
          if (a >= 0xB7ACCu && a < 0xB7ACCu + 10u * 608u && ((a - 0xB7ACCu) % 608u) == 0u) {
              int k = (int)((a - 0xB7ACCu) / 608u);
              uint32_t al = (uint32_t)r[a+48] | ((uint32_t)r[a+49] << 8);
              int hp = (int16_t)(r[a+280] | (r[a+281] << 8));
              if ((al & 1u) && hp > 0) { best = k; bestscore = -dep; }
          }
      } }
    { static int dbg = -1; static long n = 0;
      if (dbg < 0) { const char *e = getenv("DC_AIM_DBG"); dbg = (e && *e == '1') ? 1 : 0; }

      { static long an = 0;
        if ((++an % 60) == 1)
            fprintf(stderr, "aimst: best=%d spare=%d eye=%.0f,%.0f fwd=%.2f,%.2f%c",
                    best, spare, px, pz, fx, fz, 10); }

      if (best >= 0 && !pick_on) {
          extern int dc_aim_geom_hit(float rad);
          if (!(dc_aim_geom_hit(14.f) || bestlat < 120.f)) best = -1;
      }
      g_aim_our_pick = best;
      if (best >= 0) {
          r[0xB94B0] = (uint8_t)best;
          g_aim_has_target = 1;
          if (dbg && (++n % 30) == 1)
              fprintf(stderr, "aim: crosshair target slot %d (score=%.1f)%c", best, (double)bestscore, 10);
      } else if (tps && spare >= 0) {

          extern int dc_fire_recent(void);
          if (dc_fire_recent()) {
              r[0xB94B0] = (uint8_t)spare;
              if (dbg && (++n % 30) == 1)
                  fprintf(stderr, "aim: MISS (no enemy under crosshair) -> empty slot %d%c", spare, 10);
          }
      }
    }
}

void dc_aim_point_hook(uint32_t player) {
    static int en = -1; static long nlog = 0;
    if (en < 0) { const char *e = getenv("DC_AIM_POINT"); en = (e && *e == '0') ? 0 : 1; }
    if (!en || (player & 0x1FFFFFu) != 0xB928Cu) return;
    { extern int dc_nots_active(void); if (!dc_nots_active()) return; }
    { extern int dc_cut_camera(void); if (dc_cut_camera()) return; }
    uint8_t *r = memory_get_ram_ptr(); if (!r) return;
    if (r[0xB92C8] != 1) return;
    if (r[0xB92C9] != 6 && !dc_fire_recent()) return;
    extern float g_dc_cam_eye[3];
    void dc_nots_cam_get(float*, float*, float*);
    float cy, cp, cd; dc_nots_cam_get(&cy, &cp, &cd);
    float o[3] = { g_dc_cam_eye[0], g_dc_cam_eye[1], g_dc_cam_eye[2] };
    float d[3] = { sinf(cy) * cosf(cp), sinf(cp), cosf(cy) * cosf(cp) };
    float dist = 6000.f; const char *why = "far";
    {   extern uint32_t dc_aim_geom_first(float *depth);
        float dep = 0.f;
        if (dc_aim_geom_first(&dep) && dep > 1.f) { dist = dep; why = "body"; }
        else {
            extern int dc_impact_raycast(const float o[3], const float d[3], float maxd, float hit[3], float nrm[3]);
            float hp[3], nn[3];
            if (dc_impact_raycast(o, d, 12000.f, hp, nn)) {
                float ex = hp[0] - o[0], ey = hp[1] - o[1], ez = hp[2] - o[2];
                float wd = sqrtf(ex*ex + ey*ey + ez*ez);
                if (wd > 200.f) { dist = wd; why = "wall"; }
            }
        }
    }
    int16_t pt[3];
    for (int k = 0; k < 3; k++) {
        float v = o[k] + d[k] * dist;
        if (v > 32000.f) v = 32000.f; if (v < -32000.f) v = -32000.f;
        pt[k] = (int16_t)(v >= 0.f ? v + 0.5f : v - 0.5f);
    }
    int16_t was[3] = { (int16_t)(r[0xB9488] | (r[0xB9489] << 8)), (int16_t)(r[0xB948A] | (r[0xB948B] << 8)),
                       (int16_t)(r[0xB948C] | (r[0xB948D] << 8)) };
    for (int k = 0; k < 3; k++) { r[0xB9488 + 2*k] = (uint8_t)(pt[k] & 0xFF); r[0xB9489 + 2*k] = (uint8_t)((uint16_t)pt[k] >> 8); }
    if (++nlog % 60 == 1)
        fprintf(stderr, "aimpoint: game target point %d,%d,%d -> crosshair %d,%d,%d (%s %.0f) - DC_AIM_POINT=0 off%c",
                was[0], was[1], was[2], pt[0], pt[1], pt[2], why, dist, 10);
}

typedef struct { uint32_t addr; int16_t q[8]; unsigned long long vb; long hits; unsigned long long arm_vb; long arms; } DcHazRec;
static DcHazRec s_haz;
void dc_hazard_note(CPUState *cpu, uint32_t rec) {
    extern uint64_t s_frame_count;
    { static long n0 = 0; if (++n0 <= 5) fprintf(stderr, "hazrec: HOOK call #%ld rec=%08X%c", n0, rec, 10); }
    if (!rec) return;
    const uint8_t *ram = memory_get_ram_ptr(); if (!ram) return;

    if (ram[0xB9403] == 0) { s_haz.arm_vb = (unsigned long long)s_frame_count; s_haz.arms++; }
    uint32_t a = rec & 0x1FFFFFu;
    if (a + 20 >= 0x200000u) return;
    s_haz.addr = rec;
    for (int i = 0; i < 8; i++)
        s_haz.q[i] = (int16_t)(ram[a + (uint32_t)i * 2u] | (ram[a + (uint32_t)i * 2u + 1u] << 8));
    s_haz.vb = (unsigned long long)s_frame_count;
    s_haz.hits++;
    { static int dbg = -1; static long n = 0;
      if (dbg < 0) { const char *e = getenv("DC_HAZARD_DBG"); dbg = (e && *e == '1') ? 1 : 0; }
      if (dbg && (++n % 30) == 1)
          fprintf(stderr, "hazrec: %08X type=%u flags=%u floor=%u on=%u quad=%d,%d %d,%d %d,%d %d,%d%c",
                  rec, ram[a+16], ram[a+17], ram[a+18], ram[a+19],
                  s_haz.q[0], s_haz.q[1], s_haz.q[2], s_haz.q[3],
                  s_haz.q[4], s_haz.q[5], s_haz.q[6], s_haz.q[7], 10); }
}

int dc_hazard_recent(unsigned long long now_vb, unsigned long long window) {
    return s_haz.addr && s_haz.arms > 0 && now_vb >= s_haz.arm_vb && now_vb - s_haz.arm_vb <= window;
}

int dc_hazard_escape_dir(int px, int pz, unsigned long long now_vb, float *ox, float *oz) {
    if (!s_haz.addr || now_vb - s_haz.vb > 120ull) return 0;
    float best = 1e30f, bx = 0.f, bz = 0.f;
    for (int i = 0; i < 4; i++) {
        float ax = (float)s_haz.q[i * 2], az = (float)s_haz.q[i * 2 + 1];
        float bx2 = (float)s_haz.q[((i + 1) & 3) * 2], bz2 = (float)s_haz.q[((i + 1) & 3) * 2 + 1];
        float ex = bx2 - ax, ez = bz2 - az;
        float el = sqrtf(ex * ex + ez * ez);
        if (el < 1.f) continue;
        float t = ((px - ax) * ex + (pz - az) * ez) / (el * el);
        if (t < 0.f) t = 0.f; if (t > 1.f) t = 1.f;
        float cx = ax + ex * t, cz = az + ez * t;
        float dx = (float)px - cx, dz = (float)pz - cz;
        float d = sqrtf(dx * dx + dz * dz);
        if (d < best) {
            best = d;

            float nx = -ez / el, nz = ex / el;
            float mx = 0.f, mz = 0.f;
            for (int k = 0; k < 4; k++) { mx += (float)s_haz.q[k * 2]; mz += (float)s_haz.q[k * 2 + 1]; }
            mx *= 0.25f; mz *= 0.25f;
            if ((cx - mx) * nx + (cz - mz) * nz < 0.f) { nx = -nx; nz = -nz; }
            bx = nx; bz = nz;
        }
    }
    if (bx == 0.f && bz == 0.f) return 0;
    *ox = bx; *oz = bz;
    return 1;
}

static int     s_snd_cam_valid = 0;
static int16_t s_snd_cam_pos[3], s_snd_cam_yaw;
static int snd_cam_enabled(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("DC_SND_CAM"); on = (e && *e == '0') ? 0 : 1; }
    return on;
}
static void snd_cam_publish(const float eye[3], const float tgt[3]) {
    extern int dc_nots_active(void);
    const uint8_t *r = memory_get_ram_ptr();
    s_snd_cam_valid = 0;
    if (!snd_cam_enabled() || !dc_nots_active() || !r || r[0xB92C8] == 4) return;

    float wx = eye[0], wy = eye[1], wz = eye[2];
    float dx = tgt[0] - eye[0], dz = tgt[2] - eye[2];
    if (dx*dx + dz*dz < 1.f) return;
    float yaw = atan2f(dx, dz) * (4096.f / 6.2831853f);
    int iy = (int)lroundf(yaw) & 4095;
    if (wx < -32000.f || wx > 32000.f || wz < -32000.f || wz > 32000.f) return;
    s_snd_cam_pos[0] = (int16_t)lroundf(wx);
    s_snd_cam_pos[1] = (int16_t)lroundf(wy);
    s_snd_cam_pos[2] = (int16_t)lroundf(wz);
    s_snd_cam_yaw    = (int16_t)iy;
    s_snd_cam_valid  = 1;
}

int dc_snd_listener(CPUState *cpu, uint32_t actor_pos, int32_t *dist, int32_t *angle, int32_t *yaw) {
    (void)cpu;
    if (!s_snd_cam_valid) return 0;
    const uint8_t *r = memory_get_ram_ptr(); if (!r) return 0;
    uint32_t a = actor_pos & 0x1FFFFFu;
    if (a + 6u > 0x200000u) return 0;
    float ax = (float)(int16_t)(r[a]   | (r[a+1] << 8));
    float ay = (float)(int16_t)(r[a+2] | (r[a+3] << 8));
    float az = (float)(int16_t)(r[a+4] | (r[a+5] << 8));
    float dx = ax - (float)s_snd_cam_pos[0];
    float dy = ay - (float)s_snd_cam_pos[1];
    float dz = az - (float)s_snd_cam_pos[2];
    float d = sqrtf(dx*dx + dy*dy + dz*dz);
    if (d > 2.0e9f) d = 2.0e9f;
    *dist  = (int32_t)d;
    *angle = ((int32_t)lroundf(atan2f(dx, dz) * (4096.f / 6.2831853f))) & 4095;
    *yaw   = (int32_t)s_snd_cam_yaw;
    { static int st = -1; static long n = 0;
      if (st < 0) { const char *e = getenv("DC_SND_STAT"); st = (e && *e == '1') ? 1 : 0; }
      if (st && (++n % 20) == 1)
          fprintf(stderr, "sndcam: #%ld actor=(%.0f,%.0f,%.0f) listener=(%d,%d,%d yaw %d) dist=%d bearing=%d rel=%d%c",
                  n, ax, ay, az, s_snd_cam_pos[0], s_snd_cam_pos[1], s_snd_cam_pos[2], s_snd_cam_yaw,
                  *dist, *angle, (*angle - *yaw) & 4095, 10); }
    return 1;
}

int dc_body_fade_allowed(void) {
    extern int dc_nots_active(void);
    static int en = -1;
    if (en < 0) { const char *e = getenv("DC_BODY_FADE"); en = (e && *e == '0') ? 0 : 1; }
    const uint8_t *r = memory_get_ram_ptr();
    if (!en || !dc_nots_active()) return 0;
    if (r && r[0xB92C8] == 4) return 0;
    return 1;
}

int g_dc_cam_hide_regina = 0;
int dc_regina_hidden(void) {
    if (!g_dc_cam_hide_regina) return 0;
    extern uint64_t s_frame_count;
    static unsigned long long fr = ~0ull; static int v = 0;
    if ((unsigned long long)s_frame_count == fr) return v;
    fr = (unsigned long long)s_frame_count;
    extern int dc_nots_active(void); extern int dc_scene_flag_on(void); extern int dc_regina_dead(void);
    v = (dc_nots_active() && ram[0xB92C8] != 4 && !dc_scripted_now() && !dc_scene_flag_on() && !dc_cut_camera() &&
         !dc_regina_dead() && !ram[0xB93AA] && !ram[0xB93DE]) ? 1 : 0;
    { static int lv = -1;
      if (v != lv) { lv = v; fprintf(stderr, "hide regina: %s%c", v ? "hidden (CAMERA -> Hide Regina)" : "shown (option off, a scene, a grab or her death)", 10); } }
    return v;
}

#define CAMMESH_MAX 20000
static float  s_cammesh[CAMMESH_MAX][9];
static int    s_cammesh_n = 0;
static int    s_cammesh_room = -2;

static float    s_camall[CAMMESH_MAX][9];
static uint8_t  s_camall_tag[CAMMESH_MAX];
static int      s_camall_n = 0;
#define CAMTAG_MAX 31
static uint32_t s_camtag_grp[CAMTAG_MAX + 1][8];
static uint8_t  s_camtag_ng[CAMTAG_MAX + 1];
static int      s_camtag_n = 0;
static uint32_t s_camtag_mask = 0xFFFFFFFFu;
static int camtag_env(void) { static int v = -1; if (v < 0) { const char *e = getenv("DC_CAMCOLL_TAGS"); v = (e && *e == '0') ? 0 : 1; } return v; }

static int camtag_parse(const char *name) {
    if (!camtag_env()) return 0;
    uint32_t g[8]; int ng = 0;
    const char *p = name;
    while (*p && ng < 8) {
        const char *q = strstr(p, "__");
        if (!q) break;
        q += 2;
        while (*q == '_') q++;
        int n = 0; uint32_t v = 0;
        while (n < 7) {
            const char c = q[n]; const char lc = (char)(c | 32);
            if (c >= '0' && c <= '9') v = v * 16u + (uint32_t)(c - '0');
            else if (lc >= 'a' && lc <= 'f') v = v * 16u + (uint32_t)(lc - 'a' + 10);
            else break;
            n++;
        }
        const char after = q[n];
        if (n == 6 && v >= 0x100000u && v <= 0x1FFFFFu && (after == 0 || after == '_' || after == ' ' || after == 9 || after == 10 || after == 13)) {
            int dup = 0; for (int i = 0; i < ng; i++) if (g[i] == v) dup = 1;
            if (!dup) g[ng++] = v;
        }
        p = q;
    }
    if (ng == 0) return 0;
    for (int t = 1; t <= s_camtag_n; t++) {
        if (s_camtag_ng[t] != ng) continue;
        int same = 1;
        for (int i = 0; i < ng && same; i++) { int hit = 0; for (int j = 0; j < ng; j++) if (s_camtag_grp[t][j] == g[i]) hit = 1; same = hit; }
        if (same) return t;
    }
    if (s_camtag_n >= CAMTAG_MAX) return 0;
    const int t = ++s_camtag_n;
    s_camtag_ng[t] = (uint8_t)ng; for (int i = 0; i < ng; i++) s_camtag_grp[t][i] = g[i];
    return t;
}

static void cammesh_tags_tick(void) {
    if (s_camall_n <= 0) return;
    uint32_t mask = 0xFFFFFFFFu;
    if (s_camtag_n > 0 && camtag_env()) {
        mask = 1u;
        const uint8_t *sp = memory_get_scratchpad_ptr();
        const uint32_t ctx = sp ? (((uint32_t)sp[0] | ((uint32_t)sp[1] << 8) | ((uint32_t)sp[2] << 16) | ((uint32_t)sp[3] << 24)) & 0x1FFFFFu) : 0u;
        int any = 0;
        if (ram && ctx >= 0x1000u && ctx + 0x8FFCu < RAM_SIZE) {
            for (int t = 1; t <= s_camtag_n; t++) {
                int on = 0;
                for (int k = 0; k < 36 && !on; k++) {
                    const uint32_t e = ctx + 0x7CDCu + 136u * (uint32_t)k;
                    if ((ram[e + 48u] & 3u) != 3u) continue;
                    const uint32_t gp = ((uint32_t)ram[e + 60u] | ((uint32_t)ram[e + 61u] << 8) | ((uint32_t)ram[e + 62u] << 16)) & 0x1FFFFFu;
                    for (int i = 0; i < s_camtag_ng[t]; i++) if (s_camtag_grp[t][i] == gp) { on = 1; break; }
                }
                if (on) { mask |= 1u << t; any = 1; }
            }
        }
        if (!any) mask = s_camtag_mask ? s_camtag_mask : 0xFFFFFFFFu;
    }
    if (mask == s_camtag_mask && s_cammesh_n > 0) return;
    int n = 0;
    for (int i = 0; i < s_camall_n; i++) {
        if (!((mask >> s_camall_tag[i]) & 1u)) continue;
        for (int q = 0; q < 9; q++) s_cammesh[n][q] = s_camall[i][q];
        n++;
    }
    s_cammesh_n = n; s_camtag_mask = mask;
    if (s_camtag_n > 0) {
        static int nlog = 0;
        if (++nlog <= 60) {
            char on[128]; int ln = 0; on[0] = 0;
            for (int t = 1; t <= s_camtag_n && ln < 110; t++)
                if ((mask >> t) & 1u) ln += snprintf(on + ln, sizeof on - (size_t)ln, " %06X%s", s_camtag_grp[t][0], s_camtag_ng[t] > 1 ? "+" : "");
            fprintf(stderr, "camera collision: room %d - parts on by the room groups shown:%s -> %d of %d triangles (DC_CAMCOLL_TAGS=0 - all)%c",
                    s_cammesh_room, ln ? on : " (untagged only)", n, s_camall_n, 10);
        }
    }
}

static int cammesh_source_name(int room, char *out, size_t cap) {
    static char names[256][16];
    static int loaded = 0;
    if (!loaded) {
        loaded = 1;
        memset(names, 0, sizeof names);
        const char *lib = getenv("DC_ROOM_LIBRARY");
        if (lib && *lib) {
            char mf[512];
            snprintf(mf, sizeof mf, "%s/manifest.txt", lib);
            DcMemFile *f = dc_mf_open(mf);
            if (f) {
                char line[256];
                while (dc_mf_gets(line, sizeof line, f)) {
                    int idx = -1; char st[32];
                    if (sscanf(line, "room_%d.dcroom %31s", &idx, st) == 2 &&
                        idx >= 0 && idx < 256)
                        snprintf(names[idx], sizeof names[idx], "%s", st);
                }
                dc_mf_close(f);
            }
        }
    }
    if (room < 0 || room >= 256 || !names[room][0]) return 0;
    snprintf(out, cap, "%s", names[room]);
    return 1;
}

static int camfb_enabled(void);
static void cammesh_load(int room) {
    if (room == s_cammesh_room) return;
    s_cammesh_room = room; s_cammesh_n = 0;
    s_camall_n = 0; s_camtag_n = 0; s_camtag_mask = 0xFFFFFFFFu;
    if (room < 0) return;

    char dir[400];
    const char *cd = getenv("DC_ROOM_COLLISION");
    if (cd && *cd) snprintf(dir, sizeof dir, "%s", cd);
    else {
        const char *lib = getenv("DC_ROOM_LIBRARY");
        dir[0] = 0;
        if (lib && *lib) {
            snprintf(dir, sizeof dir, "%s", lib);
            for (int i = (int)strlen(dir) - 1; i > 0; i--)
                if (dir[i] == '/' || dir[i] == 92) { dir[i] = 0; break; }
            strncat(dir, "/collision", sizeof dir - strlen(dir) - 1);
        } else snprintf(dir, sizeof dir, "exported_rooms/collision");
    }

    char src_name[32]; int have_src = cammesh_source_name(room, src_name, sizeof src_name);

    char path[512]; DcMemFile *f = NULL;
    snprintf(path, sizeof path, "%s/room_%03d.obj", dir, room);
    f = dc_mf_open(path);
    if (!f && have_src) { snprintf(path, sizeof path, "%s/%s_C.obj", dir, src_name); f = dc_mf_open(path); }
    if (!f && have_src) { snprintf(path, sizeof path, "%s/%s.obj",  dir, src_name); f = dc_mf_open(path); }
    if (!f) {
        fprintf(stderr, "camera collision: room %d (%s) - no file in %s -> %s%c",
                room, have_src ? src_name : "?", dir,
                camfb_enabled() ? "falling back to the room geometry" : "no camera collision", 10);
        return;
    }

    static float vx[200000], vy[200000], vz[200000];
    int nv = 0, nt = 0; char line[1024];
    int cur_tag = 0;
    s_camtag_n = 0; s_camtag_mask = 0xFFFFFFFFu;
    while (dc_mf_gets(line, sizeof line, f)) {
        if (line[0] == 'g' && (line[1] == ' ' || line[1] == 9)) {
            cur_tag = camtag_parse(line + 2);
        } else if (line[0] == 'v' && line[1] == ' ') {
            float a, b, c;
            if (sscanf(line + 2, "%f %f %f", &a, &b, &c) == 3 && nv < 200000) {
                vx[nv] = a; vy[nv] = b; vz[nv] = c; nv++;
            }
        } else if (line[0] == 'f' && line[1] == ' ') {

            int idx[64], nidx = 0;
            const char *p = line + 2;
            while (*p && nidx < 64) {
                while (*p == ' ' || *p == 9) p++;
                if (*p < '0' && *p != '-') break;
                int v = atoi(p);
                if (v < 0) v = nv + 1 + v;
                if (v >= 1 && v <= nv) idx[nidx++] = v;
                while (*p && *p != ' ' && *p != 9 && *p != 10 && *p != 13) p++;
            }
            for (int k = 1; k + 1 < nidx && nt < CAMMESH_MAX; k++) {
                float *m = s_camall[nt];
                int a = idx[0]-1, b = idx[k]-1, c = idx[k+1]-1;
                m[0]=vx[a]; m[1]=vy[a]; m[2]=vz[a];
                m[3]=vx[b]; m[4]=vy[b]; m[5]=vz[b];
                m[6]=vx[c]; m[7]=vy[c]; m[8]=vz[c];
                s_camall_tag[nt] = (uint8_t)cur_tag;
                nt++;
            }
        }
    }
    dc_mf_close(f);
    s_camall_n = nt;
    fprintf(stderr, "camera collision: room %d -> %d triangles from %s%s%c", room, nt, path,
            s_camtag_n ? " (parts tagged by room groups - on while the game shows them, DC_CAMCOLL_TAGS=0 - all always)" : "", 10);
    s_camtag_mask = 0u;
    cammesh_tags_tick();
}

int dc_camcoll_get(const float **tris) { *tris = (const float*)s_cammesh; return s_cammesh_n; }

static float cam_pivot_clear(void) {
    static float v = -1.f;
    if (v < 0.f) { const char *e = getenv("DC_CAM_PIVOT_CLEAR"); v = e ? (float)atof(e) : 150.f; if (v < 0.f) v = 0.f; }
    return v;
}
static float cammesh_ray_tmin(const float (*tris)[9], int ntri, const float o[3], const float d[3], float tmin);
static float cammesh_ray_t(const float (*tris)[9], int ntri, const float o[3], const float d[3]) {
    return cammesh_ray_tmin(tris, ntri, o, d, 0.02f);
}

static float cammesh_ray_pivot(const float (*tris)[9], int ntri, const float o[3], const float d[3]) {
    float dl = sqrtf(d[0]*d[0] + d[1]*d[1] + d[2]*d[2]);
    float tmin = (dl > 1.f) ? cam_pivot_clear() / dl : 0.02f;
    if (tmin < 0.02f) tmin = 0.02f;
    return cammesh_ray_tmin(tris, ntri, o, d, tmin);
}
static float cammesh_ray_tmin(const float (*tris)[9], int ntri, const float o[3], const float d[3], float tmin) {
    float best = 1.f;
    float bx0 = o[0] < o[0]+d[0] ? o[0] : o[0]+d[0], bx1 = o[0] > o[0]+d[0] ? o[0] : o[0]+d[0];
    float by0 = o[1] < o[1]+d[1] ? o[1] : o[1]+d[1], by1 = o[1] > o[1]+d[1] ? o[1] : o[1]+d[1];
    float bz0 = o[2] < o[2]+d[2] ? o[2] : o[2]+d[2], bz1 = o[2] > o[2]+d[2] ? o[2] : o[2]+d[2];
    for (int t = 0; t < ntri; t++) {
        const float *m = tris[t];
        if ((m[0] < bx0 && m[3] < bx0 && m[6] < bx0) || (m[0] > bx1 && m[3] > bx1 && m[6] > bx1) ||
            (m[1] < by0 && m[4] < by0 && m[7] < by0) || (m[1] > by1 && m[4] > by1 && m[7] > by1) ||
            (m[2] < bz0 && m[5] < bz0 && m[8] < bz0) || (m[2] > bz1 && m[5] > bz1 && m[8] > bz1)) continue;
        float e1[3] = { m[3]-m[0], m[4]-m[1], m[5]-m[2] };
        float e2[3] = { m[6]-m[0], m[7]-m[1], m[8]-m[2] };
        float p[3] = { d[1]*e2[2]-d[2]*e2[1], d[2]*e2[0]-d[0]*e2[2], d[0]*e2[1]-d[1]*e2[0] };
        float det = e1[0]*p[0] + e1[1]*p[1] + e1[2]*p[2];
        if (det > -1e-6f && det < 1e-6f) continue;
        float inv = 1.f/det;
        float s[3] = { o[0]-m[0], o[1]-m[1], o[2]-m[2] };
        float u = (s[0]*p[0] + s[1]*p[1] + s[2]*p[2]) * inv;
        if (u < 0.f || u > 1.f) continue;
        float q[3] = { s[1]*e1[2]-s[2]*e1[1], s[2]*e1[0]-s[0]*e1[2], s[0]*e1[1]-s[1]*e1[0] };
        float v = (d[0]*q[0] + d[1]*q[1] + d[2]*q[2]) * inv;
        if (v < 0.f || u+v > 1.f) continue;
        float tt = (e2[0]*q[0] + e2[1]*q[1] + e2[2]*q[2]) * inv;
        if (tt > tmin && tt < best) best = tt;
    }
    return best;
}

static float sweep_sphere_point(const float o[3], const float d[3], const float c[3], float R) {
    float f[3] = { o[0]-c[0], o[1]-c[1], o[2]-c[2] };
    float a = d[0]*d[0] + d[1]*d[1] + d[2]*d[2];
    if (a < 1e-9f) return 1.f;
    float b = 2.f*(f[0]*d[0] + f[1]*d[1] + f[2]*d[2]);
    float cc = f[0]*f[0] + f[1]*f[1] + f[2]*f[2] - R*R;

    if (cc <= 0.f) return 1.f;
    float disc = b*b - 4.f*a*cc;
    if (disc < 0.f) return 1.f;
    float t = (-b - sqrtf(disc)) / (2.f*a);
    return (t >= 0.f && t <= 1.f) ? t : 1.f;
}

static float sweep_sphere_seg(const float o[3], const float d[3],
                              const float p[3], const float q[3], float R) {
    float ax[3] = { q[0]-p[0], q[1]-p[1], q[2]-p[2] };
    float al2 = ax[0]*ax[0] + ax[1]*ax[1] + ax[2]*ax[2];
    if (al2 < 1e-9f) return 1.f;
    float f[3] = { o[0]-p[0], o[1]-p[1], o[2]-p[2] };
    float dA = (d[0]*ax[0] + d[1]*ax[1] + d[2]*ax[2]) / al2;
    float fA = (f[0]*ax[0] + f[1]*ax[1] + f[2]*ax[2]) / al2;

    float dp[3] = { d[0]-ax[0]*dA, d[1]-ax[1]*dA, d[2]-ax[2]*dA };
    float fp[3] = { f[0]-ax[0]*fA, f[1]-ax[1]*fA, f[2]-ax[2]*fA };
    float a = dp[0]*dp[0] + dp[1]*dp[1] + dp[2]*dp[2];
    if (a < 1e-9f) return 1.f;
    float b = 2.f*(fp[0]*dp[0] + fp[1]*dp[1] + fp[2]*dp[2]);
    float cc = fp[0]*fp[0] + fp[1]*fp[1] + fp[2]*fp[2] - R*R;
    if (cc <= 0.f) return 1.f;
    float disc = b*b - 4.f*a*cc;
    if (disc < 0.f) return 1.f;
    float t = (-b - sqrtf(disc)) / (2.f*a);
    if (t < 0.f || t > 1.f) return 1.f;
    float s = fA + dA*t;
    return (s >= 0.f && s <= 1.f) ? t : 1.f;
}

static float cam_pt_seg_d2(const float p[3], const float a[3], const float b[3]) {
    float ab[3] = { b[0]-a[0], b[1]-a[1], b[2]-a[2] };
    float ap[3] = { p[0]-a[0], p[1]-a[1], p[2]-a[2] };
    float L = ab[0]*ab[0] + ab[1]*ab[1] + ab[2]*ab[2];
    float t = (L > 1e-9f) ? (ap[0]*ab[0] + ap[1]*ab[1] + ap[2]*ab[2]) / L : 0.f;
    if (t < 0.f) t = 0.f; else if (t > 1.f) t = 1.f;
    float c[3] = { a[0] + ab[0]*t - p[0], a[1] + ab[1]*t - p[1], a[2] + ab[2]*t - p[2] };
    return c[0]*c[0] + c[1]*c[1] + c[2]*c[2];
}

static float cammesh_hit_t(const float (*tris)[9], int ntri, const float o[3], const float e[3]) {
    float d[3] = { e[0]-o[0], e[1]-o[1], e[2]-o[2] };
    float best = cammesh_ray_pivot(tris, ntri, o, d);
    const float dlen  = sqrtf(d[0]*d[0] + d[1]*d[1] + d[2]*d[2]);
    const float pclr  = cam_pivot_clear();

    static float rad = -1.f;
    if (rad < 0.f) { const char *r = getenv("DC_CAM_RADIUS_H"); rad = r ? (float)atof(r) : 300.f; }
    if (rad <= 1.f) return best;

    float bx0 = (o[0] < e[0] ? o[0] : e[0]) - rad, bx1 = (o[0] > e[0] ? o[0] : e[0]) + rad;
    float by0 = (o[1] < e[1] ? o[1] : e[1]) - rad, by1 = (o[1] > e[1] ? o[1] : e[1]) + rad;
    float bz0 = (o[2] < e[2] ? o[2] : e[2]) - rad, bz1 = (o[2] > e[2] ? o[2] : e[2]) + rad;

    for (int t = 0; t < ntri; t++) {
        const float *m = tris[t];
        const float *v0 = &m[0], *v1 = &m[3], *v2 = &m[6];
        {
            float tx0 = v0[0], tx1 = v0[0], ty0 = v0[1], ty1 = v0[1], tz0 = v0[2], tz1 = v0[2];
            if (v1[0] < tx0) tx0 = v1[0]; if (v1[0] > tx1) tx1 = v1[0];
            if (v2[0] < tx0) tx0 = v2[0]; if (v2[0] > tx1) tx1 = v2[0];
            if (v1[1] < ty0) ty0 = v1[1]; if (v1[1] > ty1) ty1 = v1[1];
            if (v2[1] < ty0) ty0 = v2[1]; if (v2[1] > ty1) ty1 = v2[1];
            if (v1[2] < tz0) tz0 = v1[2]; if (v1[2] > tz1) tz1 = v1[2];
            if (v2[2] < tz0) tz0 = v2[2]; if (v2[2] > tz1) tz1 = v2[2];
            if (tx1 < bx0 || tx0 > bx1 || ty1 < by0 || ty0 > by1 || tz1 < bz0 || tz0 > bz1) continue;
        }
        float e1[3] = { v1[0]-v0[0], v1[1]-v0[1], v1[2]-v0[2] };
        float e2[3] = { v2[0]-v0[0], v2[1]-v0[1], v2[2]-v0[2] };
        float n[3] = { e1[1]*e2[2]-e1[2]*e2[1], e1[2]*e2[0]-e1[0]*e2[2], e1[0]*e2[1]-e1[1]*e2[0] };
        float nl = sqrtf(n[0]*n[0] + n[1]*n[1] + n[2]*n[2]);
        if (nl < 1e-6f) continue;
        n[0]/=nl; n[1]/=nl; n[2]/=nl;
        float tt = 1.f;

        float dist = (o[0]-v0[0])*n[0] + (o[1]-v0[1])*n[1] + (o[2]-v0[2])*n[2];
        float dn = d[0]*n[0] + d[1]*n[1] + d[2]*n[2];
        float side = dist >= 0.f ? 1.f : -1.f;
        float tf = -1.f;
        int against = (fabsf(dist) <= rad);
        if (against) tf = -1.f;
        else if (dn*side < -1e-9f) tf = (side*rad - dist) / dn;

        { static int ae = -1;
          if (ae < 0) { const char *e2 = getenv("DC_CAM_AGAINST_EDGES"); ae = (e2 && *e2 == '0') ? 0 : 1; }
          if (against && !ae) continue; }
        if (tf >= 0.f && tf <= 1.f) {
            float c[3] = { o[0]+d[0]*tf - n[0]*side*rad,
                           o[1]+d[1]*tf - n[1]*side*rad,
                           o[2]+d[2]*tf - n[2]*side*rad };

            float vp[3] = { c[0]-v0[0], c[1]-v0[1], c[2]-v0[2] };
            float d00 = e1[0]*e1[0]+e1[1]*e1[1]+e1[2]*e1[2];
            float d01 = e1[0]*e2[0]+e1[1]*e2[1]+e1[2]*e2[2];
            float d11 = e2[0]*e2[0]+e2[1]*e2[1]+e2[2]*e2[2];
            float d20 = vp[0]*e1[0]+vp[1]*e1[1]+vp[2]*e1[2];
            float d21 = vp[0]*e2[0]+vp[1]*e2[1]+vp[2]*e2[2];
            float den = d00*d11 - d01*d01;
            if (fabsf(den) > 1e-9f) {
                float u = (d11*d20 - d01*d21) / den;
                float v = (d00*d21 - d01*d20) / den;
                if (u >= 0.f && v >= 0.f && u+v <= 1.f) tt = tf;
            }
        }

        const float *vs[3] = { v0, v1, v2 };
        static int sg = -1;
        if (sg < 0) { const char *e2 = getenv("DC_CAM_START_GUARD");
                      sg = (e2 && *e2 == '0') ? 0 : 1; }
        const float rad2 = rad * rad;
        for (int k = 0; k < 3; k++) {
            const float *va = vs[k], *vb = vs[(k+1)%3];
            if (!sg || cam_pt_seg_d2(o, va, vb) > rad2) {
                float s = sweep_sphere_seg(o, d, va, vb, rad);
                if (s < tt) tt = s;
            }
            if (!sg || cam_pt_seg_d2(o, va, va) > rad2) {
                float s = sweep_sphere_point(o, d, va, rad);
                if (s < tt) tt = s;
            }
        }
        if (tt < 1.f && tt * dlen < pclr) tt = 1.f;
        if (tt > 0.001f && tt < best) best = tt;
    }
    return best;
}

static float cammesh_hit(const float o[3], const float e[3]) {
    return cammesh_hit_t(s_cammesh, s_cammesh_n, o, e);
}

#define CAMFB_MAX 24000
static float s_camfb[2][CAMFB_MAX][9];
static int   s_camfb_n[2] = { 0, 0 };
static int   s_camfb_r = 0;
static int   s_camfb_wn = 0;
static int camfb_enabled(void) {
    static int on = -1;

    if (on < 0) { const char *e = getenv("DC_CAM_FALLBACK"); on = (e && *e == '1') ? 1 : 0; }
    return on;
}
void dc_camfb_begin(void) { s_camfb_wn = 0; }
static uint8_t s_camfb_semi[2][CAMFB_MAX];
void dc_camfb_add(const float a[3], const float b[3], const float c[3]) {

    if (s_camfb_wn >= CAMFB_MAX) return;
    s_camfb_semi[s_camfb_r ^ 1][s_camfb_wn] = 0;
    float *m = s_camfb[s_camfb_r ^ 1][s_camfb_wn++];
    m[0]=a[0]; m[1]=a[1]; m[2]=a[2]; m[3]=b[0]; m[4]=b[1]; m[5]=b[2]; m[6]=c[0]; m[7]=c[1]; m[8]=c[2];
}
void dc_camfb_add_semi(const float a[3], const float b[3], const float c[3]) {
    if (s_camfb_wn >= CAMFB_MAX) return;
    s_camfb_semi[s_camfb_r ^ 1][s_camfb_wn] = 1;
    float *m = s_camfb[s_camfb_r ^ 1][s_camfb_wn++];
    m[0]=a[0]; m[1]=a[1]; m[2]=a[2]; m[3]=b[0]; m[4]=b[1]; m[5]=b[2]; m[6]=c[0]; m[7]=c[1]; m[8]=c[2];
}

static uint32_t s_camfb_gen = 0;
static uint32_t dc_camfb_gen(void) { return s_camfb_gen; }
void dc_camfb_commit(void) {
    if (s_camfb_wn <= 0) return;
    s_camfb_n[s_camfb_r ^ 1] = s_camfb_wn;
    s_camfb_r ^= 1;
    s_camfb_wn = 0;
    s_camfb_gen++;
}
int dc_camfb_count(void) { return s_camfb_n[s_camfb_r]; }

static float dc_items_ground_y(float x, float y, float z, int* found) {
    const float (*T)[9] = s_camfb[s_camfb_r];
    int n = s_camfb_n[s_camfb_r];
    float best = 1e9f;
    *found = 0;
    for (int i = 0; i < n; i++) {
        const float* t = T[i];
        float d1 = (t[3]-t[0])*(z-t[2]) - (t[5]-t[2])*(x-t[0]);
        float d2 = (t[6]-t[3])*(z-t[5]) - (t[8]-t[5])*(x-t[3]);
        float d3 = (t[0]-t[6])*(z-t[8]) - (t[2]-t[8])*(x-t[6]);
        int sneg = (d1 < 0.f) || (d2 < 0.f) || (d3 < 0.f);
        int spos = (d1 > 0.f) || (d2 > 0.f) || (d3 > 0.f);
        if (sneg && spos) continue;
        float ux = t[3]-t[0], uy = t[4]-t[1], uz = t[5]-t[2];
        float vx = t[6]-t[0], vy = t[7]-t[1], vz = t[8]-t[2];
        float nx = uy*vz - uz*vy, ny = uz*vx - ux*vz, nz = ux*vy - uy*vx;
        float alen = sqrtf(nx*nx + ny*ny + nz*nz);
        if (alen < 1e-3f || fabsf(ny) < 0.35f * alen) continue;
        float py = t[1] + (nx*(t[0]-x) + nz*(t[2]-z)) / ny;
        if (py >= y - 40.f && py < best) { best = py; *found = 1; }
    }
    return best;
}

static float dc_room_lowest_y(int* ntri) {
    const float (*T)[9] = s_camfb[s_camfb_r];
    const int n = s_camfb_n[s_camfb_r];
    float lo = -1e9f;
    for (int i = 0; i < n; i++) {
        const float* t = T[i];
        if (t[1] > lo) lo = t[1];
        if (t[4] > lo) lo = t[4];
        if (t[7] > lo) lo = t[7];
    }
    *ntri = n;
    return lo;
}

static float imp_ray_set(const float (*tris)[9], int ntri,
                         const float o[3], const float d[3], float best, float n_out[3]) {
    for (int t = 0; t < ntri; t++) {
        const float *m = tris[t];
        float e1[3] = { m[3]-m[0], m[4]-m[1], m[5]-m[2] };
        float e2[3] = { m[6]-m[0], m[7]-m[1], m[8]-m[2] };
        float p[3] = { d[1]*e2[2]-d[2]*e2[1], d[2]*e2[0]-d[0]*e2[2], d[0]*e2[1]-d[1]*e2[0] };
        float det = e1[0]*p[0] + e1[1]*p[1] + e1[2]*p[2];
        if (det > -1e-6f && det < 1e-6f) continue;
        float inv = 1.0f / det;
        float s[3] = { o[0]-m[0], o[1]-m[1], o[2]-m[2] };
        float u = (s[0]*p[0] + s[1]*p[1] + s[2]*p[2]) * inv;
        if (u < 0.f || u > 1.f) continue;
        float q[3] = { s[1]*e1[2]-s[2]*e1[1], s[2]*e1[0]-s[0]*e1[2], s[0]*e1[1]-s[1]*e1[0] };
        float v = (d[0]*q[0] + d[1]*q[1] + d[2]*q[2]) * inv;
        if (v < 0.f || u + v > 1.f) continue;
        float tt = (e2[0]*q[0] + e2[1]*q[1] + e2[2]*q[2]) * inv;
        if (tt <= 8.f || tt >= best) continue;
        best = tt;
        n_out[0] = e1[1]*e2[2]-e1[2]*e2[1];
        n_out[1] = e1[2]*e2[0]-e1[0]*e2[2];
        n_out[2] = e1[0]*e2[1]-e1[1]*e2[0];
    }
    return best;
}
int dc_impact_raycast(const float o[3], const float d[3], float maxd,
                      float hit[3], float nrm[3]) {
    float n[3] = { 0, 0, 0 };
    float best = maxd;
    best = imp_ray_set(s_cammesh, s_cammesh_n, o, d, best, n);
    best = imp_ray_set((const float (*)[9])s_camfb[s_camfb_r], s_camfb_n[s_camfb_r], o, d, best, n);
    if (best >= maxd) return 0;
    float nl = sqrtf(n[0]*n[0] + n[1]*n[1] + n[2]*n[2]);
    if (nl < 1e-6f) return 0;
    n[0] /= nl; n[1] /= nl; n[2] /= nl;
    if (n[0]*d[0] + n[1]*d[1] + n[2]*d[2] > 0.f) { n[0] = -n[0]; n[1] = -n[1]; n[2] = -n[2]; }
    for (int i = 0; i < 3; i++) { hit[i] = o[i] + d[i] * best; nrm[i] = n[i]; }
    return 1;
}

#define CAMBOX_MAX 256
static struct { uint32_t room; float x0, z0, x1, z1, y0, y1; } s_cambox[CAMBOX_MAX];
static int s_cambox_n = 0;
static void cambox_load(void) {
    static unsigned long long last = 0;
    extern uint64_t s_frame_count;
    if (s_frame_count - last < 120 && last) return;
    last = s_frame_count;
    FILE *f = NULL;
    if (!f) { s_cambox_n = 0; return; }
    int n = 0; char line[256];
    while (n < CAMBOX_MAX && fgets(line, sizeof line, f)) {
        char *p = line; while (*p == ' ' || *p == 9) p++;
        if (*p == '#' || *p == 10 || *p == 13 || !*p) continue;
        unsigned room = 0; float a, b, c, d, y0 = -1e9f, y1 = 1e9f;
        int got = sscanf(p, "%u %f %f %f %f %f %f", &room, &a, &b, &c, &d, &y0, &y1);
        if (got < 5) continue;
        if (got < 7) { y0 = -1e9f; y1 = 1e9f; }
        s_cambox[n].room = room;
        s_cambox[n].x0 = a < c ? a : c;  s_cambox[n].x1 = a < c ? c : a;
        s_cambox[n].z0 = b < d ? b : d;  s_cambox[n].z1 = b < d ? d : b;
        s_cambox[n].y0 = y0 < y1 ? y0 : y1;  s_cambox[n].y1 = y0 < y1 ? y1 : y0;
        n++;
    }
    fclose(f);
    { static int shown = -1;
      if (n != shown) { shown = n;
          fprintf(stderr, "camera_boxes.txt: %d box(es) loaded%c", n, 10); } }
    s_cambox_n = n;
}

static float cambox_hit(uint32_t room, const float o[3], const float e[3]) {
    float best = 1.f;
    float dx = e[0]-o[0], dy = e[1]-o[1], dz = e[2]-o[2];
    for (int i = 0; i < s_cambox_n; i++) {
        if (s_cambox[i].room != room) continue;
        float t0 = 0.f, t1 = 1.f;
        for (int ax = 0; ax < 2; ax++) {
            float oo = ax ? o[2] : o[0], dd = ax ? dz : dx;
            float lo = ax ? s_cambox[i].z0 : s_cambox[i].x0;
            float hi = ax ? s_cambox[i].z1 : s_cambox[i].x1;
            if (dd > -1e-6f && dd < 1e-6f) { if (oo < lo || oo > hi) { t0 = 1.f; t1 = 0.f; break; } continue; }
            float ta = (lo-oo)/dd, tb = (hi-oo)/dd;
            if (ta > tb) { float s = ta; ta = tb; tb = s; }
            if (ta > t0) t0 = ta;
            if (tb < t1) t1 = tb;
        }
        if (t1 < t0 || t0 <= 0.f || t0 >= best) continue;
        float hy = o[1] + dy*t0;
        if (hy < s_cambox[i].y0 || hy > s_cambox[i].y1) continue;
        best = t0;
    }
    return best;
}

static int s_camwhy_map = 0, s_camwhy_box = 0, s_camwhy_mesh = 0, s_camwhy_hand = 0;
static float s_camdbg_tw[3], s_camdbg_ew[3];
static float s_nots_oyaw = 0.f, s_nots_opitch = 0.35f;
static int   s_nots_oinit = 0;

static int   s_rec_arm = 0;

static int   s_takeover_pend = 0, s_takeover_left = 0;
static unsigned long long s_takeover_f = 0, s_takeover_done_f = 0;

static unsigned long long s_snap_arm_f;
static void dc_cam_snap_hold_tick(unsigned long long gfq);
static int dc_ours_shown_now(void);

static int s_shotsw_left = 0;
static unsigned long long s_shotsw_f = 0;

static float s_cam_eff_t;
static float s_cam_probe_pub;
static int s_takeover_steady = 0;
int dc_cam_takeover_pending(void) {
    static int en = -1;
    if (en < 0) { const char *e = getenv("DC_CAM_TAKEOVER_HOLD"); en = (e && *e == '0') ? 0 : 1; }
    extern uint64_t s_frame_count;
    const unsigned long long now = (unsigned long long)s_frame_count;

    static float eff_seen = -1.f;
    const float eff_prev = eff_seen;
    eff_seen = s_cam_eff_t;
    if (s_shotsw_left > 0) {
        if (now - s_shotsw_f > 8ull) s_shotsw_left = 0;
        else {
            s_shotsw_left--;
            static long n = 0;
            if (++n <= 40) fprintf(stderr, "shotswitch: f=%llu the first frame of the scene's shot after our camera - mixed, not shown%c", now, 10);
            return 1;
        }
    }
    if (!en) return 0;
    if (s_snap_arm_f) {
        dc_cam_snap_hold_tick(now);
        if (s_snap_arm_f && !dc_ours_shown_now()) {
            static long n = 0;
            if (++n <= 12) fprintf(stderr, "snaphold: f=%llu the game's camera draws the frame - hold released%c", now, 10);
            s_snap_arm_f = 0; s_takeover_pend = 0; s_takeover_left = 0; s_takeover_steady = 0;
            return 0;
        }
    }
    if (s_takeover_left > 0) {
        if (now - s_takeover_done_f > 8ull) s_takeover_left = 0;
        else {
            s_takeover_left--;
            if (!s_takeover_left) { static int st = -1;
                if (st < 0) { const char *e = getenv("DC_CAM_TAKEOVER_STEADY"); st = e ? atoi(e) : 3; if (st < 0) st = 0; if (st > 8) st = 8; }
                s_takeover_steady = st; }
            return 1;
        }
    }
    if (s_takeover_steady > 0) {
        if (now - s_takeover_done_f > 14ull) s_takeover_steady = 0;
        else if (eff_prev >= 0.f && fabsf(s_cam_eff_t - eff_prev) > 0.05f) {
            s_takeover_steady--;
            static long n = 0;
            if (++n <= 40) fprintf(stderr, "takeover: f=%llu the boom is still settling (%.2f -> %.2f, probe %.2f) - frame held%c",
                                   now, eff_prev, s_cam_eff_t, s_cam_probe_pub, 10);
            return 1;
        } else s_takeover_steady = 0;
    }
    if (!s_takeover_pend) return 0;

    { static int wd = -1; if (wd < 0) { const char *e = getenv("DC_CAM_TAKEOVER_WATCHDOG"); wd = e ? atoi(e) : 32; if (wd < 1) wd = 1; }
      if (now - s_takeover_f > (unsigned long long)wd) { s_takeover_pend = 0; return 0; } }
    return 1;
}
static void dc_cam_takeover_done(void) {
    static int settle = -1;
    if (settle < 0) { const char *e = getenv("DC_CAM_TAKEOVER_SETTLE"); settle = e ? atoi(e) : 1; if (settle < 0) settle = 0; }
    extern uint64_t s_frame_count;
    if (s_takeover_pend) { s_takeover_left = settle; s_takeover_done_f = (unsigned long long)s_frame_count; }
    s_takeover_pend = 0;
}

static unsigned long long s_snap_arm_f = 0;
static float s_snap_eye0[3];
static void dc_cam_snap_hold_arm(unsigned long long gfq) {
    static int en = -1;
    if (en < 0) { const char *e = getenv("DC_CAM_SNAP_HOLD"); en = (e && *e == '0') ? 0 : 1; }
    if (!en) return;
    s_snap_arm_f = gfq;
    for (int i = 0; i < 3; i++) s_snap_eye0[i] = g_dc_cam_eye[i];
    s_takeover_pend = 1; s_takeover_f = gfq; s_takeover_left = 0;
}
static void dc_cam_snap_hold_tick(unsigned long long gfq) {
    if (!s_snap_arm_f || gfq <= s_snap_arm_f) return;
    const float dx = g_dc_cam_eye[0] - s_snap_eye0[0], dy = g_dc_cam_eye[1] - s_snap_eye0[1], dz = g_dc_cam_eye[2] - s_snap_eye0[2];
    const float d = sqrtf(dx * dx + dy * dy + dz * dz);
    if (d > 300.f || gfq - s_snap_arm_f > 8ull) {
        static long n = 0;
        if (++n <= 12) fprintf(stderr, "snaphold: f=%llu the camera eye %s (moved %.0f over %llu vblanks) - present released in a frame%c",
                               gfq, d > 300.f ? "jumped to it" : "did not move (guard)", d, gfq - s_snap_arm_f, 10);
        s_snap_arm_f = 0;
        dc_cam_takeover_done();
    }
}
static float s_nd_dist = 1700.f, s_nd_shoulder = 280.f, s_nd_tgtup = 1560.f;

static int nots_true_shoulder(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("DC_NOTS_TRUE_SHOULDER"); v = (e && *e == '0') ? 0 : 1; }
    return v;
}

int dc_ws_world(void);
float dc_ws_sq(void);
float dc_ws_frame_k(void);

float g_dc_cam_side_pct = 100.f;
float g_dc_cam_up = 0.f;

float g_dc_cam_aim_pct = 100.f;
float g_dc_cam_run_pct = 100.f;
static float s_cam_aim_floor = 1e9f;
static float s_cam_up_live = 0.f;
static float cam_side_k(void) { return g_dc_cam_side_pct * 0.01f; }
static float nots_shoulder_eff_d(float dist) {
    static int init = 0, law = 1;

    static float fnear = 0.32f, dnear = 600.f, dfar = 6500.f, pw = 4.5f, shmax = 0.25f;
    if (!nots_true_shoulder()) return s_nd_shoulder * cam_side_k();
    if (!init) { const char *e;
        if ((e = getenv("DC_NOTS_SHOULDER_LAW"))) law = (e[0] == '0') ? 0 : 1;
        if ((e = getenv("DC_NOTS_SH_NEAR")))  fnear = (float)atof(e);
        if ((e = getenv("DC_NOTS_SH_DNEAR"))) dnear = (float)atof(e);
        if ((e = getenv("DC_NOTS_SH_DFAR")))  dfar  = (float)atof(e);
        if ((e = getenv("DC_NOTS_SH_POW")))   pw    = (float)atof(e);
        if ((e = getenv("DC_NOTS_SHOULDER_MAX"))) shmax = (float)atof(e);
        if (dfar < dnear + 1.f) dfar = dnear + 1.f;
        init = 1; }
    if (dist < 1.f) dist = 1.f;
    if (!law) {
        float lim = dist * shmax, sh = s_nd_shoulder;
        if (sh >  lim) sh =  lim;
        if (sh < -lim) sh = -lim;
        return sh * cam_side_k();
    }
    { float t = (dfar - dist) / (dfar - dnear);
      float f, scale = s_nd_shoulder / 300.f * cam_side_k();
      if (t < 0.f) t = 0.f; if (t > 1.f) t = 1.f;
      f = fnear * powf(t, pw) * scale;
      f /= dc_ws_frame_k();
      return f * dist; }
}
static float nots_shoulder_eff(void) { return nots_shoulder_eff_d(s_nd_dist); }

static float s_cam_eff_t = 1.f;

static float s_cam_probe_pub = 1.f;

static int   s_cam_cut_snap = 0;

static int   s_cam_postcut = 0;

static float s_cam_aim_k = 0.f;

static float s_cam_near_clear = 0.f;

static float s_cam_wsk_len_pub = -1.f;
float dc_cam_whisker_len(void) { return s_cam_wsk_len_pub; }

static int dc_cam_one_curve(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("DC_CAM_ONE_CURVE"); v = (e && *e == '0') ? 0 : 1; }
    return v;
}

static int dc_cam_out_one(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("DC_CAM_OUT_ONE"); v = (e && *e == '0') ? 0 : 1; }
    return v;
}
enum { DCC_LINEAR, DCC_SINE, DCC_POWER, DCC_EXPO, DCC_CIRC, DCC_BACK, DCC_ELASTIC, DCC_BOUNCE, DCC_SMOOTH, DCC_HUMP, DCC_N };
enum { DCD_IN, DCD_OUT, DCD_INOUT, DCD_OUTIN, DCD_N };
static const char *const s_dcc_name[DCC_N] = { "linear", "sine", "power", "expo", "circ", "back", "elastic", "bounce", "smooth", "hump" };
static const char *const s_dcd_name[DCD_N] = { "in", "out", "inout", "outin" };
typedef struct { const char *key; float time, speed, delay, power; int curve, dir; } DcCamTr;
enum { TR_RUN, TR_AIM, TR_AIM_RUN, TR_BACK_RUN, TR_BACK_AIM, TR_WALL_IN, TR_WALL_OUT, TR_GRAB_IN, TR_GRAB_MOVE, TR_GRAB_OUT,
       TR_TURN180, TR_SCENE_BACK, TR_SCENE_TREX, TR_N };

static DcCamTr s_camtr[TR_N] = {
    { "run",        0.70f, 0.f,    0.f,  1.f, DCC_SMOOTH, DCD_INOUT },
    { "aim",        1.00f, 0.f,    0.f,  2.f, DCC_POWER,  DCD_OUT   },
    { "aim_run",    1.00f, 0.f,    0.f,  2.f, DCC_POWER,  DCD_OUT   },
    { "back_run",   0.70f, 0.f,    0.f,  1.f, DCC_SMOOTH, DCD_INOUT },
    { "back_aim",   0.70f, 0.f,    0.f,  1.f, DCC_SMOOTH, DCD_INOUT },
    { "wall_in",    0.00f, 0.f,    0.f,  1.f, DCC_LINEAR, DCD_OUT   },
    { "wall_out",   2.00f, 0.f,    0.25f, 2.f, DCC_SMOOTH, DCD_INOUT },
    { "grab_in",    0.55f, 514.3f, 0.f,  5.f, DCC_HUMP,   DCD_OUT   },
    { "grab_move",  0.60f, 514.3f, 0.f,  5.f, DCC_HUMP,   DCD_OUT   },
    { "grab_out",   0.60f, 514.3f, 0.f,  5.f, DCC_HUMP,   DCD_OUT   },
    { "turn180",    0.00f, 0.f,    0.f,  2.f, DCC_SINE,   DCD_INOUT },
    { "scene_back", 0.00f, 0.f,    0.f,  2.f, DCC_SINE,   DCD_INOUT },
    { "scene_trex", 0.00f, 0.f,    0.f,  2.f, DCC_SINE,   DCD_INOUT },
};
static float dcc_bounce_out(float x) {
    const float n1 = 7.5625f, d1 = 2.75f;
    if (x < 1.f / d1) return n1 * x * x;
    if (x < 2.f / d1) { x -= 1.5f / d1;   return n1 * x * x + 0.75f; }
    if (x < 2.5f / d1) { x -= 2.25f / d1; return n1 * x * x + 0.9375f; }
    x -= 2.625f / d1; return n1 * x * x + 0.984375f;
}
static float dcc_smooth_n(int n, float u) {
    switch (n) {
    case 1:  return u * u * (3.f - 2.f * u);
    case 2:  return u * u * u * (10.f + u * (-15.f + 6.f * u));
    case 3:  return u * u * u * u * (35.f + u * (-84.f + u * (70.f - 20.f * u)));
    default: return u * u * u * u * u * (126.f + u * (-420.f + u * (540.f + u * (-315.f + 70.f * u))));
    }
}

static float dcc_in(int c, float s, float u) {
    const float PI = 3.14159265f;
    switch (c) {
    case DCC_SINE:    return 1.f - cosf(u * PI * 0.5f);
    case DCC_POWER:   return powf(u, s > 0.05f ? s : 0.05f);
    case DCC_EXPO:    { const float k = s > 0.01f ? s : 0.01f; return (powf(2.f, k * u) - 1.f) / (powf(2.f, k) - 1.f); }
    case DCC_CIRC:    { const float q = 1.f - u * u; return 1.f - sqrtf(q > 0.f ? q : 0.f); }
    case DCC_BACK:    return (s + 1.f) * u * u * u - s * u * u;
    case DCC_ELASTIC: { const float n = s > 0.25f ? s : 0.25f;
                        const float g0 = powf(2.f, -10.f) * cosf(2.f * PI * n);
                        const float g  = -powf(2.f, 10.f * (u - 1.f)) * sinf(2.f * PI * n * (u - 1.f) - PI * 0.5f);
                        return (g - g0) / (1.f - g0); }
    case DCC_BOUNCE:  return 1.f - dcc_bounce_out(1.f - u);
    case DCC_SMOOTH:  { int n = (int)(s + 0.5f); if (n < 1) n = 1; if (n > 4) n = 4; return 2.f * dcc_smooth_n(n, u * 0.5f); }
    case DCC_HUMP:    { const float b = s > 1.f ? s : 1.f; return powf(u, b) * (1.f + b - b * u); }
    default:          return u;
    }
}
static float dcc_ease(int c, int d, float s, float u) {
    if (u <= 0.f) return 0.f;
    if (u >= 1.f) return 1.f;
    switch (d) {
    case DCD_IN:    return dcc_in(c, s, u);
    case DCD_OUT:   return 1.f - dcc_in(c, s, 1.f - u);
    case DCD_INOUT: return (u < 0.5f) ? 0.5f * dcc_in(c, s, 2.f * u) : 1.f - 0.5f * dcc_in(c, s, 2.f - 2.f * u);
    default:        return (u < 0.5f) ? 0.5f * (1.f - dcc_in(c, s, 1.f - 2.f * u)) : 0.5f + 0.5f * dcc_in(c, s, 2.f * u - 1.f);
    }
}
#ifdef DCME_RELEASE
#include "dc_pak.h"
#pragma push_macro("GetFileAttributesExA")
#undef GetFileAttributesExA
#pragma push_macro("FILE")
#undef FILE
#pragma push_macro("fopen")
#undef fopen
#pragma push_macro("fgets")
#undef fgets
#pragma push_macro("fread")
#undef fread
#pragma push_macro("fclose")
#undef fclose
#pragma push_macro("_stat64")
#undef _stat64
#pragma push_macro("_findfirst")
#undef _findfirst
#pragma push_macro("_findnext")
#undef _findnext
#pragma push_macro("_findclose")
#undef _findclose
#define GetFileAttributesExA(p, lvl, fa) dcme_file_attr((p), (fa))
#define FILE DcMemFile
#define fopen(p, m) dc_mf_open(p)
#define fgets(b, n, f) dc_mf_gets((b), (n), (f))
#define fread(d, s, c, f) dc_mf_read((d), (s), (c), (f))
#define fclose(f) dc_mf_close(f)
#define _stat64(p, st) dcme_stat64((p), (st))
#define _findfirst(p, fd) dcme_findfirst((p), (fd))
#define _findnext(h, fd) dcme_findnext((h), (fd))
#define _findclose(h) dcme_findclose(h)
#endif
static void dc_camtr_load(void) {
    static int done = 0;
    if (done) return;
    done = 1;
    const char *path = getenv("DC_CAM_CURVES_FILE"); if (!path || !*path) path = "dc_cam_curves.cfg";
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "camcurves: %s not found - the game's own transition curves%c", path, 10); return; }
    char line[256]; int n = 0, bad = 0;
    while (fgets(line, sizeof line, f)) {
        char *p = line; while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == ';' || *p == '\r' || *p == '\n' || !*p) continue;
        char *eq = strchr(p, '='), *dot = strchr(p, '.');
        if (!eq || !dot || dot > eq) { bad++; continue; }
        *eq = 0; *dot = 0;
        char *key = p, *fld = dot + 1, *val = eq + 1;
        for (char *q = fld + strlen(fld); q > fld && (q[-1] == ' ' || q[-1] == '\t'); q--) q[-1] = 0;
        while (*val == ' ' || *val == '\t') val++;
        for (char *q = val + strlen(val); q > val && (q[-1] == '\r' || q[-1] == '\n' || q[-1] == ' ' || q[-1] == '\t'); q--) q[-1] = 0;
        int ti = -1; for (int i = 0; i < TR_N; i++) if (!strcmp(s_camtr[i].key, key)) ti = i;
        if (ti < 0) { bad++; continue; }
        DcCamTr *t = &s_camtr[ti];
        if      (!strcmp(fld, "time"))  t->time  = (float)atof(val);
        else if (!strcmp(fld, "speed")) t->speed = (float)atof(val);
        else if (!strcmp(fld, "delay")) t->delay = (float)atof(val);
        else if (!strcmp(fld, "power")) t->power = (float)atof(val);
        else if (!strcmp(fld, "curve")) { int c = -1; for (int i = 0; i < DCC_N; i++) if (!strcmp(s_dcc_name[i], val)) c = i;
                                          if (c < 0) { bad++; continue; } t->curve = c; }
        else if (!strcmp(fld, "dir"))   { int d = -1; for (int i = 0; i < DCD_N; i++) if (!strcmp(s_dcd_name[i], val)) d = i;
                                          if (d < 0) { bad++; continue; } t->dir = d; }
        else { bad++; continue; }
        n++;
    }
    fclose(f);
    fprintf(stderr, "camcurves: %d values from %s (%d lines not understood)%c", n, path, bad, 10);
    for (int i = 0; i < TR_N; i++)
        fprintf(stderr, "camcurves:   %-10s time %.2f s, speed %.0f, delay %.2f s, %s %s %.2f%c", s_camtr[i].key, s_camtr[i].time,
                s_camtr[i].speed, s_camtr[i].delay, s_dcc_name[s_camtr[i].curve], s_dcd_name[s_camtr[i].dir], s_camtr[i].power, 10);
}
#ifdef DCME_RELEASE
#undef GetFileAttributesExA
#pragma pop_macro("GetFileAttributesExA")
#undef FILE
#pragma pop_macro("FILE")
#undef fopen
#pragma pop_macro("fopen")
#undef fgets
#pragma pop_macro("fgets")
#undef fread
#pragma pop_macro("fread")
#undef fclose
#pragma pop_macro("fclose")
#undef _stat64
#pragma pop_macro("_stat64")
#undef _findfirst
#pragma pop_macro("_findfirst")
#undef _findnext
#pragma pop_macro("_findnext")
#undef _findclose
#pragma pop_macro("_findclose")
#endif
static float dc_camtr_dur(int tr, float dist) {
    dc_camtr_load();
    const DcCamTr *t = &s_camtr[tr];
    float d = t->time + ((t->speed > 0.f) ? fabsf(dist) / t->speed : 0.f);
    return d > 0.f ? d : 0.f;
}
static float dc_camtr_ease(int tr, float u) {
    dc_camtr_load();
    const DcCamTr *t = &s_camtr[tr];
    return dcc_ease(t->curve, t->dir, t->power, u);
}

static float s_cam_aim_dist_pub = 650.f;
static float s_nots_pitch_eff = 0.35f;
extern float g_dc_cam_tgt[3];
static float nots_shoulder_eff_d(float dist);
void dc_nots_aim_eye(float out[3]) {
    float cy = cosf(s_nots_oyaw), sy = sinf(s_nots_oyaw);
    float cp = cosf(s_nots_pitch_eff), sp = sinf(s_nots_pitch_eff);
    float dir[3] = { sy*cp, sp, cy*cp };
    float rtl[3] = { cy, 0.f, -sy };
    float sh = nots_shoulder_eff_d(s_cam_aim_dist_pub);
    out[0] = g_dc_cam_tgt[0] - dir[0]*s_cam_aim_dist_pub + rtl[0]*sh;
    out[1] = g_dc_cam_tgt[1] - s_cam_up_live - dir[1]*s_cam_aim_dist_pub;
    out[2] = g_dc_cam_tgt[2] - dir[2]*s_cam_aim_dist_pub + rtl[2]*sh;
}

void dc_nots_cam_get(float *yaw, float *pitch, float *dist) {
    *yaw = s_nots_oyaw; *pitch = s_nots_opitch; *dist = s_nd_dist;
}
void dc_nots_cam_set(float yaw, float pitch, float dist) {
    s_nots_oyaw = yaw; s_nots_opitch = pitch;
    if (dist > 0.f) {
        s_nd_dist = dist;

        if (s_nd_dist < 100.f)  s_nd_dist = 100.f;

        if (s_nd_dist > 40000.f) s_nd_dist = 40000.f;
    }
}

float dc_nots_dist_opt_get(void) { return s_nd_dist < 1.5f ? 0.f : s_nd_dist; }
void  dc_nots_dist_opt_set(float v) { s_nd_dist = (v < 1.f) ? 1.f : (v > 40000.f ? 40000.f : v); }

static float s_cam_vlp(void) {
    static float v = -1.f;
    if (v < 0.f) { const char *e = getenv("DC_CAM_VLP"); v = e ? (float)atof(e) : 0.45f;
                   if (v < 0.f) v = 0.f; if (v > 1.f) v = 1.f; }
    return v;
}
static float s_cam_spring(void) {
    static float v = -1.f;
    if (v < 0.f) { const char *e = getenv("DC_CAM_SPRING"); v = e ? (float)atof(e) : 0.25f;
                   if (v < 0.f) v = 0.f; if (v > 1.f) v = 1.f; }
    return v;
}

static float s_cam_lp(void) {
    static float v = -1.f;

    if (v < 0.f) { const char *e = getenv("DC_CAM_LP"); v = e ? (float)atof(e) : 0.f;
                   if (v < 0.f) v = 0.f; if (v > 1.f) v = 1.f; }
    return v;
}

static float s_cam_lead(void) {
    static float v = -1.f;
    if (v < 0.f) { const char *e = getenv("DC_CAM_LEAD"); v = e ? (float)atof(e) : 2.f;
                   if (v < 0.f) v = 0.f; if (v > 2.f) v = 2.f; }
    return v;
}
static int s_cam_lock(void) {
    static int v = -1;

    if (v < 0) { const char *e = getenv("DC_CAM_LOCK"); v = (e && *e == '0') ? 0 : 1; }
    return v;
}

static uint64_t s_nots_look_vb = 0;

static uint64_t s_tank_rigid_vb = 0;
int dc_tank_warp_rigid(void) {
    static int en = -1; if (en < 0) { const char *e = getenv("DC_TANK_WARP_RIGID"); en = (e && *e == '0') ? 0 : 1; }
    extern uint64_t s_frame_count;
    return en && s_tank_rigid_vb && s_frame_count - s_tank_rigid_vb <= 3;
}
void dc_nots_look(float dyaw, float dpitch) {

    { extern int dc_game_paused(void); if (dc_game_paused()) return; }
    if (fabsf(dyaw) > 0.004f || fabsf(dpitch) > 0.004f) { extern uint64_t s_frame_count; s_nots_look_vb = s_frame_count; }
    s_nots_oyaw += dyaw;
    while (s_nots_oyaw >  3.14159265f) s_nots_oyaw -= 6.2831853f;
    while (s_nots_oyaw < -3.14159265f) s_nots_oyaw += 6.2831853f;
    s_nots_opitch += dpitch;

    { static float up = -2.f, dn = -2.f;
      if (up < -1.f) { const char *e = getenv("DC_CAM_PITCH_UP");
                       up = e ? (float)atof(e) : 1.26f;
                       e = getenv("DC_CAM_PITCH_DOWN"); dn = e ? (float)atof(e) : 1.31f; }
      if (s_nots_opitch < -up) s_nots_opitch = -up;
      if (s_nots_opitch >  dn) s_nots_opitch =  dn; }
}

static void nots_cutlock_rebase(const float Rn[9]) {
    static int en = -1;
    if (en < 0) { const char *e = getenv("DC_NOTS_CUTLOCK"); en = (e && *e == '1') ? 1 : 0; }
    if (!en || !s_nots_domhave) return;
    const float *Ro = s_nots_domR;
    float B[9];
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++)
            B[r*3+c] = Rn[r*3+0]*Ro[c*3+0] + Rn[r*3+1]*Ro[c*3+1] + Rn[r*3+2]*Ro[c*3+2];
    if (fabsf(B[4] - 1.0f) > 0.06f || fabsf(B[1]) > 0.12f || fabsf(B[3]) > 0.12f ||
        fabsf(B[5]) > 0.12f || fabsf(B[7]) > 0.12f) return;
    float beta = atan2f(-B[2], B[0]);
    static float sgn = 0.f;
    if (sgn == 0.f) { const char *e = getenv("DC_NOTS_CUTSIGN"); sgn = (e && *e == '-') ? -1.f : 1.f; }
    s_nots_oyaw -= sgn * beta;
    while (s_nots_oyaw >  3.14159265f) s_nots_oyaw -= 6.2831853f;
    while (s_nots_oyaw < -3.14159265f) s_nots_oyaw += 6.2831853f;
    { static int dbg = -1;
      if (dbg < 0) { const char *e = getenv("DC_CAMDBG"); dbg = (e && *e == '1') ? 1 : 0; }
      if (dbg) fprintf(stderr, "cutlock: zone turned %.1f deg, orbit re-aimed to %.1f%c",
                       beta * 57.2957795f, s_nots_oyaw * 57.2957795f, 10); }
}

void dc_nots_zoom(float dd) {

    static float zmin = -1.f, zmax = -1.f;
    if (zmin < 0.f) { const char *e = getenv("DC_NOTS_ZOOM_MIN"); zmin = e ? (float)atof(e) : 1200.f;
                      e = getenv("DC_NOTS_ZOOM_MAX"); zmax = e ? (float)atof(e) : 6500.f;
                      if (zmin < 100.f) zmin = 100.f;
                      if (zmax < zmin)  zmax = zmin; }
    s_nd_dist += dd;
    if (s_nd_dist < zmin) s_nd_dist = zmin;
    if (s_nd_dist > zmax) s_nd_dist = zmax;
}

void dc_nots_shoulder_adjust(float d){
    int ws;
    float eff;
    s_nd_shoulder += d;
    eff = nots_shoulder_eff();
    ws = dc_ws_world();
    fprintf(stderr, "nOTS shoulder=%.0f eff=%.0f  (%.1f%% of frame width%s)\n",
            s_nd_shoulder, eff,
            100.f * (eff / s_nd_dist) * dc_ws_sq(),
            ws ? ", wide" : "");
}
void dc_nots_tgtup_adjust(float d){ s_nd_tgtup += d; fprintf(stderr,"nOTS tgtup=%.0f\n",s_nd_tgtup); }

int g_opt_cam_height = 0;

static float s_nd_hip_drop = 400.f;

#define HIPROOMS_MAX 128
static int s_hiprooms[HIPROOMS_MAX], s_hiprooms_n = 0, s_hiprooms_loaded = 0;
static void hiprooms_load(void) {
    if (s_hiprooms_loaded) return;
    s_hiprooms_loaded = 1;
    DcMemFile *f = dc_mf_open("dc_hiprooms_user.cfg");
    if (!f) return;
    char line[64];
    while (dc_mf_gets(line, sizeof line, f) && s_hiprooms_n < HIPROOMS_MAX) {
        int r; if (line[0] != '#' && sscanf(line, "%d", &r) == 1) s_hiprooms[s_hiprooms_n++] = r;
    }
    dc_mf_close(f);
    fprintf(stderr, "nOTS cam height: hips in %d room(s) (dc_hiprooms_user.cfg)\n", s_hiprooms_n);
}
static void hiprooms_save(void) {
    FILE *f = NULL;
    if (!f) return;
    fprintf(f, "# rooms where the hips camera is on (F8 -> Camera height -> hips in THIS room)\n");
    for (int i = 0; i < s_hiprooms_n; i++) fprintf(f, "%d\n", s_hiprooms[i]);
    fclose(f);
}
/* Load camera curves and hip offsets before play. */
void dc_memory_preload(void) { dc_camtr_load(); hiprooms_load(); }
int  dc_cam_hiproom_get(int room) { hiprooms_load(); for (int i = 0; i < s_hiprooms_n; i++) if (s_hiprooms[i] == room) return 1; return 0; }
void dc_cam_hiproom_set(int room, int on) {
    hiprooms_load();
    int i = -1; for (int k = 0; k < s_hiprooms_n; k++) if (s_hiprooms[k] == room) i = k;
    if (on && i < 0 && s_hiprooms_n < HIPROOMS_MAX) s_hiprooms[s_hiprooms_n++] = room;
    if (!on && i >= 0) { for (int k = i; k + 1 < s_hiprooms_n; k++) s_hiprooms[k] = s_hiprooms[k + 1]; s_hiprooms_n--; }
    hiprooms_save();
    fprintf(stderr, "nOTS cam height: room %d hips %s (%d rooms)\n", room, on ? "ON" : "off", s_hiprooms_n);
}
int  dc_cam_hiproom_list(int *out, int max) { hiprooms_load(); int n = s_hiprooms_n < max ? s_hiprooms_n : max; for (int i = 0; i < n; i++) out[i] = s_hiprooms[i]; return n; }
static int nots_hips_now(void) {
    extern int dc_room_effective(void);
    if (g_opt_cam_height == 1) return 1;
    int room = dc_room_effective();
    int hips = dc_cam_hiproom_get(room);
    static int last_room = -2, last_hips = -1;
    if (room != last_room || hips != last_hips) {
        fprintf(stderr, "nOTS cam height: room %d -> %s%c", room, hips ? "hips" : "shoulders", 10);
        last_room = room; last_hips = hips;
    }
    return hips;
}
static float nots_tgtup_eff(void) { return nots_hips_now() ? (s_nd_tgtup - s_nd_hip_drop) : s_nd_tgtup; }
float dc_nots_cam_height_get(void) { return (float)g_opt_cam_height; }
void  dc_nots_cam_height_set(float v) { int m = (v >= 0.5f) ? 1 : 0;
                                        if (m != g_opt_cam_height) fprintf(stderr, "nOTS cam height: %s (tgtup %.0f)\n", m ? "hips" : "shoulders", m ? s_nd_tgtup - s_nd_hip_drop : s_nd_tgtup);
                                        g_opt_cam_height = m; }
float dc_nots_hip_drop_get(void) { return s_nd_hip_drop; }
void  dc_nots_hip_drop_set(float v) { if (v < 0.f) v = 0.f; if (v > 1000.f) v = 1000.f; s_nd_hip_drop = v; }

static float s_nots_D[9]; static float s_nots_T[3]; static int s_nots_dhave = 0;

static float s_dNp[9], s_deyev[3]; static int s_dNhave = 0;

static float s_dtgt[3];
static float s_wD_N[9], s_wD_eye[3], s_wD_tgt[3];  static int s_wD_have = 0;  static unsigned long long s_wD_flip = 0;
static float s_wDp_N[9], s_wDp_eye[3], s_wDp_tgt[3]; static int s_wDp_have = 0; static unsigned long long s_wDp_flip = 0;

static float s_wD_cam = 0.f, s_wDp_cam = 0.f; static int s_wD_her = 0, s_wDp_her = 0;
static float s_nots_Dp[9]; static float s_nots_Tp[3]; static int s_nots_dphave = 0;

static int s_nots_D_pass = 0, s_nots_Dp_pass = 0;
int dc_nots_delta_pass(int prev) { return prev ? s_nots_Dp_pass : s_nots_D_pass; }

static float s_nots_bbmin[3], s_nots_bbmax[3]; static int s_nots_bbhave = 0;
void dc_nots_set_aabb(const float mn[3], const float mx[3]) {

    if (!s_nots_bbhave) {
        for (int i=0;i<3;i++){ s_nots_bbmin[i]=mn[i]; s_nots_bbmax[i]=mx[i]; }
        s_nots_bbhave = 1;
        return;
    }
    for (int i=0;i<3;i++){
        s_nots_bbmin[i] += 0.25f*(mn[i]-s_nots_bbmin[i]);
        s_nots_bbmax[i] += 0.25f*(mx[i]-s_nots_bbmax[i]);
    }
}

static int nots_cutdbg(void) {
    static int d = -1;
    if (d < 0) { const char* e = getenv("DC_NOTS_CUTDBG"); d = (e && *e=='1') ? 1 : 0; }
    return d;
}
static int      s_nots_cut_phase = 0;
static uint64_t s_nots_cut_up15  = 0;
static uint64_t s_nots_cut_rel   = 0;
long dc_nots_cut_votes = 0;
void dc_nots_flip_notify(void) {
    if (s_nots_cut_phase == 3 && dc_nots_cut_votes >= 300) {
        s_nots_cut_phase = 0;
        if (nots_cutdbg()) fprintf(stderr, "cuthold: RELEASE at voted flip (votes=%ld)\n",
                                   dc_nots_cut_votes);
    }
}

int g_hold_reason = 0;
long g_hold_n[4] = {0,0,0,0};
int dc_nots_present_hold(void) {

    {
        static int ps = -1;
        if (ps < 0) {
            const char* e = getenv("DC_NOTS_PAIRSKIP");
            ps = (e && *e=='0') ? 0 : 1;
        }
        extern uint64_t s_frame_count;

        static uint64_t flags_up_f = 0;
        static long     vote_snap = 0;
        long votes_now = dc_nots_cut_votes;
        long vdelta = votes_now - vote_snap;
        vote_snap = votes_now;
        if (ram[0xB92C8] != 4) {
            uint32_t fl2 = (uint32_t)ram[0xB788C] | ((uint32_t)ram[0xB788D]<<8) |
                           ((uint32_t)ram[0xB788E]<<16) | ((uint32_t)ram[0xB788F]<<24);
            if (fl2 & 0x00300000u) flags_up_f = s_frame_count;
            extern int dcvk_stream_count(void); extern int dcvk_warp_pending(void);
            int warp_b = (dcvk_stream_count() == 0 && dcvk_warp_pending());
            if (nots_cutdbg() && flags_up_f && s_frame_count - flags_up_f < 12)
                fprintf(stderr, "cuthold: f=%llu window votes=%ld stream=%d warp_b=%d%c",
                        (unsigned long long)s_frame_count, vdelta, dcvk_stream_count(), warp_b, 10);
            if (ps && s_nots_dhave && flags_up_f &&
                s_frame_count - flags_up_f < 12 && vdelta < 200 && !warp_b) {
                s_nots_pairskip = 0;
                if (nots_cutdbg())
                    fprintf(stderr, "cuthold: f=%llu REDRAWSKIP (votes=%ld)\n",
                            (unsigned long long)s_frame_count, vdelta);
                g_hold_reason = 1; g_hold_n[1]++;
                return 1;
            }
        }

        if (ps && dc_scripted_edge() && !dc_scripted_now()) s_nots_pairskip = 1;
        if (ps && s_nots_pairskip && s_nots_dhave && !dc_scripted_now()) {
            s_nots_pairskip = 0;
            if (nots_cutdbg())
                fprintf(stderr, "cuthold: f=%llu PAIRSKIP (one frame hidden)\n",
                        (unsigned long long)s_frame_count);
            g_hold_reason = 2; g_hold_n[2]++;
            return 1;
        }
        s_nots_pairskip = 0;
    }
    static int en = -1;
    if (en < 0) {
        const char* e = getenv("DC_NATIVE_OTS");
        int nen = (e && *e=='1') ? 1 : 0;
        e = getenv("DC_NOTS_CUTHOLD");

        en = (nen && e && *e=='1') ? 1 : 0;
    }
    if (!en) return 0;
    extern uint64_t s_frame_count;
    if (ram[0xB92C8] == 4) {
        s_nots_cut_phase = 0;
        return 0;
    }
    uint32_t fl = (uint32_t)ram[0xB788C] | ((uint32_t)ram[0xB788D]<<8) |
                  ((uint32_t)ram[0xB788E]<<16) | ((uint32_t)ram[0xB788F]<<24);
    if (fl & 0x00300000u) {
        if (s_nots_cut_phase == 0) {
            s_nots_cut_phase = 1;
            s_nots_cut_up15  = s_frame_count + 15;
            if (nots_cutdbg()) fprintf(stderr, "cuthold: f=%llu flags=%06X UP\n",
                (unsigned long long)s_frame_count, fl);
        }
        if (s_nots_cut_phase == 1 && s_frame_count >= s_nots_cut_up15) {
            s_nots_cut_phase = 4;
            if (nots_cutdbg()) fprintf(stderr, "cuthold: f=%llu GAVE UP (flags too long)\n",
                (unsigned long long)s_frame_count);
        }
    } else {
        if (s_nots_cut_phase == 1) {
            s_nots_cut_phase = 3;
            s_nots_cut_rel   = s_frame_count + 12;
            dc_nots_cut_votes = 0;
            if (nots_cutdbg()) fprintf(stderr, "cuthold: f=%llu flags DOWN -> await voted flip (cap=%llu)\n",
                (unsigned long long)s_frame_count, (unsigned long long)s_nots_cut_rel);
        } else if (s_nots_cut_phase == 4) {
            s_nots_cut_phase = 0;
        }
    }
    if (s_nots_cut_phase == 3 && s_frame_count >= s_nots_cut_rel) {
        s_nots_cut_phase = 0;
        if (nots_cutdbg()) fprintf(stderr, "cuthold: f=%llu RELEASE (backstop)\n",
                                   (unsigned long long)s_frame_count);
    }
    return (s_nots_cut_phase == 1 || s_nots_cut_phase == 3) && s_nots_dhave;
}

static int s_scr_state = 0, s_scr_edge = 0, s_scr_have = 0;
static void dc_scripted_tick(void) {
    extern uint64_t gpu_flip_seq(void);
    static unsigned long long pin = ~0ull;
    static int latch = -1;
    if (latch < 0) { const char *e = getenv("DC_SCRIPT_LATCH"); latch = (e && *e == '0') ? 0 : 1; }
    int now = (ram[0xB92C8] == 4);
    if (!latch) { s_scr_edge = (s_scr_have && now != s_scr_state); s_scr_state = now; s_scr_have = 1; return; }
    unsigned long long f = (unsigned long long)gpu_flip_seq();
    if (pin == f) return;
    pin = f;
    s_scr_edge = (s_scr_have && now != s_scr_state);
    s_scr_state = now; s_scr_have = 1;
}
int dc_scripted_now(void)  { dc_scripted_tick(); return s_scr_state; }
int dc_scripted_edge(void) { dc_scripted_tick(); return s_scr_edge;  }

int dc_nots_get_view(float Rv[9], float Tv[3]) {
    extern int dc_nots_get_delta(float D[9], float T[3]);
    float D[9], T[3];
    static long fail_dom = 0, fail_delta = 0, okn = 0;
    if (!s_nots_domhave) { if (++fail_dom % 600 == 1) fprintf(stderr, "ownview: no dom (%ld)\n", fail_dom); return 0; }
    if (!dc_nots_get_delta(D, T)) { if (++fail_delta % 600 == 1) fprintf(stderr, "ownview: no delta (%ld)\n", fail_delta); return 0; }
    if (++okn == 1) fprintf(stderr, "ownview: active\n");
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++)
            Rv[r*3+c] = D[r*3+0]*s_nots_domR[0*3+c] +
                        D[r*3+1]*s_nots_domR[1*3+c] +
                        D[r*3+2]*s_nots_domR[2*3+c];
    for (int r = 0; r < 3; r++)
        Tv[r] = D[r*3+0]*s_nots_domT[0] + D[r*3+1]*s_nots_domT[1] +
                D[r*3+2]*s_nots_domT[2] + T[r];
    return 1;
}
extern int g_dc_orig_mode;

static int dc_scene_no_delta(void) {
    static int keep = -1;
    if (keep < 0) { const char *e = getenv("DC_NOTS_SCENE_DELTA"); keep = (e && *e == '1') ? 1 : 0; }
    if (keep) return 0;
    uint8_t *r = memory_get_ram_ptr();
    return (r && r[0xB7878] == 1) ? 1 : 0;
}

int dc_game_cam_zone(void) {
    if (!ram) return -1;
    const int cur = ram[0xB796C], rec = ram[0xB796E];
    if (rec >= 64) return -1;
    const uint32_t a = 0xB79B4u + 4u * (uint32_t)rec;
    const uint32_t p = ((uint32_t)ram[a] | ((uint32_t)ram[a + 1] << 8) | ((uint32_t)ram[a + 2] << 16) | ((uint32_t)ram[a + 3] << 24)) & 0x1FFFFFu;
    if (p < 0x1000u || p + 4u >= 0x200000u) return -1;
    return ram[p + 2] == (uint8_t)cur ? 1 : 0;
}

static unsigned long long s_zc_tf = ~0ull, s_zc_world_since = 0;
static int s_zc_prev_scr = -1, s_zc_scr_reset = 0, s_zc_scene_room = -2;
static int zc_wmin(void) {
    static int wmin = -1;
    if (wmin < 0) { const char *e = getenv("DC_SCENE_ZONECAM_WORLD"); wmin = e ? atoi(e) : 45; if (wmin < 0) wmin = 0; }
    return wmin;
}

static void zc_world_tick(unsigned long long now) {
    if (now == s_zc_tf) return;
    s_zc_tf = now;
    static int menu_keep = -1;
    if (menu_keep < 0) { const char *e = getenv("DC_SCENE_ZONECAM_MENU"); menu_keep = (e && *e == '0') ? 0 : 1; }
    const int scr = ram[0xB7878];
    const int menu = menu_keep && (scr == 5 || scr == 7 || scr == 8);
    const int pmenu = menu_keep && (s_zc_prev_scr == 5 || s_zc_prev_scr == 7 || s_zc_prev_scr == 8);
    if (scr != 3) { if (!menu) s_zc_scr_reset = 1; }
    else if (s_zc_prev_scr != 3 && !pmenu) s_zc_world_since = now;
    s_zc_prev_scr = scr;
}

static int zc_start_ours_now(unsigned long long now) {
    return ram[0xB7878] == 3 && dc_ours_shown_now() && now - s_zc_world_since >= (unsigned long long)zc_wmin();
}
int dc_scene_zonecam_ours(void) {
    static int en = -1;
    if (en < 0) { const char *e = getenv("DC_SCENE_ZONECAM_OURS"); en = (e && *e == '0') ? 0 : 1; }
    if (!en || g_dc_orig_mode || !ram) return 0;
    extern uint64_t s_frame_count;
    extern int dc_fmv_active(void); extern int dc_regina_dead(void); extern int dc_cut_camera(void);
    const unsigned long long now = (unsigned long long)s_frame_count;
    const int in = dc_scripted_now() || dc_cut_camera();
    static unsigned long long out_f = 0;
    static int prev_in = 0, start_ours = 0, shot_seen = 0;
    zc_world_tick(now);
    if (s_zc_scr_reset) { s_zc_scr_reset = 0; start_ours = 0; shot_seen = 0; }

    if (in && !prev_in && (out_f == 0 || now - out_f > 4ull)) {
        extern int dc_room_current(void);
        start_ours = zc_start_ours_now(now);
        s_zc_scene_room = dc_room_current();
        shot_seen = 0;
        fprintf(stderr, "zonecam: f=%llu scene started %s%c", now,
                start_ours ? "mid-game - the zone's usual shot is driven by our camera"
                           : "after a transition/not on our camera - the zone's shot is driven by the game's camera, ours after the scene's first shot", 10);
    }
    if (!in && prev_in) out_f = now;
    prev_in = in;
    if (ram[0xB7878] != 3) return 0;
    if (dc_fmv_active() || dc_regina_dead()) return 0;
    if (!in) return 0;
    const int z = dc_game_cam_zone();
    if (z == 0) shot_seen = 1;
    const int ours = (z == 1 && (start_ours || shot_seen)) ? 1 : 0;
    { static int last = -2, lours = -1;
      if (z != last || ours != lours) {
          if (lours == 1 && !ours && z == 0) {
              static int sh = -1;
              if (sh < 0) { const char *e = getenv("DC_SCENE_SHOT_HOLD"); sh = e ? atoi(e) : 1; if (sh < 0) sh = 0; if (sh > 4) sh = 4; }
              if (sh) { s_shotsw_left = sh; s_shotsw_f = now; }
          }
          last = z; lours = ours;
          fprintf(stderr, "zonecam: f=%llu in the scene the game has %s (shot %d, record %d)%c", now,
                  z == 1 ? (ours ? "the zone's usual shot - our camera" : "the zone's usual shot - the game's camera (the scene started after a transition)")
                         : z == 0 ? "the scene's shot - the game's camera" : "an unclear shot",
                  ram[0xB796C], ram[0xB796E], 10); } }
    return ours;
}

int dc_scene_ourcam_play(void) {
    static int en = -1;
    if (en < 0) { const char *e = getenv("DC_SCENE_OURCAM_PLAY"); en = (e && *e == '0') ? 0 : 1; }
    if (!en || !ram || g_dc_orig_mode || ram[0xB7878] != 3) return 0;
    extern int dc_cut_camera(void); extern int dc_room_current(void); extern uint64_t s_frame_count;
    static int lv = -1;
    const int rc = dc_room_current();
    int v = 0;
    if (dc_scripted_now() || dc_cut_camera()) {

        v = (dc_scene_zonecam_ours() && rc >= 0 && rc == s_zc_scene_room) ? 1 : 0;
    } else if (ram[0xB92C8] == 4) {

        const unsigned long long now = (unsigned long long)s_frame_count;
        zc_world_tick(now);
        v = (zc_start_ours_now(now) && dc_game_cam_zone() == 1 && rc >= 0) ? 1 : 0;
    }
    if (v != lv) {
        if (lv >= 0 || v) {
            extern uint64_t s_frame_count;
            fprintf(stderr, "ourcam scene: f=%llu %s (room %d; DC_SCENE_OURCAM_PLAY=0 - cutscene rules as before)%c",
                    (unsigned long long)s_frame_count,
                    v ? "a scene under OUR camera - the room is drawn as in play (our walk, union, showall, two-sided)"
                      : "scene rules follow the game's camera again", rc, 10);
        }
        lv = v;
    }
    return v;
}

static int stairs_scene_here(void) {
    static char rooms[96]; static int have = 0;
    if (!have) { const char *e = getenv("DC_STAIRS_ROOM_ROOMS"); snprintf(rooms, sizeof rooms, ",%s,", e ? e : "119,66"); have = 1; }
    if (!ram || g_dc_orig_mode) return 0;
    if (ram[0xB92C8] != 4 || ram[0xB93DE] != 2 || ram[0xB93DF] != 0 || ram[0xB7878] != 3) return 0;
    extern int dc_room_current(void);
    char key[16]; snprintf(key, sizeof key, ",%d,", dc_room_current());
    if (!strstr(rooms, key)) return 0;
    if (!dc_scene_zonecam_ours()) return 0;
    return 1;
}
int dc_scene_stairs_room(void) {
    static int en = -1;
    if (en < 0) { const char *e = getenv("DC_STAIRS_ROOM"); en = (e && *e == '0') ? 0 : 1; }
    if (!en || !stairs_scene_here()) return 0;
    { static unsigned long long said_f = 0; extern uint64_t s_frame_count;
      if ((unsigned long long)s_frame_count > said_f + 120ull)
          fprintf(stderr, "stairs: room %d - Regina's scripted walk under our camera: the room is drawn by our walk with the current piece "
                          "matrices (DC_STAIRS_ROOM=0 - the game draws it; DC_STAIRS_NOHIST=0 - with history, as 04.10 (3))%c",
                  dc_room_current(), 10);
      said_f = (unsigned long long)s_frame_count; }
    return 1;
}
int dc_scene_stairs_keep(void) {
    static int en = -1;
    if (en < 0) { const char *e = getenv("DC_STAIRS_KEEP"); en = (e && *e == '0') ? 0 : 1; }
    if (!en || !stairs_scene_here()) return 0;
    { static unsigned long long said_f = 0; extern uint64_t s_frame_count;
      if ((unsigned long long)s_frame_count > said_f + 120ull)
          fprintf(stderr, "stairs: room %d - Regina's scripted walk under our camera: the game draws the room, the pieces its shots hide "
                          "stay on (union and showall as in play) (DC_STAIRS_KEEP=0 off)%c", dc_room_current(), 10);
      said_f = (unsigned long long)s_frame_count; }
    return 1;
}

static unsigned long long s_zone_chg_f = 0;
static void dc_zone_change_tick(unsigned long long gfq) {
    static int zc = -1, zr = -1;
    if (!ram) return;
    const int c = ram[0xB796C], rr = ram[0xB796E];
    if (zc >= 0 && (c != zc || rr != zr)) s_zone_chg_f = gfq;
    zc = c; zr = rr;
}

static uint64_t s_ours_flip = 0;
static int dc_ours_shown_now(void) {
    extern uint64_t gpu_flip_seq(void);
    return !g_dc_orig_mode && ram && ram[0xB7878] == 3 && s_ours_flip && gpu_flip_seq() - s_ours_flip <= 2ull;
}
static int dc_cam_scriptsnap_ok(const uint8_t *r, float px, float pz, unsigned long long gfq) {
    static int en = -1;
    if (en < 0) { const char *e = getenv("DC_CAM_SCRIPTSNAP"); en = (e && *e == '0') ? 0 : 1; }
    static float lx = 0.f, lz = 0.f; static unsigned long long lf = 0; static int lh = 0;
    const int stable = lh && gfq > lf && gfq - lf <= 2ull && fabsf(px - lx) < 120.f && fabsf(pz - lz) < 120.f;
    lx = px; lz = pz; lf = gfq; lh = 1;
    if (!en || !r) return 0;
    const int held = (r[0xB7890] & 4) || (r[0xB8F30] & 4) || r[0xB92C8] == 4;

    const int zchg = s_zone_chg_f && gfq >= s_zone_chg_f && gfq - s_zone_chg_f <= 2ull && r[0xB7878] == 3;
    { static long n = 0;
      if (!held && !zchg && ++n <= 12) fprintf(stderr, "notsgate: f=%llu far sample %.0f,%.0f, the script does not hold her (GS+64 %02X, scene %02X, mode %d) - waiting the old way%c",
                                               gfq, px, pz, r[0xB7890], r[0xB8F30], r[0xB92C8], 10); }
    return (held && stable) || zchg;
}
int dc_scene_cam_ready(void) {
    if (dc_scene_zonecam_ours()) return 0;
    { static int zr = -1; if (zr < 0) { const char *e = getenv("DC_SCENE_ZONECAM_OURS"); zr = (e && *e == '0') ? 0 : 1; }
      if (zr && !g_dc_orig_mode && ram && ram[0xB7878] == 3 && dc_game_cam_zone() == 0) return 1; }
    static int wait = -1;
    if (wait < 0) { const char *e = getenv("DC_SCENE_CAM_WAIT"); wait = e ? atoi(e) : 45; if (wait < 0) wait = 0; }
    if (!wait) return 1;
    extern uint64_t s_frame_count;
    static unsigned long long last_vb = ~0ull; static int last_res = 1;
    if ((unsigned long long)s_frame_count == last_vb) return last_res;
    last_vb = (unsigned long long)s_frame_count;
    extern int dc_cut_camera(void);
    const int in = (dc_scripted_now() || dc_cut_camera()) ? 1 : 0;
    static int prev = 0, armed = 0; static unsigned long long t0 = 0; static int snap[9];
    int res = 1;
    if (in && !prev) {
        t0 = (unsigned long long)s_frame_count; armed = 1;
        for (int i = 0; i < 9; i++) snap[i] = (int)(int16_t)(ram[0xBF9F4 + i * 2] | (ram[0xBF9F4 + i * 2 + 1] << 8));
    }
    prev = in;
    if (!in) { armed = 0; last_res = 1; return 1; }
    if (armed) {
        int same = 1;
        for (int i = 0; i < 9 && same; i++)
            if ((int)(int16_t)(ram[0xBF9F4 + i * 2] | (ram[0xBF9F4 + i * 2 + 1] << 8)) != snap[i]) same = 0;

        { extern int dc_fmv_active(void);
          if (ram[0x000B92C8] == 4 || dc_fmv_active()) same = 0;

          extern int dc_script_shot_now(void);
          if (dc_script_shot_now()) same = 0;

          extern int dc_letterbox_scene_now(void);
          if (dc_letterbox_scene_now()) same = 0;

          extern int dc_regina_dead(void);
          if (dc_regina_dead()) same = 0; }
        if (!same || (unsigned long long)s_frame_count - t0 > (unsigned long long)wait) {
            armed = 0;
            static long n = 0;
            if (++n <= 40)
                fprintf(stderr, "scenecam: the game drives the camera from vblank %llu (shot %s, +%llu)%c",
                        (unsigned long long)s_frame_count, same ? "unchanged - by the guard" : "changed",
                        (unsigned long long)s_frame_count - t0, 10);
        } else {
            res = 0;
        }
    }
    last_res = res;
    return res;
}
int dc_nots_get_delta_submit(float D[9], float T[3]) {
    if (g_dc_orig_mode) return 0;
    if (dc_scene_no_delta()) return 0;

    { static int gc = -1;
      if (gc < 0) { const char *e = getenv("DC_SCENE_GAMECAM"); gc = (e && *e == '0') ? 0 : 1; }
      static int oc = -1;
      if (oc < 0) { const char *e = getenv("DC_NOTS_ORIGCUT"); oc = (e && *e == '0') ? 0 : 1; }
      if (gc && oc) {
          extern int dc_cut_camera(void);
          if ((dc_scripted_now() || dc_cut_camera()) && dc_scene_cam_ready()) return 0;
      } }
    static int en = -1;
    if (en < 0) { const char *e = getenv("DC_ROOM_DELTA_PREV"); en = (e && *e == '0') ? 0 : 1; }
    if (en && s_nots_dphave) {
        for (int i = 0; i < 9; i++) D[i] = s_nots_Dp[i];
        for (int i = 0; i < 3; i++) T[i] = s_nots_Tp[i];
        return 1;
    }
    extern int dc_nots_get_delta(float D[9], float T[3]);
    return dc_nots_get_delta(D, T);
}
int dc_nots_get_delta(float D[9], float T[3]) {
    if (g_dc_orig_mode) return 0;
    if (dc_scene_no_delta()) return 0;
    static int en=-1; if(en<0){ const char*e=getenv("DC_NATIVE_OTS"); en=(e&&*e=='1')?1:0; }
    if (!en || !s_nots_dhave) return 0;

    static int oc=-1; if(oc<0){ const char*e=getenv("DC_NOTS_ORIGCUT"); oc=(e&&*e=='0')?0:1; }
    { int dc_scene_cam_ready(void);
      if (oc && dc_scripted_now() && dc_scene_cam_ready()) return 0; }

    { extern int dc_cut_camera(void); int dc_scene_cam_ready(void);
      if (oc && dc_cut_camera() && dc_scene_cam_ready()) return 0; }
    for(int i=0;i<9;i++) D[i]=s_nots_D[i];
    for(int i=0;i<3;i++) T[i]=s_nots_T[i];
    return 1;
}

int dc_nots_get_owncam(float R9[9], float T3[3]) {
    if (g_dc_orig_mode) return 0;
    if (dc_scene_no_delta()) return 0;
    static int en=-1; if(en<0){ const char*e=getenv("DC_NATIVE_OTS"); en=(e&&*e=='1')?1:0; }
    if (!en || !s_nots_dhave || !s_dNhave) return 0;
    static int oc=-1; if(oc<0){ const char*e=getenv("DC_NOTS_ORIGCUT"); oc=(e&&*e=='0')?0:1; }
    { int dc_scene_cam_ready(void); extern int dc_cut_camera(void);
      if (oc && (dc_scripted_now() || dc_cut_camera()) && dc_scene_cam_ready()) return 0; }
    for (int i = 0; i < 9; i++) R9[i] = s_dNp[i];
    T3[0] = -(s_dNp[0]*s_deyev[0] + s_dNp[1]*s_deyev[1] + s_dNp[2]*s_deyev[2]);
    T3[1] = -(s_dNp[3]*s_deyev[0] + s_dNp[4]*s_deyev[1] + s_dNp[5]*s_deyev[2]);
    T3[2] = -(s_dNp[6]*s_deyev[0] + s_dNp[7]*s_deyev[1] + s_dNp[8]*s_deyev[2]);
    return 1;
}

int dc_nots_warp_cams(float Nr[9], float er[3], float tr[3], float Nn[9], float en_[3], float tn[3],
                      unsigned long long *flip_r, unsigned long long *flip_n) {
    if (g_dc_orig_mode || dc_scene_no_delta()) return 0;
    static int nen = -1; if (nen < 0) { const char *e = getenv("DC_NATIVE_OTS"); nen = (e && *e == '1') ? 1 : 0; }
    if (!nen || !s_nots_dhave || !s_nots_dphave || !s_wD_have || !s_wDp_have) return 0;
    if (dc_scripted_now() || dc_scripted_edge()) return 0;
    { extern int dc_cut_camera(void); if (dc_cut_camera()) return 0; }
    memcpy(Nr, s_wDp_N, 9 * sizeof(float)); memcpy(er, s_wDp_eye, 3 * sizeof(float)); memcpy(tr, s_wDp_tgt, 3 * sizeof(float));
    memcpy(Nn, s_wD_N, 9 * sizeof(float));  memcpy(en_, s_wD_eye, 3 * sizeof(float)); memcpy(tn, s_wD_tgt, 3 * sizeof(float));
    if (flip_r) *flip_r = s_wDp_flip;
    if (flip_n) *flip_n = s_wD_flip;
    return 1;
}

int dc_nots_warp_turn(float *dcam, float *dher) {
    if (!s_wD_have || !s_wDp_have) return 0;
    float c = s_wD_cam - s_wDp_cam;
    while (c >  3.14159265f) c -= 6.2831853f;
    while (c < -3.14159265f) c += 6.2831853f;
    int h = (s_wD_her - s_wDp_her) & 4095; if (h > 2048) h -= 4096;
    if (dcam) *dcam = c;
    if (dher) *dher = (float)h * (6.2831853f / 4096.0f);
    return 1;
}

int dc_orig_gfx(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("DC_ORIG_GFX"); v = (e && *e == '0') ? 0 : 1; }
    return v;
}
int dc_orig_nogfx(void) { return g_dc_orig_mode && !dc_orig_gfx(); }
int dc_orig_native_draw(void) {
    if (!g_dc_orig_mode) return 0;
    static int nat = -1;
    if (nat < 0) { const char *e = getenv("DC_ORIG_NATIVE_DRAW"); nat = (e && *e == '1') ? 1 : 0; }
    return nat;
}

static int s_scene_room = -2;
static uint64_t s_sr_door_flip = 0;

void dc_scene_door_mark(void) {
    static int en = -1; if (en < 0) { const char *e = getenv("DC_SCENE_DOOR_MARK"); en = (e && *e == '0') ? 0 : 1; }
    extern uint64_t gpu_flip_seq(void);
    if (en) s_sr_door_flip = gpu_flip_seq();
}
int dc_scene_our_room(void) {
    static int en = -1; if (en < 0) { const char *e = getenv("DC_SCENE_OUR_ROOM"); en = (e && *e == '0') ? 0 : 1; }
    if (!en || dc_orig_nogfx()) return 0;
    static int nen = -1; if (nen < 0) { const char *e = getenv("DC_NATIVE_OTS"); nen = (e && *e == '1') ? 1 : 0; }
    if (!nen || !s_nots_dhave) return 0;
    if (dc_scene_no_delta()) return 0;
    { extern int dc_fmv_active(void); if (dc_fmv_active()) return 0; }
    uint8_t *r = memory_get_ram_ptr();
    if (r && (r[0xB7878] == 2 || r[0xB7878] == 6)) { extern uint64_t gpu_flip_seq(void); s_sr_door_flip = gpu_flip_seq(); }
    if (!r || r[0xB7878] != 3) return 0;

    { extern int dc_room_current(void); extern uint64_t gpu_flip_seq(void);
      static int cand = -3; static uint64_t since = 0, seen = 0;
      const int rc = dc_room_current();
      const uint64_t now = gpu_flip_seq();
      if (rc == s_scene_room || now - seen > 2) cand = -3;
      seen = now;
      if (rc != s_scene_room) {
          static int adopt = -1, adopt_door = -1;
          if (adopt < 0) { const char *e = getenv("DC_SCENE_ROOM_ADOPT"); adopt = e ? atoi(e) : 30;

                           e = getenv("DC_SCENE_ROOM_ADOPT_DOOR"); adopt_door = e ? atoi(e) : 6; }
          if (rc != cand) { cand = rc; since = now; }
          const int after_door = (s_sr_door_flip && since >= s_sr_door_flip && since - s_sr_door_flip <= 20);
          const int need = (after_door && adopt > 0 && adopt_door >= 0 && adopt_door < adopt) ? adopt_door : adopt;
          if (adopt <= 0 || rc < 0 || now - since < (uint64_t)need) return 0;
          fprintf(stderr, "scene room: the scene goes on in a new room %d (%llu vblanks) - our emitter draws it\n", rc,
                  (unsigned long long)(now - since));
          s_scene_room = rc;
      } }
    if (dc_scene_zonecam_ours()) return 0;
    { extern int dc_scene_flag_on(void); extern int dc_regina_dead(void);
      extern int dc_cut_camera(void);
      return (dc_scripted_now() || dc_scene_flag_on() || dc_regina_dead() || dc_cut_camera()) ? 1 : 0; }
}

int dc_twosided_scene_off(void) {
    static int en = -1, sc = -1;
    if (en < 0) { const char *e = getenv("DC_SCENE_TWOSIDED_ALL"); en = (e && *e == '0') ? 0 : 1;
                  e = getenv("DC_SCENE_TWOSIDED"); sc = (e && *e == '1') ? 1 : 0; }
    if (!en || sc || g_dc_orig_mode || !ram) return 0;
    extern uint64_t s_frame_count;
    static unsigned long long fr = ~0ull; static int v = 0;
    if ((unsigned long long)s_frame_count == fr) return v;
    fr = (unsigned long long)s_frame_count;
    extern int dc_scene_flag_on(void); extern int dc_cut_camera(void); extern int dc_regina_dead(void);
    v = ((dc_scripted_now() || dc_scene_flag_on() || dc_cut_camera() || dc_regina_dead()) && !dc_scene_stairs_room() && !dc_scene_stairs_keep()
         && !dc_scene_ourcam_play()) ? 1 : 0;
    { static int lv = -1;
      if (v != lv) { lv = v; fprintf(stderr, "twosided: %s (DC_SCENE_TWOSIDED_ALL=0 - only under the game's camera, as before)%c",
                                      v ? "cutscene - no two-sided faces at all, as on the console" : "back to play - two-sided as under our camera", 10); } }
    return v;
}
int dc_gamecam_twosided_ok(void) {
    static int sc = -1, og = -1;
    if (sc < 0) { const char *e = getenv("DC_SCENE_TWOSIDED"); sc = (e && *e == '1') ? 1 : 0;
                  e = getenv("DC_ORIG_TWOSIDED"); og = (e && *e == '1') ? 1 : 0; }
    if (dc_twosided_scene_off()) return 0;
    if (g_dc_orig_mode) return og;
    if (dc_scene_our_room()) return sc;
    return 1;
}
static void camsel_log(const char *branch) {
    static int dbg = -1; static long n = 0; static const char *last = "";
    if (dbg < 0) { const char *e = getenv("DC_CUT_DBG"); dbg = (e && *e == '1') ? 1 : 0; }
    if (!dbg) return;
    extern int dc_cut_camera(void); extern int dc_scene_flag_on(void); extern int dc_scene_our_room(void);
    if (branch == last && (++n % 120)) return;
    last = branch; n = 1;
    fprintf(stderr, "camsel: %s | cut=%d sceneflag=%d scripted=%d our_room=%d orig=%d\n",
            branch, dc_cut_camera(), dc_scene_flag_on(), dc_scripted_now(), dc_scene_our_room(), g_dc_orig_mode);
}

static int s_camsel_ours = 0;
int dc_camsel_ours_now(void) { return s_camsel_ours; }
int dc_room_get_delta(float D[9], float T[3]) {
    if (!g_dc_orig_mode) {
        if (dc_nots_get_delta(D, T)) { extern int dc_room_current(void); extern uint64_t gpu_flip_seq(void);
                                       s_scene_room = dc_room_current(); s_ours_flip = gpu_flip_seq(); s_camsel_ours = 1; camsel_log("OUR camera (nots delta)"); return 1; }
        if (!dc_scene_our_room()) { s_camsel_ours = 0; camsel_log("the room is not drawn (scene/movie)"); return 0; }
        for (int i = 0; i < 9; i++) D[i] = (i % 4 == 0) ? 1.0f : 0.0f;
        T[0] = T[1] = T[2] = 0.0f;
        s_camsel_ours = 0;
        camsel_log("the GAME's camera (our room in a scene)");
        return 1;
    }
    if (dc_orig_native_draw()) return 0;
    if (dc_scene_no_delta()) return 0;
    static int en=-1; if(en<0){ const char*e=getenv("DC_NATIVE_OTS"); en=(e&&*e=='1')?1:0; }
    if (!en || !s_nots_dhave) return 0;
    static int oc=-1; if(oc<0){ const char*e=getenv("DC_NOTS_ORIGCUT"); oc=(e&&*e=='0')?0:1; }
    { extern int dc_cut_camera(void);
      if (oc && (dc_scripted_now() || dc_cut_camera())) {

          if (!dc_orig_gfx() || !dc_scene_our_room()) return 0;
          for (int i = 0; i < 9; i++) D[i] = (i % 4 == 0) ? 1.0f : 0.0f;
          T[0] = T[1] = T[2] = 0.0f;
          camsel_log("O mode: the GAME's camera (our room in a scene)");
          return 1;
      } }
    for (int i = 0; i < 9; i++) D[i] = (i % 4 == 0) ? 1.0f : 0.0f;
    T[0] = T[1] = T[2] = 0.0f;
    { extern int dc_room_current(void); s_scene_room = dc_room_current(); }
    return 1;
}

static int   s_nots_ready = 0;
static uint64_t s_nots_ready_vb = 0;
static float s_nots_cam = 0.f;
static float s_nots_her = 0.f;

float g_dcns_corr = 0.f, g_dcns_cam = 0.f, g_dcns_her = 0.f;
int g_dcns_gate[6] = {0, 0, 0, 0, 0, 0}, g_dcns_her_yaw = 0;

int g_dc_aspect = -1;
static int dc_canvas_wide_env(void) {
    static int w = -1;
    if (w < 0) { const char *e = getenv("DC_WIDESCREEN"); w = (e && *e == '1') ? 1 : 0; }
    return w;
}
int dc_aspect_get(void) {
    if (g_dc_aspect < 0) {
        const char *e = getenv("DC_ASPECT");
        int m = e ? atoi(e) : 1;
        g_dc_aspect = (m < 0 || m > 2) ? 1 : m;
    }
    return g_dc_aspect;
}
static int s_ws_dirty = 1;
void dc_aspect_set(int m) {
    if (m < 0) m = 0;
    if (m > 2) m = 2;
    if (m != dc_aspect_get())
        fprintf(stderr, "aspect: %d -> %d (%s)\n", g_dc_aspect, m,
                m == 0 ? "4:3" : m == 1 ? "wide, interface 4:3" : "wide");
    g_dc_aspect = m;
    s_ws_dirty = 1;
}

static const struct { int w, h; } s_ratio_tab[] = {
    { 0, 0 }, { 4, 3 }, { 3, 2 }, { 16, 10 }, { 16, 9 }, { 64, 27 }, { 43, 18 }, { 12, 5 }, { 32, 9 } };
#define DC_RATIO_N ((int)(sizeof s_ratio_tab / sizeof s_ratio_tab[0]))
int g_dc_ratio = -1;
static int s_win_w = 16, s_win_h = 9;
static int s_ws_num = 3, s_ws_den = 4, s_ws_on = 1, s_cv_w = 854, s_pic_w = 16, s_pic_h = 9;
static float s_ws_sq = 0.75f;
int dc_ratio_get(void) {
    if (g_dc_ratio < 0) {
        const char *e = getenv("DC_RATIO");
        int m = e ? atoi(e) : 0;
        g_dc_ratio = (m < 0 || m >= DC_RATIO_N) ? 0 : m;
    }
    return g_dc_ratio;
}
void dc_ratio_set(int m) {
    if (m < 0) m = 0;
    if (m >= DC_RATIO_N) m = DC_RATIO_N - 1;
    if (m != dc_ratio_get())
        fprintf(stderr, "ratio: %d -> %d (%d:%d)\n", g_dc_ratio, m, s_ratio_tab[m].w, s_ratio_tab[m].h);
    g_dc_ratio = m;
    s_ws_dirty = 1;
}
void dc_ratio_window(int w, int h) {
    if (w <= 0 || h <= 0 || (w == s_win_w && h == s_win_h)) return;
    s_win_w = w; s_win_h = h;
    s_ws_dirty = 1;
}

static int s_orig_uncrop = 0;
int dc_orig_play(void) { return s_orig_uncrop; }
static int orig_uncrop_force(void) { static int v = -1; if (v < 0) { const char *e = getenv("DC_ORIG_UNCROP"); v = (e && *e == '1') ? 1 : 0; } return v; }
void dc_orig_uncrop_set(int on) {
    on = on ? 1 : 0;
    if (on == s_orig_uncrop) return;
    s_orig_uncrop = on;
    if (orig_uncrop_force()) s_ws_dirty = 1;
    fprintf(stderr, "ratio: original mode %s\n", on ? (orig_uncrop_force() ? "in play - picture not wider than the window" : "in play - the location drawn to the edges of a wide picture")
                                                    : "scene or modern - picture as chosen");
}
static long long ws_gcd(long long a, long long b) { while (b) { long long t = a % b; a = b; b = t; } return a ? a : 1; }
static void ws_update(void) {
    const int wide = dc_canvas_wide_env();
    const int r = dc_ratio_get();
    int pw = r ? s_ratio_tab[r].w : s_win_w, ph = r ? s_ratio_tab[r].h : s_win_h;
    s_ws_dirty = 0;
    if (r && s_orig_uncrop && orig_uncrop_force() && (long long)pw * s_win_h > (long long)ph * s_win_w) { pw = s_win_w; ph = s_win_h; }
    if (!wide || dc_aspect_get() == 0 || (long long)pw * 3 <= (long long)ph * 4) { pw = 4; ph = 3; }
    { const long long g = ws_gcd(pw, ph); s_pic_w = (int)(pw / g); s_pic_h = (int)(ph / g); }
    if (s_pic_w == 4 && s_pic_h == 3) { s_ws_on = 0; s_ws_num = 1; s_ws_den = 1; s_ws_sq = 1.0f; }
    else {
        const long long n = 4LL * s_pic_h, d = 3LL * s_pic_w, g = ws_gcd(n, d);
        s_ws_on = 1; s_ws_num = (int)(n / g); s_ws_den = (int)(d / g);
        s_ws_sq = (float)((double)s_ws_num / (double)s_ws_den);
    }
    if (!wide) s_cv_w = 640;
    else {
        const long long W = (480LL * s_win_w + s_win_h - 1) / s_win_h;
        s_cv_w = W < 640 ? 640 : (W > 4096 ? 4096 : (int)W);
    }
    fprintf(stderr, "ratio: picture %d:%d, world x%.4f (%d/%d), canvas %dx480 (window %dx%d)\n",
            s_pic_w, s_pic_h, s_ws_sq, s_ws_num, s_ws_den, s_cv_w, s_win_w, s_win_h);
}
int dc_ws_world(void)     { if (s_ws_dirty) ws_update(); return s_ws_on; }
int dc_ui43_squeeze(void) { return dc_ws_world() && dc_aspect_get() == 1; }
float dc_ws_sq(void)      { if (s_ws_dirty) ws_update(); return s_ws_sq; }
void dc_ws_frac(int *num, int *den) { if (s_ws_dirty) ws_update(); *num = s_ws_num; *den = s_ws_den; }

float dc_ws_frame_k(void) { const float s = dc_ws_sq(); return s < 0.75f ? 0.75f : s; }
int dc_canvas_w(void)     { if (s_ws_dirty) ws_update(); return s_cv_w; }
void dc_pic_ratio(int *w, int *h) { if (s_ws_dirty) ws_update(); *w = s_pic_w; *h = s_pic_h; }

int g_dc_orig_mode = 0;

int dc_orig_pure(void) {
    static int en = -1;
    if (en < 0) { const char *e = getenv("DC_ORIG_PURE"); en = (e && *e == '0') ? 0 : 1; }
    return en && g_dc_orig_mode;
}
int dc_nots_active(void) {
    if (g_dc_orig_mode) return 0;
    static int en=-1; if(en<0){ const char*e=getenv("DC_NATIVE_OTS"); en=(e&&*e=='1')?1:0; }
    if (!en || !s_nots_ready) return 0;

    {
        extern uint64_t s_frame_count;
        if (s_frame_count - s_nots_ready_vb > 30) return 0;
    }
    return 1;
}
void dc_nots_readiness(int *ready, uint64_t *age) {
    extern uint64_t s_frame_count;
    if (ready) *ready = s_nots_ready;
    if (age) *age = s_nots_ready ? s_frame_count - s_nots_ready_vb : 0;
}
static int s_nots_her_yaw = 0;
float dc_nots_rel(float* S) {
    if (S) *S = (6.2831853f/4096.0f);
    float her = s_nots_her;

    { static int en = -1;
      if (en < 0) { const char *e = getenv("DC_REL_LIVE"); en = (e && *e == '0') ? 0 : 1; }
      if (en) { extern uint8_t *memory_get_ram_ptr(void); uint8_t *rm = memory_get_ram_ptr();
          if (rm) { int y = (int16_t)(rm[0xB92B6] | (rm[0xB92B7] << 8));
                    int d = ((y - s_nots_her_yaw) & 4095); if (d > 2048) d -= 4096;
                    if (d > -300 && d < 300) her += (float)d * (6.2831853f/4096.0f); } } }
    float r = s_nots_cam - her;
    while (r >  3.14159265f) r -= 6.2831853f;
    while (r < -3.14159265f) r += 6.2831853f;
    return r;
}

int dc_nots_rel_yaw(void) {
    static int en = -1;
    if (en < 0) { const char *e = getenv("DC_REL_LIVE"); en = (e && *e == '0') ? 0 : 1; }
    if (!en) return s_nots_her_yaw;
    extern uint8_t *memory_get_ram_ptr(void); uint8_t *rm = memory_get_ram_ptr();
    if (!rm) return s_nots_her_yaw;
    const int y = (int16_t)(rm[0xB92B6] | (rm[0xB92B7] << 8));
    int d = ((y - s_nots_her_yaw) & 4095); if (d > 2048) d -= 4096;
    return (d > -300 && d < 300) ? s_nots_her_yaw + d : s_nots_her_yaw;
}

static float s_nots_oyaw_pub = 0.f; static int s_nots_oyaw_pub_have = 0;
float dc_nots_course_lead(void) {
    if (!s_nots_oyaw_pub_have) return 0.f;
    float d = s_nots_oyaw - s_nots_oyaw_pub;
    while (d >  3.14159265f) d -= 6.2831853f;
    while (d < -3.14159265f) d += 6.2831853f;
    return (d > -0.5f && d < 0.5f) ? d : 0.f;
}

int dc_nots_dump_armed(void) {
    static int poll = -1, n = 0, armed = 0;
    if (poll < 0) { const char *e = getenv("DC_NOTS_DUMP_POLL"); poll = (e && *e == '1') ? 1 : 0; }
    (void)poll; (void)n; armed = 0;
    return armed;
}
static int s_nots_gate_far = 0;

static void dc_camcoll_dump(CPUState* cpu) {
    static int en = -1, done = 0;
    if (en < 0) { const char* e = getenv("DC_CAMCOLL_DUMP"); en = (e && *e == '1') ? 1 : 0; }
    if (!en || done) return;
    uint32_t ctx = cpu->read_word(0x1F800000u) & 0x1FFFFFu;
    if (!ctx || ctx + 400u >= RAM_SIZE) return;
    uint32_t sec = cpu->read_word(ctx + 340u) & 0x1FFFFFu;
    if (!sec || sec + 64u >= RAM_SIZE) return;
    done = 1;
    fprintf(stderr, "camcoll: ctx=%06X sec(ctx+340)=%06X%c", ctx, sec, 10);
    for (int r = 0; r < 4; r++) {
        fprintf(stderr, "  sec+%02d:", r * 16);
        for (int c = 0; c < 4; c++)
            fprintf(stderr, " %08X", cpu->read_word(sec + (uint32_t)(r * 16 + c * 4)));
        fprintf(stderr, "%c", 10);
    }

    for (int k = 0; k < 6; k++) {
        uint32_t p = cpu->read_word(sec + (uint32_t)k * 4);
        uint32_t pm = p & 0x1FFFFFu;
        if (p >= 0x80000000u && pm + 32u < RAM_SIZE) {
            fprintf(stderr, "  ptr[%d]=%08X ->", k, p);
            for (int j = 0; j < 8; j++)
                fprintf(stderr, " %6d", (int16_t)cpu->read_half(pm + (uint32_t)j * 2));
            fprintf(stderr, "%c", 10);
        } else fprintf(stderr, "  ptr[%d]=%08X%c", k, p, 10);
    }
}

int g_dc_pass_id   = 0;
int g_dc_passdbg   = -1;
int g_dc_dma_pass  = -1;
int g_dc_body_pass = -1;

static float s_gf_lt[3], s_gf_lD = 1700.f, s_gf_lp = 0.35f; static int s_gf_lhave = 0;

static float s_gfp_fast = 0.f;

static int dc_actor_is_trex(const uint8_t *r, int slot) {
    static int kind = -1, en = -1;
    if (kind < 0) { const char *e = getenv("DC_TREX_KIND"); kind = e ? (int)strtol(e, NULL, 0) : 0x0A; }
    if (en < 0) { const char *e = getenv("DC_TREX_RULES"); en = (e && *e == '0') ? 0 : 1; }
    if (!en || !r || slot < 0 || slot >= 10) return 0;
    return r[0xB7ACCu + (uint32_t)slot * 608u + 80u] == (uint8_t)kind;
}
extern int dc_actor_box(int slot, float lo[3], float hi[3], float (*pts)[3], int maxp, int *np);
#define GFP_MAXP 64
#define GFP_DB   40
typedef struct {
    float p[GFP_MAXP][3], rad[GFP_MAXP]; int n;
    float tp[4][3]; int nt;
    float db[GFP_DB][3]; int nd;
    float C[3], Rc[3];
    float axis[2]; int has_axis, dino, box, grab, trex;
} GfSubj;
static float gfp_wrap(float a) { const float PI = 3.14159265f; return a - 2.f * PI * floorf((a + PI) / (2.f * PI)); }

static float gfp_ease(float u) {
    if (u <= 0.f) return 0.f;
    if (u >= 1.f) return 1.f;
    const float v = 1.f - u, v2 = v * v;
    return 1.f - v2 * v2 * v * (1.f + 5.f * u);
}
static void gfp_subject(GfSubj *S, const float (*reg)[3], int nreg, int dk, const float dp[3], int lying,
                        float px, float py, float pz, int trex) {
    S->n = 0; S->nt = 0; S->nd = 0; S->has_axis = 0; S->dino = 0; S->box = 0; S->grab = 0; S->trex = trex;
    float lo[3] = { 1e9f, 1e9f, 1e9f }, hi[3] = { -1e9f, -1e9f, -1e9f };
    #define GFP_ADD(pp, rr) do { if (S->n < GFP_MAXP) { for (int q_ = 0; q_ < 3; q_++) S->p[S->n][q_] = (pp)[q_]; \
        S->rad[S->n] = (rr); S->n++; } } while (0)
    if (nreg >= 3) {
        for (int i = 0; i < nreg; i++) { GFP_ADD(reg[i], 170.f);
            for (int q = 0; q < 3; q++) { if (reg[i][q] < lo[q]) lo[q] = reg[i][q]; if (reg[i][q] > hi[q]) hi[q] = reg[i][q]; } }
    } else {
        const float c[3] = { px, py - 500.f, pz }; GFP_ADD(c, 600.f);
        for (int q = 0; q < 3; q++) { lo[q] = c[q] - 500.f; hi[q] = c[q] + 500.f; }
    }
    for (int q = 0; q < 3; q++) S->Rc[q] = 0.5f * (lo[q] + hi[q]);

    for (int q = 0; q < 3; q++) S->tp[0][q] = S->Rc[q];
    S->nt = 1;
    if (nreg >= 3) {
        int ia = 0, ib = 1; float dmax2 = -1.f;
        for (int i = 0; i < nreg; i++) for (int j = i + 1; j < nreg; j++) {
            const float dx = reg[i][0] - reg[j][0], dy = reg[i][1] - reg[j][1], dz = reg[i][2] - reg[j][2];
            const float d2 = dx * dx + dy * dy + dz * dz;
            if (d2 > dmax2) { dmax2 = d2; ia = i; ib = j; }
        }
        for (int q = 0; q < 3; q++) { S->tp[1][q] = reg[ia][q]; S->tp[2][q] = reg[ib][q]; }
        S->nt = 3;
        if (lying) {
            const float ax = reg[ib][0] - reg[ia][0], az = reg[ib][2] - reg[ia][2], l = sqrtf(ax * ax + az * az);
            if (l > 300.f) { S->axis[0] = ax / l; S->axis[1] = az / l; S->has_axis = 1; }
        }
    }
    if (dk >= 0) {
        float blo[3], bhi[3], bp[GFP_DB][3]; int nbp = 0;
        float dc[3];
        const int hb = dc_actor_box(dk, blo, bhi, bp, GFP_DB, &nbp);
        { static int dbg = -1; static unsigned n = 0;
          if (dbg < 0) { const char *e = getenv("DC_CAM_GRABPLAN_DBG"); dbg = (e && *e == '1') ? 1 : 0; }
          if (dbg && hb && (n++ % 30) == 0) {
              fprintf(stderr, "grabplan: enemy %d box %.0f,%.0f,%.0f .. %.0f,%.0f,%.0f root %.0f,%.0f,%.0f her %.0f,%.0f,%.0f bones %d:",
                      dk, blo[0], blo[1], blo[2], bhi[0], bhi[1], bhi[2], dp[0], dp[1], dp[2], S->Rc[0], S->Rc[1], S->Rc[2], nbp);
              for (int i = 0; i < nbp && i < 12; i++) fprintf(stderr, " %.0f,%.0f,%.0f", bp[i][0], bp[i][1], bp[i][2]);
              fprintf(stderr, "%c", 10); } }
        int nin = 0;
        if (hb) {

            float sx = 0.f, sy = 0.f, sz = 0.f;
            for (int i = 0; i < nbp; i++) {
                const float dx = bp[i][0] - S->Rc[0], dy = bp[i][1] - S->Rc[1], dz = bp[i][2] - S->Rc[2];
                const float d2 = dx * dx + dy * dy + dz * dz;
                if (d2 < 1700.f * 1700.f) { GFP_ADD(bp[i], 300.f); sx += bp[i][0]; sy += bp[i][1]; sz += bp[i][2]; nin++; }
                if (d2 > 450.f * 450.f && S->nd < GFP_DB) { for (int q = 0; q < 3; q++) S->db[S->nd][q] = bp[i][q]; S->nd++; }
            }
            if (nin) { dc[0] = sx / (float)nin; dc[1] = sy / (float)nin; dc[2] = sz / (float)nin; S->box = 1; }
        }
        if (!nin) {
            const float rx = dp[0] - S->Rc[0], rz = dp[2] - S->Rc[2], l0 = sqrtf(rx * rx + rz * rz);
            float q0[3] = { dp[0], dp[1], dp[2] };
            if (l0 > 1500.f) { q0[0] = S->Rc[0] + rx * 1500.f / l0; q0[2] = S->Rc[2] + rz * 1500.f / l0; }
            for (int k = 0; k < 3; k++) { const float c[3] = { q0[0], dp[1] - 150.f - 500.f * (float)k, q0[2] }; GFP_ADD(c, 420.f); }
            if (l0 > 450.f && S->nd < GFP_DB) { S->db[S->nd][0] = dp[0]; S->db[S->nd][1] = dp[1] - 650.f; S->db[S->nd][2] = dp[2]; S->nd++; }
            dc[0] = q0[0]; dc[1] = dp[1] - 650.f; dc[2] = q0[2];
        }

        float ax = dp[0] - S->Rc[0], az = dp[2] - S->Rc[2], l = sqrtf(ax * ax + az * az);
        if (!trex || l <= 300.f) { ax = dc[0] - S->Rc[0]; az = dc[2] - S->Rc[2]; l = sqrtf(ax * ax + az * az); }
        if (l > 200.f) { S->axis[0] = ax / l; S->axis[1] = az / l; S->has_axis = 1; }
        S->dino = 1;
    }
    #undef GFP_ADD
    float clo[3] = { 1e9f, 1e9f, 1e9f }, chi[3] = { -1e9f, -1e9f, -1e9f };
    for (int i = 0; i < S->n; i++) for (int q = 0; q < 3; q++) {
        if (S->p[i][q] - S->rad[i] < clo[q]) clo[q] = S->p[i][q] - S->rad[i];
        if (S->p[i][q] + S->rad[i] > chi[q]) chi[q] = S->p[i][q] + S->rad[i];
    }
    for (int q = 0; q < 3; q++) S->C[q] = 0.5f * (clo[q] + chi[q]);
}

static void gfp_predict(GfSubj *S, const float v[3]) {
    const float sp = sqrtf(v[0] * v[0] + v[2] * v[2]);
    if (sp < 20.f) return;
    static const float L[4] = { 400.f, 800.f, 1200.f, 1500.f };
    float e[3] = { S->Rc[0], S->Rc[1], S->Rc[2] };
    for (int k = 0; k < 4 && S->n < GFP_MAXP; k++) {
        e[0] = S->Rc[0] + v[0] / sp * L[k]; e[2] = S->Rc[2] + v[2] / sp * L[k];
        for (int q = 0; q < 3; q++) S->p[S->n][q] = e[q];
        S->rad[S->n] = 400.f; S->n++;
    }
    if (S->nt < 4) { for (int q = 0; q < 3; q++) S->tp[S->nt][q] = e[q]; S->nt++; }
    float clo[3] = { 1e9f, 1e9f, 1e9f }, chi[3] = { -1e9f, -1e9f, -1e9f };
    for (int i = 0; i < S->n; i++) for (int q = 0; q < 3; q++) {
        if (S->p[i][q] - S->rad[i] < clo[q]) clo[q] = S->p[i][q] - S->rad[i];
        if (S->p[i][q] + S->rad[i] > chi[q]) chi[q] = S->p[i][q] + S->rad[i];
    }
    for (int q = 0; q < 3; q++) S->C[q] = 0.5f * (clo[q] + chi[q]);
}

static float gfp_fit(const GfSubj *S, const float C[3], float yaw, float pitch) {
    const float cy = cosf(yaw), sy = sinf(yaw), cp = cosf(pitch), sp = sinf(pitch);
    const float fd[3] = { sy * cp, sp, cy * cp }, rt[3] = { cy, 0.f, -sy };
    const float up[3] = { fd[1] * rt[2] - fd[2] * rt[1], fd[2] * rt[0] - fd[0] * rt[2], fd[0] * rt[1] - fd[1] * rt[0] };
    const float th = 0.5f * 0.78f, tv = 0.375f * 0.78f;
    float need = 0.f;
    for (int i = 0; i < S->n; i++) {
        const float v[3] = { S->p[i][0] - C[0], S->p[i][1] - C[1], S->p[i][2] - C[2] };
        const float x = fabsf(v[0] * rt[0] + v[2] * rt[2]) + S->rad[i];
        const float y = fabsf(v[0] * up[0] + v[1] * up[1] + v[2] * up[2]) + S->rad[i];
        const float z = v[0] * fd[0] + v[1] * fd[1] + v[2] * fd[2];
        const float dh = x / th - z, dv = y / tv - z;
        if (dh > need) need = dh;
        if (dv > need) need = dv;
    }
    return need;
}

static float gfp_clear(const float C[3], float yaw, float pitch, float D, int whiskers) {
    if (s_cammesh_n <= 0 || D < 1.f) return 1.f;
    const float cp = cosf(pitch), sp = sinf(pitch);
    const float E[3] = { C[0] - sinf(yaw) * cp * D, C[1] - sp * D, C[2] - cosf(yaw) * cp * D };
    float t = cammesh_hit_t(s_cammesh, s_cammesh_n, C, E);
    if (whiskers) for (int w = -1; w <= 1; w += 2) {
        const float ya = yaw + 0.384f * (float)w;
        const float dv[3] = { -sinf(ya) * cp * D * 1.25f, -sp * D * 1.25f, -cosf(ya) * cp * D * 1.25f };
        const float tw = cammesh_ray_pivot(s_cammesh, s_cammesh_n, C, dv) * 1.25f;
        if (tw < t) t = tw;
    }
    return t > 1.f ? 1.f : t;
}

static void gfp_occl(const GfSubj *S, const float E[3], float *wall, float *body) {
    int nw = 0, nb = 0;
    for (int i = 0; i < S->nt; i++) {
        const float v[3] = { S->tp[i][0] - E[0], S->tp[i][1] - E[1], S->tp[i][2] - E[2] };
        if (s_cammesh_n > 0 && cammesh_ray_t(s_cammesh, s_cammesh_n, E, v) < 0.92f) nw++;
        const float vv = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
        if (vv < 1.f) continue;
        for (int j = 0; j < S->nd; j++) {
            const float w[3] = { S->db[j][0] - E[0], S->db[j][1] - E[1], S->db[j][2] - E[2] };
            const float s = (w[0] * v[0] + w[1] * v[1] + w[2] * v[2]) / vv;
            if (s < 0.05f || s > 0.95f) continue;
            const float c[3] = { w[0] - v[0] * s, w[1] - v[1] * s, w[2] - v[2] * s };
            if (c[0] * c[0] + c[1] * c[1] + c[2] * c[2] < 230.f * 230.f) { nb++; break; }
        }
    }
    *wall = S->nt ? (float)nw / (float)S->nt : 0.f;
    *body = S->nt ? (float)nb / (float)S->nt : 0.f;
}
typedef struct { float s, y, p, De, t, fit, ow, ob; } GfCand;

static float gfp_eval(const GfSubj *S, const float C[3], float yaw, float pitch, float yref, float ppref,
                      float dmin, float dmax, float rotw, GfCand *o) {
    const float PI = 3.14159265f;
    const float fit = gfp_fit(S, C, yaw, pitch);
    float D = fit > dmin ? fit : dmin; if (D > dmax) D = dmax;
    const float t = gfp_clear(C, yaw, pitch, D, 1), De = t * D;
    const float cp = cosf(pitch), sp = sinf(pitch);
    const float E[3] = { C[0] - sinf(yaw) * cp * De, C[1] - sp * De, C[2] - cosf(yaw) * cp * De };
    float ow = 0.f, ob = 0.f; gfp_occl(S, E, &ow, &ob);

    int clut = 0;
    { const float fd[3] = { sinf(yaw) * cp, sp, cosf(yaw) * cp };
      for (int j = 0; j < S->nd; j++) {
          const float v[3] = { S->db[j][0] - E[0], S->db[j][1] - E[1], S->db[j][2] - E[2] };
          const float z = v[0] * fd[0] + v[1] * fd[1] + v[2] * fd[2];
          if (z <= 100.f || z > 0.55f * De) continue;
          const float c[3] = { v[0] - fd[0] * z, v[1] - fd[1] * z, v[2] - fd[2] * z };
          if (c[0] * c[0] + c[1] * c[1] + c[2] * c[2] < 0.2f * z * z) clut++;
      } }
    float s = 2.5f * t;
    s -= 1.2f * (clut >= 4 ? 1.f : (float)clut / 4.f);

    { static int bb_n = -1, bb_room = -3; static float bb_lo[3], bb_hi[3];
      if (s_cammesh_n > 0 && (bb_n != s_cammesh_n || bb_room != s_cammesh_room)) {
          bb_n = s_cammesh_n; bb_room = s_cammesh_room;
          for (int q = 0; q < 3; q++) { bb_lo[q] = 1e9f; bb_hi[q] = -1e9f; }
          for (int i = 0; i < s_cammesh_n; i++) for (int k = 0; k < 3; k++) for (int q = 0; q < 3; q++) {
              const float v = s_cammesh[i][k * 3 + q];
              if (v < bb_lo[q]) bb_lo[q] = v; if (v > bb_hi[q]) bb_hi[q] = v;
          }
      }
      if (S->trex && s_cammesh_n > 0 && (E[0] < bb_lo[0] - 150.f || E[0] > bb_hi[0] + 150.f || E[2] < bb_lo[2] - 150.f || E[2] > bb_hi[2] + 150.f))
          s -= 3.f; }
    if (De < fit) s -= 3.f * (1.f - De / (fit > 1.f ? fit : 1.f));
    s -= 2.f * ow + 1.5f * ob;
    s -= rotw * fabsf(gfp_wrap(yaw - yref)) / PI;
    s -= 0.7f * fabsf(pitch - ppref);
    if (S->has_axis) {
        const float c = sinf(yaw) * S->axis[0] + cosf(yaw) * S->axis[1];

        if (S->dino) s -= (S->grab && S->trex ? 3.0f : 0.7f) * (c < 0.f ? -c : 0.f);
        else s -= 0.25f * fabsf(c);
    }
    if (o) { o->s = s; o->y = yaw; o->p = pitch; o->De = De; o->t = t; o->fit = fit; o->ow = ow; o->ob = ob; }
    return s;
}

static GfCand gfp_search(const GfSubj *S, float yref, float ppref, float dmin, float dmax,
                         float y0, float p0, float d0, float rotw, int dbg, int *ncand) {
    static const float PS[5] = { 0.45f, 0.60f, 0.75f, 0.90f, 1.05f };
    const float PI = 3.14159265f;
    GfCand top[6]; int nt = 0;
    *ncand = 0;

    const int face = S->grab && S->trex && S->has_axis;
    const float yax = atan2f(S->axis[0], S->axis[1]);
    const int kcur = face && fabsf(gfp_wrap(yref - yax)) < 0.70f;
    const int k0 = face ? -2 : 0, k1 = face ? (kcur ? 3 : 2) : 23;
    for (int k = k0; k <= k1; k++) for (int j = 0; j < 5; j++) {
        const float yv = face ? (k == 3 ? yref : gfp_wrap(yax + (float)k * PI / 12.f)) : gfp_wrap(yref + (float)k * PI / 12.f);
        GfCand c; gfp_eval(S, S->C, yv, PS[j], yref, ppref, dmin, dmax, rotw, &c);
        (*ncand)++;
        int pos = nt; while (pos > 0 && top[pos - 1].s < c.s) pos--;
        if (pos < 6) { for (int m = (nt < 6 ? nt : 5); m > pos; m--) top[m] = top[m - 1]; top[pos] = c; if (nt < 6) nt++; }
    }
    int bi = 0; float bs = -1e9f;
    for (int i = 0; i < nt; i++) {
        float tmin = 1.f;
        for (int h = 1; h <= 2; h++) {
            const float u = 0.35f * (float)h;
            const float yu = y0 + gfp_wrap(top[i].y - y0) * u, pu = p0 + (top[i].p - p0) * u, du = d0 + (top[i].De - d0) * u;
            const float tu = gfp_clear(S->C, yu, pu, du, 0);
            if (tu < tmin) tmin = tu;
        }
        const float s = top[i].s - 0.6f * (1.f - tmin);
        if (dbg) fprintf(stderr, "grabplan:   cand %d yaw %+.0f pitch %.2f dist %.0f fit %.0f free %.2f walls %.2f body %.2f path %.2f -> %.2f%c",
                         i, gfp_wrap(top[i].y - yref) * 180.f / PI, top[i].p, top[i].De, top[i].fit, top[i].t, top[i].ow, top[i].ob, tmin, s, 10);
        if (s > bs) { bs = s; bi = i; }
    }
    GfCand r = top[bi]; r.s = bs;
    return r;
}
static struct {
    int on, link, dino, have, since, bad, touch, rest, moving, replans;
    float y, p, d, ya, yb, pa, pb, da, db, u, T;
    float w, wa, wb, wu, wT;
    float Rs[3], Off[3], lastRc[3], vel[3];
    float pio0, ylast, effn;
    int trk, wtrk; float wait;
} G;
static float s_gfp_effn = 1.f;

static float gfplan_update(int cam_step, int on, int link, int lying, const float (*reg)[3], int nreg, int dk,
                           const float dp[3], float px, float py, float pz, float pitch_io, float dist_live,
                           const float tgt_n[3], float Fo[3], float *Do, float *pfo, int trex) {
    static int init = 0, dbg = 0; static float t_in = 0.55f, t_move = 0.60f, t_out = 0.60f, dmin_abs = 2200.f, dmax = 4000.f;
    const float PI = 3.14159265f;
    if (!init) { init = 1; const char *e;
        e = getenv("DC_CAM_GRABPLAN_IN");   if (e) t_in = (float)atof(e);
        e = getenv("DC_CAM_GRABPLAN_MOVE"); if (e) t_move = (float)atof(e);
        e = getenv("DC_CAM_GRABPLAN_OUT");  if (e) t_out = (float)atof(e);
        e = getenv("DC_CAM_GRABPLAN_DMIN"); if (e) dmin_abs = (float)atof(e);
        e = getenv("DC_CAM_GRABPLAN_DMAX"); if (e) dmax = (float)atof(e);
        e = getenv("DC_CAM_GRABPLAN_DBG");  dbg = (e && *e == '1') ? 1 : 0;
        if (t_in < 0.05f) t_in = 0.05f; if (t_move < 0.05f) t_move = 0.05f; if (t_out < 0.05f) t_out = 0.05f; }
    GfSubj S; gfp_subject(&S, reg, nreg, dk, dp, lying, px, py, pz, dk >= 0 ? trex : 0);
    S.grab = link;
    const float ycur = s_nots_oyaw;
    if (G.have && (G.on || G.w > 0.f) && fabsf(gfp_wrap(ycur - G.ylast)) > 1e-4f) {
        if (!G.touch) fprintf(stderr, "grabplan: the player turned the camera - his course, no auto plan for 2 s%c", 10);
        G.touch = 60; G.ya = G.yb = ycur; G.y = ycur;
    }
    const float dmin = (dist_live * 1.3f > dmin_abs) ? dist_live * 1.3f : dmin_abs;
    const float ppref = link ? 0.55f : 0.80f;
    const float cpio = G.p + (pitch_io - G.pio0);
    #define GFP_LOG(tag, c, nc, ms) fprintf(stderr, "grabplan: %s | her at %.0f,%.0f,%.0f%s%s | yaw %+.0f deg -> %.2f, pitch %.2f, dist %.0f (fit %.0f, free %.2f, hidden by walls %.2f / by the enemy %.2f) score %.2f | %d candidates %.1f ms%c", \
        tag, S.Rc[0], S.Rc[1], S.Rc[2], S.dino ? ", enemy in the frame" : "", S.dino && !S.box ? " (root only)" : "", \
        gfp_wrap((c).y - ycur) * 180.f / PI, (c).y, (c).p, (c).De * 0.97f, (c).fit, (c).t, (c).ow, (c).ob, (c).s, nc, ms, 10)
    if (on && !G.on) {
        const float wv = G.w;
        const float p0 = (wv > 0.f) ? pitch_io + (cpio - pitch_io) * wv : pitch_io;
        const float d0 = (wv > 0.f) ? dist_live + (G.d - dist_live) * wv : dist_live;
        const clock_t c0 = clock(); int nc = 0;
        GfSubj SP = S; if (!(link && S.trex)) gfp_predict(&SP, G.vel);
        const GfCand c = gfp_search(&SP, ycur, ppref, dmin, dmax, ycur, p0, d0, 0.5f, dbg, &nc);
        const float ms = (float)(clock() - c0) * 1000.f / (float)CLOCKS_PER_SEC;

        const int one = dc_cam_one_curve();
        const float Tm = one ? dc_camtr_dur(TR_GRAB_IN, fabsf(gfp_wrap(c.y - ycur)) * 57.2957795f)
                             : t_in + 0.35f * fabsf(gfp_wrap(c.y - ycur)) / PI;
        G.on = 1; G.link = link; G.dino = S.dino; G.have = 1;
        G.ya = ycur; G.yb = c.y; G.pa = G.pb = c.p; G.da = G.db = c.De * 0.97f; G.u = 0.f; G.T = Tm;
        G.wa = wv; G.wb = 1.f; G.wu = 0.f; G.wT = Tm;
        G.trk = G.wtrk = one ? TR_GRAB_IN + 1 : 0; G.wait = one ? s_camtr[TR_GRAB_IN].delay : 0.f;
        for (int q = 0; q < 3; q++) { G.Rs[q] = S.Rc[q]; G.Off[q] = S.C[q] - S.Rc[q]; G.lastRc[q] = S.Rc[q]; }
        G.pio0 = pitch_io; G.since = 0; G.bad = 0; G.touch = 0; G.rest = 0; G.replans = 0; G.moving = 0;
        G.effn = (wv > 0.f) ? 1.f : s_cam_eff_t;
        GFP_LOG(link ? "ENTER (grab)" : "ENTER (knocked down)", c, nc, ms);
        if (SP.n != S.n) fprintf(stderr, "grabplan:   she is still flying at %.0f per tick - the frame covers her path ahead%c",
                                 sqrtf(G.vel[0] * G.vel[0] + G.vel[2] * G.vel[2]), 10);
    } else if (!on && G.on) {
        G.on = 0; G.wa = G.w; G.wb = 0.f; G.wu = 0.f; G.wT = t_out;
        if (G.w >= 0.99f && s_cam_eff_t < 0.995f) { G.d *= s_cam_eff_t; G.da = G.db = G.d; }

        float best_y = G.y, t_here = 1.f, t_best = 1.f;
        if (!G.touch && s_cammesh_n > 0) {
            float best_s = -1e9f;
            for (int k = -12; k <= 12; k++) {
                const float yv = gfp_wrap(G.y + (float)k * PI / 12.f);
                const float t = gfp_clear(tgt_n, yv, pitch_io, dist_live, 1);
                if (!k) t_here = t;
                const float s = 2.5f * t - 0.6f * fabsf((float)k) / 12.f;
                if (s > best_s) { best_s = s; best_y = yv; t_best = t; }
            }
            if (t_here >= 0.9f) { best_y = G.y; t_best = t_here; }
        }
        G.ya = G.y; G.yb = best_y; G.pa = G.pb = G.p; G.da = G.db = G.d; G.u = 0.f;
        G.T = t_out + 0.35f * fabsf(gfp_wrap(best_y - G.y)) / PI; G.wT = G.T;
        G.trk = G.wtrk = 0; G.wait = 0.f;
        if (dc_cam_one_curve()) {
            G.T = dc_camtr_dur(TR_GRAB_OUT, fabsf(gfp_wrap(best_y - G.y)) * 57.2957795f); G.wT = G.T;
            G.trk = G.wtrk = TR_GRAB_OUT + 1; G.wait = s_camtr[TR_GRAB_OUT].delay;
        }
        fprintf(stderr, "grabplan: EXIT (plans %d) - back to the ordinary camera in %.2f s; its boom is free %.2f on this course -> turn %+.0f deg (free %.2f)%c",
                G.replans + 1, G.T, t_here, gfp_wrap(best_y - G.y) * 180.f / PI, t_best, 10);
    }
    if (cam_step) {
        if (G.wait > 0.f) G.wait -= 1.f / 30.f;
        else {
            G.u += (G.T > 0.01f) ? 1.f / (G.T * 30.f) : 1.f; if (G.u > 1.f) G.u = 1.f;
            G.wu += (G.wT > 0.01f) ? 1.f / (G.wT * 30.f) : 1.f; if (G.wu > 1.f) G.wu = 1.f;
        }
        float spd = 0.f;
        for (int q = 0; q < 3; q++) {
            G.Rs[q] += (S.Rc[q] - G.Rs[q]) * 0.35f;
            G.Off[q] += ((S.C[q] - S.Rc[q]) - G.Off[q]) * 0.06f;
            G.vel[q] += ((S.Rc[q] - G.lastRc[q]) - G.vel[q]) * 0.5f;
            spd += (S.Rc[q] - G.lastRc[q]) * (S.Rc[q] - G.lastRc[q]); G.lastRc[q] = S.Rc[q];
        }
        spd = sqrtf(spd);
        if (G.on) {
            G.since++; if (G.touch > 0) G.touch--;

            if (G.u >= 1.f && G.w >= 0.99f && s_cam_eff_t < 0.985f && G.d * s_cam_eff_t > 900.f) {
                G.ya = G.yb = G.y; G.pa = G.pb = G.p; G.da = G.d * s_cam_eff_t; G.db = G.da * 0.95f; G.u = 0.f; G.T = 0.4f; G.trk = 0;
                if (dbg) fprintf(stderr, "grabplan: boom pinned to %.2f - plan length %.0f -> %.0f%c", s_cam_eff_t, G.d, G.db, 10);
            }
            if (spd > 30.f) G.moving = 1;
            if (G.moving) { if (spd < 12.f) G.rest++; else G.rest = 0; }
            int replan = 0; const char *why = "";
            if (link != G.link) { replan = 2; why = link ? "grabbed" : "released"; }
            else if (S.dino && !G.dino) { replan = 1; why = "the enemy came close"; }
            else if (G.moving && G.rest >= 5) { replan = 1; why = "she came to rest"; G.moving = 0; G.rest = 0; }
            if (!S.dino && G.dino) G.dino = 0;
            float C[3]; for (int q = 0; q < 3; q++) C[q] = G.Rs[q] + G.Off[q];

            if (G.u >= 1.f && (G.since & 3) == 2) {
                const float fit = gfp_fit(&S, C, G.y, G.p);
                if (fit > G.d * 1.08f && G.d < dmax) {
                    float want = fit * 1.05f < dmax ? fit * 1.05f : dmax;
                    const float tf = gfp_clear(C, G.y, G.p, want, 1);
                    want *= tf * 0.97f;
                    if (want > G.d + 150.f) { G.ya = G.yb = G.y; G.pa = G.pb = G.p; G.da = G.d; G.db = want; G.u = 0.f; G.T = t_move; G.trk = 0; }
                }
            }
            if ((G.since & 3) == 0 && G.u >= 1.f && !replan) {
                const float t = gfp_clear(C, G.y, G.p, G.d, 1);
                const float cp = cosf(G.p), sp = sinf(G.p);
                const float E[3] = { C[0] - sinf(G.y) * cp * G.d * t, C[1] - sp * G.d * t, C[2] - cosf(G.y) * cp * G.d * t };
                float ow, ob; gfp_occl(&S, E, &ow, &ob);
                const float fit = gfp_fit(&S, C, G.y, G.p);
                const int bad = (t < 0.85f) || (ow >= 0.66f) || (ob >= 0.66f) || (G.d * t < fit * 0.8f);
                G.bad = bad ? G.bad + 1 : 0;
                if (G.bad >= 6) { replan = 1; why = (t < 0.85f) ? "boom pinned" : (ow >= 0.66f ? "walls hide her" : (ob >= 0.66f ? "the enemy hides her" : "the frame does not fit")); }
            }
            if (replan && G.since >= (replan == 2 ? 15 : 60) && (G.touch == 0 || replan == 2)) {
                const clock_t c0 = clock(); int nc = 0;
                const GfCand c = gfp_search(&S, G.y, ppref, dmin, dmax, G.y, cpio, G.d, 1.0f, dbg, &nc);
                const float ms = (float)(clock() - c0) * 1000.f / (float)CLOCKS_PER_SEC;
                GfCand cur; gfp_eval(&S, S.C, G.y, cpio, G.y, ppref, dmin, dmax, 0.f, &cur);
                G.link = link; G.dino = S.dino;
                if (replan == 2 || c.s > cur.s + 0.35f) {
                    G.ya = G.y; G.yb = c.y; G.pa = cpio; G.pb = c.p; G.da = G.d; G.db = c.De * 0.97f; G.u = 0.f;
                    G.T = t_move + 0.35f * fabsf(gfp_wrap(c.y - G.y)) / PI; G.trk = 0;
                    if (dc_cam_one_curve()) {
                        G.T = dc_camtr_dur(TR_GRAB_MOVE, fabsf(gfp_wrap(c.y - G.y)) * 57.2957795f);
                        G.trk = TR_GRAB_MOVE + 1; G.wait = s_camtr[TR_GRAB_MOVE].delay;
                    }
                    G.pio0 = pitch_io; G.since = 0; G.bad = 0; G.replans++;
                    char tag[96]; snprintf(tag, sizeof tag, "REPLAN (%s; current %.2f)", why, cur.s);
                    GFP_LOG(tag, c, nc, ms);
                } else {
                    G.bad = 0; G.since = -60;
                    fprintf(stderr, "grabplan: kept (%s): best %.2f is not better than the current %.2f%c", why, c.s, cur.s, 10);
                }
            }
            if (dbg && (G.since % 15) == 0)
                fprintf(stderr, "grabplan: w %.2f yaw %.3f pitch %.2f dist %.0f centre %.0f,%.0f,%.0f speed %.0f bad %d%c",
                        G.w, G.y, cpio, G.d, C[0], C[1], C[2], spd, G.bad, 10);
        }
    }
    #undef GFP_LOG
    const float e  = G.trk  ? dc_camtr_ease(G.trk - 1, G.u)   : gfp_ease(G.u);
    const float ew = G.wtrk ? dc_camtr_ease(G.wtrk - 1, G.wu) : gfp_ease(G.wu);
    G.y = gfp_wrap(G.ya + gfp_wrap(G.yb - G.ya) * e);
    G.p = G.pa + (G.pb - G.pa) * e;
    G.d = G.da + (G.db - G.da) * e;
    G.w = G.wa + (G.wb - G.wa) * ew;
    if (G.w < 0.001f && !G.on) G.w = 0.f;
    if (G.on || G.w > 0.f) s_nots_oyaw = G.y;
    G.ylast = s_nots_oyaw;
    for (int q = 0; q < 3; q++) Fo[q] = G.Rs[q] + G.Off[q];
    *Do = G.d;
    *pfo = G.p + (pitch_io - G.pio0);
    s_gfp_fast = G.w;
    s_gfp_effn = (G.on && G.wu < 1.f) ? G.effn : 1.f;
    return G.w;
}

static int dc_cam_scriptmove_any(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("DC_CAM_SCRIPTMOVE_ANY"); v = (e && *e == '0') ? 0 : 1; }
    return v;
}
static int dc_cam_scriptmove_now(const uint8_t *r) {
    return r && r[0xB93DE] == 2 && r[0xB93DF] == 0 && r[0xB92C8] != 5 && dc_cam_scriptmove_any();
}

static float push_ease(float p) {
    p = p < 0.f ? 0.f : (p > 1.f ? 1.f : p);
    return p * p * p * (p * (p * 6.f - 15.f) + 10.f);
}
static void push_spring(float *x, float *v, float target, float smooth, float dt) {
    const float om = 2.f / smooth, xx = om * dt, ex = 1.f / (1.f + xx + 0.48f * xx * xx + 0.235f * xx * xx * xx);
    const float ch = *x - target, tmp = (*v + om * ch) * dt;
    *v = (*v - om * tmp) * ex;
    *x = target + (ch + tmp) * ex;
}
static float pushcam_step(const uint8_t *r, int cam_step, float tgt[3], float dir[3], float *dist_live, float *pitch_io) {
    static int en = -1, v1 = 0;
    static float drop = 300.f, dadd = 600.f, padd = 0.25f, t_in = 0.6f, t_out = 0.8f, hold = 0.f, smooth = 0.35f;
    if (en < 0) {
        const char *e = getenv("DC_CAM_PUSH"); en = (e && *e == '0') ? 0 : 1;
        e = getenv("DC_CAM_PUSH_V1");    v1 = (e && *e == '1') ? 1 : 0;
        if (v1) { t_in = 0.5f; t_out = 1.0f; hold = 0.7f; }
        e = getenv("DC_CAM_PUSH_DROP");  if (e) drop = (float)atof(e);
        e = getenv("DC_CAM_PUSH_DIST");  if (e) dadd = (float)atof(e);
        e = getenv("DC_CAM_PUSH_PITCH"); if (e) padd = (float)atof(e);
        e = getenv("DC_CAM_PUSH_IN");    if (e) t_in = (float)atof(e);
        e = getenv("DC_CAM_PUSH_OUT");   if (e) t_out = (float)atof(e);
        e = getenv("DC_CAM_PUSH_HOLD");  if (e) hold = (float)atof(e);
        e = getenv("DC_CAM_PUSH_SMOOTH"); if (e) smooth = (float)atof(e);
        if (t_in < 0.05f) t_in = 0.05f; if (t_out < 0.05f) t_out = 0.05f; if (smooth < 0.02f) smooth = 0.02f;
    }
    if (!en || !r) return 0.f;
    extern int dc_cut_camera(void);
    const int act7 = !dc_cut_camera() && r[0xB92C8] == 1 && r[0xB92C9] == 7;
    const int push_now = act7 && (v1 || r[0xB92CA] < 3);
    static float w = 0.f, pp = 0.f; static int was = 0, after = 0;
    static float sp[3], sv[3], s_add = 0.f; static int sp_ok = 0;
    if (cam_step) after = push_now ? (int)(hold * 30.f + 0.5f) : (after > 0 ? after - 1 : 0);
    const int pushing = push_now || (after > 0 && r[0xB92C8] == 1 && !dc_cut_camera());
    if (cam_step) {
        if (v1) {
            const float k = 1.f - powf(0.05f, 1.f / ((pushing ? t_in : t_out) * 30.f));
            w += ((pushing ? 1.f : 0.f) - w) * k;
            if (!pushing && w < 0.002f) w = 0.f;
        } else {
            pp += pushing ? 1.f / (t_in * 30.f) : -1.f / (t_out * 30.f);
            if (pp < 0.f) pp = 0.f;
            if (pp > 1.f) pp = 1.f;
            w = push_ease(pp);
        }
    }
    if (pushing != was) { was = pushing;
        fprintf(stderr, "pushcam: %s%c", pushing ? "ON - she pushes: the look goes lower, the boom longer (DC_CAM_PUSH=0 off)" : "off", 10); }
    { static int dbg = -1;
      if (dbg < 0) { const char *e = getenv("DC_CAM_PUSH_DBG"); dbg = (e && *e == '1') ? 1 : 0; }
      if (dbg && cam_step && (w > 0.f || push_now)) {
          extern uint64_t gpu_flip_seq(void);
          fprintf(stderr, "pushdbg f=%llu mode=%d act=%d sub=%d now=%d after=%d w=%.3f add=%.0f%c", (unsigned long long)gpu_flip_seq(),
                  r[0xB92C8], r[0xB92C9], r[0xB92CA], push_now, after, w, v1 ? dadd : s_add, 10); } }

    static float s_prev_raw[3], s_raw_vel[3]; static int s_prev_ok = 0;
    if (cam_step) {
        for (int q = 0; q < 3; q++) { s_raw_vel[q] = s_prev_ok ? (tgt[q] - s_prev_raw[q]) * 30.f : 0.f; s_prev_raw[q] = tgt[q]; }
        s_prev_ok = 1;
    }
    if (w <= 0.f) { sp_ok = 0; s_add = 0.f; return 0.f; }
    if (!v1) {

        if (!sp_ok) { for (int q = 0; q < 3; q++) { sp[q] = tgt[q]; sv[q] = s_raw_vel[q]; } sp_ok = 1; }
        if (cam_step) for (int q = 0; q < 3; q++) push_spring(&sp[q], &sv[q], tgt[q], smooth, 1.f / 30.f);

        const float ws = pushing ? 1.f : w;
        for (int q = 0; q < 3; q++) tgt[q] += (sp[q] - tgt[q]) * ws;
    }
    tgt[1] += drop * w;
    if (v1) *dist_live += dadd * w;
    else {

        if (cam_step) {
            float pf = *pitch_io + padd;
            if (pf > 1.35f) pf = 1.35f;
            const float D = *dist_live + dadd;
            const float fr = gfp_clear(tgt, s_nots_oyaw, pf, D, 1);
            float want = fr * D - 40.f - *dist_live;
            if (want < 0.f) want = 0.f;
            if (want > dadd) want = dadd;
            s_add += (want - s_add) * (want < s_add ? 0.6f : 0.12f);
        }
        *dist_live += s_add * w;
    }
    if (padd != 0.f) {
        float pb = *pitch_io + padd * w;
        if (pb > 1.35f) pb = 1.35f;
        *pitch_io = pb;
        const float yw = s_nots_oyaw, cpb = cosf(pb), spb = sinf(pb);
        dir[0] = sinf(yw) * cpb; dir[1] = spb; dir[2] = cosf(yw) * cpb;
    }
    return w;
}
static float grabfocus_step(const uint8_t *r, int cam_step, float px, float py, float pz,
                            float yaw, float tgt[3], float dir[3], float *dist_live, float *pitch_io) {
    static int en = -1;

    static float hdown = 1000.f, pmin = 0.55f, dmax = 3600.f, rdino = 2600.f, marg = 0.78f;

    static float t_in = 0.5f, t_out = 1.2f, k_fol = 0.08f, k_zup = 0.10f, k_zdn = 0.04f;
    static int yaw_en = 1; static float yaw_rate = 140.f, yaw_max = 120.f;
    static float ks_in = 0.36f, pk_in = 0.19f, yaw_k = 0.30f; static int yaw_eval = 4;

    static int yaw_pause = 22;
    if (en < 0) {
        const char *e2;
        e2 = getenv("DC_CAM_GRABFOCUS_YAW_PAUSE"); if (e2) yaw_pause = atoi(e2);
        if (yaw_pause < 0) yaw_pause = 0;
        e2 = getenv("DC_CAM_GRABFOCUS_SMOOTH_IN"); if (e2) ks_in = (float)atof(e2);
        e2 = getenv("DC_CAM_GRABFOCUS_PITCH_K");   if (e2) pk_in = (float)atof(e2);
        e2 = getenv("DC_CAM_GRABFOCUS_YAW_K");     if (e2) yaw_k = (float)atof(e2);
        e2 = getenv("DC_CAM_GRABFOCUS_YAW_EVAL");  if (e2) yaw_eval = atoi(e2);
        if (ks_in <= 0.f || ks_in > 1.f) ks_in = 1.f;
        if (pk_in <= 0.f || pk_in > 1.f) pk_in = 1.f;
        if (yaw_k <= 0.f || yaw_k > 1.f) yaw_k = 1.f;
        if (yaw_eval < 1) yaw_eval = 1;
    }
    if (en < 0) {
        const char *e = getenv("DC_CAM_GRABFOCUS"); en = (e && *e == '0') ? 0 : 1;
        e = getenv("DC_CAM_GRABFOCUS_DOWN");   if (e) hdown = (float)atof(e);
        e = getenv("DC_CAM_GRABFOCUS_PITCH");  if (e) pmin  = (float)atof(e);
        e = getenv("DC_CAM_GRABFOCUS_MAX");    if (e) dmax  = (float)atof(e);
        e = getenv("DC_CAM_GRABFOCUS_R");      if (e) rdino = (float)atof(e);
        e = getenv("DC_CAM_GRABFOCUS_MARGIN"); if (e) marg  = (float)atof(e);
        e = getenv("DC_CAM_GRABFOCUS_IN");     if (e) t_in  = (float)atof(e);
        e = getenv("DC_CAM_GRABFOCUS_OUT");    if (e) t_out = (float)atof(e);
        e = getenv("DC_CAM_GRABFOCUS_FOLLOW"); if (e) k_fol = (float)atof(e);
        e = getenv("DC_CAM_GRABFOCUS_ZOOM_UP");   if (e) k_zup = (float)atof(e);
        e = getenv("DC_CAM_GRABFOCUS_ZOOM_DOWN"); if (e) k_zdn = (float)atof(e);
        e = getenv("DC_CAM_GRABFOCUS_YAW");       if (e) yaw_en = (*e == '0') ? 0 : 1;
        e = getenv("DC_CAM_GRABFOCUS_YAW_RATE");  if (e) yaw_rate = (float)atof(e);
        e = getenv("DC_CAM_GRABFOCUS_YAW_MAX");   if (e) yaw_max  = (float)atof(e);
        if (marg < 0.3f) marg = 0.3f; if (marg > 1.f) marg = 1.f;
        if (t_in < 0.05f) t_in = 0.05f; if (t_out < 0.05f) t_out = 0.05f;
    }
    if (!en) return 0.f;
    #define GF_S16(a) ((float)(int16_t)(r[(a)] | (r[(a)+1] << 8)))
    #define GF_S32(a) ((float)(int32_t)((uint32_t)r[(a)] | ((uint32_t)r[(a)+1] << 8) | ((uint32_t)r[(a)+2] << 16) | ((uint32_t)r[(a)+3] << 24)))
    extern int dc_cut_camera(void);
    const int scene = dc_cut_camera() || r[0xB92C8] == 4;
    int link = (r[0xB93DE] || r[0xB93DF]) ? 1 : 0;
    { static int was = 0; const int smv = link && dc_cam_scriptmove_now(r);
      if (smv && !was) fprintf(stderr, "grabfocus: scripted motion (0xB93DE=2, mode %d) - not a grab: no grab framing (DC_CAM_SCRIPTMOVE_ANY=0 off)%c",
                               r[0xB92C8], 10);
      was = smv;
      if (smv) link = 0; }
    const int react = (r[0xB93AA] != 0);
    float pts[20][3]; float prad[20]; int np = 0; float hmax = -1e9f;
    for (int f = 0; f < 15; f++) {
        uint32_t a = 0xB9500u + (uint32_t)f * 172u;
        float bx = GF_S32(a), by = GF_S32(a + 4u), bz = GF_S32(a + 8u);
        if (fabsf(bx - px) > 2500.f || fabsf(bz - pz) > 2500.f || fabsf(by - py) > 3000.f) continue;
        pts[np][0] = bx; pts[np][1] = by; pts[np][2] = bz; prad[np] = 200.f; np++;
        float h = py - by;
        if (h > hmax) hmax = h;
    }
    const int np_reg = np;
    const int down = react && np >= 6 && hmax < hdown;
    static int on = 0;
    if (scene) on = 0;
    else if (!on && (link || down)) on = 1;
    else if (on && !link && !react) on = 0;

    int dk = -1; float dp[3] = { 0.f, 0.f, 0.f };
    if (on) {
        uint32_t ap = ((uint32_t)r[0xB93E0] | ((uint32_t)r[0xB93E1] << 8) | ((uint32_t)r[0xB93E2] << 16) | ((uint32_t)r[0xB93E3] << 24)) & 0x1FFFFFu;
        if (link && ap >= 0xB7ACCu && ap < 0xB7ACCu + 10u * 608u && (ap - 0xB7ACCu) % 608u == 0u) {
            dk = (int)((ap - 0xB7ACCu) / 608u);
            dp[0] = GF_S16(ap + 32u); dp[1] = GF_S16(ap + 34u); dp[2] = GF_S16(ap + 36u);
        } else {

            static int dk_prev = -1;
            float best = rdino * rdino;
            for (int i = 0; i < 10; i++) {
                uint32_t b = 0xB7ACCu + (uint32_t)i * 608u;
                if ((int16_t)(r[b + 280u] | (r[b + 281u] << 8)) <= 0) continue;
                float ax = GF_S16(b + 32u), ay = GF_S16(b + 34u), az = GF_S16(b + 36u);
                if (ax == 0.f && az == 0.f) continue;
                float d2 = (ax - px) * (ax - px) + (az - pz) * (az - pz);
                float lim = (i == dk_prev) ? 1.69f * rdino * rdino : best;
                if (d2 < lim && (dk < 0 || d2 < best)) { best = d2 < best ? d2 : best; dk = i; dp[0] = ax; dp[1] = ay; dp[2] = az; }
            }
            dk_prev = dk;
        }
        if (dk >= 0 && np < 17) {
            for (int q = 0; q < 3; q++) {
                pts[np][0] = dp[0]; pts[np][1] = dp[1] - 150.f - 500.f * (float)q; pts[np][2] = dp[2];
                prad[np] = 520.f; np++;
            }
        }
    }

    { static int plan_en = -1;
      if (plan_en < 0) { const char *e = getenv("DC_CAM_GRABPLAN"); plan_en = (e && *e == '0') ? 0 : 1;
                         fprintf(stderr, "grabplan: %s%c", plan_en ? "on - the best view is planned once and reached on a curve (DC_CAM_GRABPLAN=0 - the old follow)" : "off (DC_CAM_GRABPLAN=0)", 10); }
      if (plan_en) {
          float Fo[3] = { tgt[0], tgt[1], tgt[2] }, Do = *dist_live, pfo = *pitch_io;
          const float w = gfplan_update(cam_step, on, link, down, (const float (*)[3])pts, np_reg, dk, dp, px, py, pz,
                                        *pitch_io, *dist_live, tgt, Fo, &Do, &pfo, dc_actor_is_trex(r, dk));

          { static float otgt[3]; static int ohave = 0, osm = 0;
            float want[3];
            for (int q = 0; q < 3; q++) want[q] = tgt[q] + (Fo[q] - tgt[q]) * w;
            if (on || w > 0.f || react || link) osm = 1;
            if (osm && ohave) {
                const float kk = (w > 0.f) ? 0.5f : ks_in;
                if (cam_step) for (int q = 0; q < 3; q++) otgt[q] += (want[q] - otgt[q]) * kk;
                const float err = fabsf(want[0] - otgt[0]) + fabsf(want[1] - otgt[1]) + fabsf(want[2] - otgt[2]);
                if (!on && w <= 0.f && err < 2.f) osm = 0;
                for (int q = 0; q < 3; q++) tgt[q] = otgt[q];
            } else {
                for (int q = 0; q < 3; q++) { tgt[q] = want[q]; otgt[q] = want[q]; }
                ohave = 1;
            }
            if (!osm) for (int q = 0; q < 3; q++) otgt[q] = tgt[q]; }
          for (int q = 0; q < 3; q++) s_gf_lt[q] = tgt[q];
          s_gf_lhave = 1;
          if (w <= 0.f) { s_gf_lD = *dist_live; s_gf_lp = *pitch_io; return 0.f; }
          { const float dn = *dist_live * s_gfp_effn; *dist_live = dn + (Do - dn) * w; }
          float pb = *pitch_io + (pfo - *pitch_io) * w;
          if (pb > 1.35f) pb = 1.35f;
          *pitch_io = pb;
          const float yw = s_nots_oyaw, cpb = cosf(pb), spb = sinf(pb);
          dir[0] = sinf(yw) * cpb; dir[1] = spb; dir[2] = cosf(yw) * cpb;
          s_gf_lD = *dist_live; s_gf_lp = pb;
          return w;
      } }

    static float poff = 0.f, poff_cur = 0.f; static int was_on = 0;
    if (on && !was_on) poff = (pmin > 0.f && *pitch_io < pmin) ? pmin - *pitch_io : 0.f;
    was_on = on;
    if (cam_step) { const float pt_ = (on && link) ? poff : 0.f;
                    poff_cur += (pt_ - poff_cur) * ((pt_ > poff_cur) ? pk_in : 0.10f); }
    float pf = *pitch_io + poff_cur;
    if (pf > 1.35f) pf = 1.35f;
    float cy = cosf(yaw), sy = sinf(yaw), cpf = cosf(pf), spf = sinf(pf);
    float fd[3] = { sy * cpf, spf, cy * cpf };
    float rt[3] = { cy, 0.f, -sy };
    float up[3] = { fd[1] * rt[2] - fd[2] * rt[1], fd[2] * rt[0] - fd[0] * rt[2], fd[0] * rt[1] - fd[1] * rt[0] };
    float F[3] = { tgt[0], tgt[1], tgt[2] }, D = *dist_live;
    if (on && np > 0) {
        float lo[3] = { 1e9f, 1e9f, 1e9f }, hi[3] = { -1e9f, -1e9f, -1e9f };
        for (int i = 0; i < np; i++) for (int q = 0; q < 3; q++) {
            if (pts[i][q] < lo[q]) lo[q] = pts[i][q];
            if (pts[i][q] > hi[q]) hi[q] = pts[i][q];
        }
        for (int q = 0; q < 3; q++) F[q] = 0.5f * (lo[q] + hi[q]);
        const float th = 0.5f * marg, tv = 0.375f * marg;
        float need = 0.f;
        for (int i = 0; i < np; i++) {
            float v[3] = { pts[i][0] - F[0], pts[i][1] - F[1], pts[i][2] - F[2] };
            float x = fabsf(v[0] * rt[0] + v[2] * rt[2]) + prad[i];
            float y = fabsf(v[0] * up[0] + v[1] * up[1] + v[2] * up[2]) + prad[i];
            float z = v[0] * fd[0] + v[1] * fd[1] + v[2] * fd[2];
            float dh = x / th - z, dv = y / tv - z;
            if (dh > need) need = dh;
            if (dv > need) need = dv;
        }
        D = need;
        if (D < *dist_live) D = *dist_live;
        if (D > dmax) D = dmax;
    }

    static float wl = 0.f; static float Fs[3] = { 0.f, 0.f, 0.f }, Ds = 0.f;

    if (cam_step) {
        if (on && wl <= 0.f) { Fs[0] = F[0]; Fs[1] = F[1]; Fs[2] = F[2]; Ds = D; }
        const float kin  = 1.f - powf(0.05f, 1.f / (t_in  * 30.f));
        const float kout = 1.f - powf(0.05f, 1.f / (t_out * 30.f));
        wl += ((on ? 1.f : 0.f) - wl) * (on ? kin : kout);
        if (!on && wl < 0.003f) wl = 0.f;
        if (on && wl > 0.997f) wl = 1.f;
        if (on) { for (int q = 0; q < 3; q++) Fs[q] += (F[q] - Fs[q]) * k_fol;

                  static float zvmax = -1.f;
                  if (zvmax < 0.f) { const char *e = getenv("DC_CAM_GRABFOCUS_ZOOM_VMAX"); zvmax = e ? (float)atof(e) : 60.f; }
                  float dz = (D - Ds) * ((D > Ds) ? k_zup : k_zdn);
                  if (zvmax > 0.f) { if (dz > zvmax) dz = zvmax; if (dz < -zvmax * 0.67f) dz = -zvmax * 0.67f; }
                  Ds += dz; }
    }
    float w = wl;

    { static float ytgt = 0.f, ylast = 1e9f; static int yhave = 0, ypause = 0, yeval = 0;
      static float tb_y[4], tb_c[4]; static int tb_t[4] = { 0, 0, 0, 0 }, tb_i = 0;
      const float PI = 3.14159265f, step = 15.f * PI / 180.f, ymaxr = yaw_max * PI / 180.f;
      #define GF_WRAP(a) ((a) - 2.f * PI * floorf(((a) + PI) / (2.f * PI)))
      if (!on || !yaw_en || s_cammesh_n <= 0 || wl < 0.5f) { yhave = 0; yeval = 0; if (!on) memset(tb_t, 0, sizeof tb_t); }
      else {
          float cur = s_nots_oyaw;
          if (yhave && fabsf(GF_WRAP(cur - ylast)) > 1e-4f) {
              ypause = yaw_pause; yhave = 0;
              fprintf(stderr, "grabfocus: yaw - the player turned the camera, auto-turn paused%c", 10);
          }
          if (cam_step) {
              if (ypause > 0) ypause--;
              else {

                  const float *lt = s_gf_lhave ? s_gf_lt : Fs;
                  const float lD = s_gf_lhave ? s_gf_lD : Ds, lpt = s_gf_lhave ? s_gf_lp : pf;
                  float cpf2 = cosf(lpt), spf2 = sinf(lpt);
                  #define GF_CLEAR(yv, out) do { float c_ = 1.f; \
                      for (int wq_ = -1; wq_ <= 1; wq_++) { float ya_ = (yv) + 0.384f * (float)wq_; \
                          float dd_[3] = { sinf(ya_) * cpf2, spf2, cosf(ya_) * cpf2 }; \
                          float ee_[3] = { lt[0] - dd_[0] * lD, lt[1] - dd_[1] * lD, lt[2] - dd_[2] * lD }; \
                          float v_[3] = { ee_[0] - lt[0], ee_[1] - lt[1], ee_[2] - lt[2] }; \
                          float t_ = wq_ ? cammesh_ray_t(s_cammesh, s_cammesh_n, lt, v_) \
                                         : cammesh_hit_t(s_cammesh, s_cammesh_n, lt, ee_); \
                          if (t_ < c_) c_ = t_; } \
                      (out) = c_; } while (0)

                  for (int q = 0; q < 4; q++) if (tb_t[q] > 0) tb_t[q]--;
                  #define GF_TABU(yv, c) do { for (int q_ = 0; q_ < 4; q_++) \
                      if (tb_t[q_] > 0 && fabsf(GF_WRAP((yv) - tb_y[q_])) < 0.18f && tb_c[q_] < (c)) (c) = tb_c[q_]; } while (0)
                  if (!yhave || ++yeval >= yaw_eval) {
                      yeval = 0;
                      const float pnow = s_cam_probe_pub + 0.03f;
                      float c0; GF_CLEAR(cur, c0);
                      if (c0 > pnow + 0.30f) {
                          tb_y[tb_i] = cur; tb_c[tb_i] = pnow; tb_t[tb_i] = 90; tb_i = (tb_i + 1) % 4;
                          fprintf(stderr, "grabfocus: yaw - forecast %.2f but the boom is held at %.2f here: course remembered for 3 s%c", c0, pnow, 10);
                      }
                      if (pnow < c0) c0 = pnow;
                      float best_s = c0, best_y = cur;
                      for (int k = -8; k <= 8; k++) {
                          if (!k || fabsf((float)k * step) > ymaxr + 1e-3f) continue;
                          float yv = cur + (float)k * step, c; GF_CLEAR(yv, c); GF_TABU(yv, c);
                          float sc = c - 0.12f * fabsf((float)k * step) / PI;
                          if (sc > best_s) { best_s = sc; best_y = yv; }
                      }
                      if (!yhave) { ytgt = (c0 >= 0.97f) ? cur : best_y; yhave = 1;
                          fprintf(stderr, "grabfocus: yaw - boom free %.2f here, target %+.0f deg%c", c0, GF_WRAP(ytgt - cur) * 180.f / PI, 10); }
                      else {
                          float ct; GF_CLEAR(ytgt, ct); GF_TABU(ytgt, ct);
                          if (fabsf(GF_WRAP(ytgt - cur)) < 0.05f && pnow < ct) ct = pnow;
                          float st = ct - 0.12f * fabsf(GF_WRAP(ytgt - cur)) / PI;
                          if (best_s > st + 0.12f) { ytgt = best_y;
                              fprintf(stderr, "grabfocus: yaw - better view %+.0f deg (free %.2f -> score %.2f)%c", GF_WRAP(ytgt - cur) * 180.f / PI, ct, best_s, 10); }
                      }
                  }
                  #undef GF_TABU
                  #undef GF_CLEAR
                  float dy = GF_WRAP(ytgt - cur), mv = dy * yaw_k, lim = yaw_rate * PI / 180.f / 30.f;
                  if (mv > lim) mv = lim; if (mv < -lim) mv = -lim;
                  if (fabsf(dy) < 0.002f) mv = dy;
                  s_nots_oyaw = cur + mv;
              }
          }
          ylast = s_nots_oyaw;
      }
      #undef GF_WRAP
      yaw = s_nots_oyaw;
      cy = cosf(yaw); sy = sinf(yaw); }
    { static int pon = 0; static long nlog = 0; static unsigned long long nf = 0;
      if (on != pon && nlog < 200) { nlog++;
          fprintf(stderr, "grabfocus: %s (link=%d react=%02X lying=%d height %.0f, bones %d, dino %d at %.0f,%.0f,%.0f; boom %.0f -> %.0f)%c",
                  on ? "ON" : "off", link, (unsigned)r[0xB93AA], down, hmax, np, dk, dp[0], dp[1], dp[2], *dist_live, D, 10); }
      pon = on;
      if (on && cam_step && (++nf % 15) == 0 && nlog < 200) { nlog++;
          fprintf(stderr, "grabfocus: w=%.2f centre %.0f,%.0f,%.0f boom %.0f (fit %.0f) pitch %.2f height %.0f dino %d%c",
                  w, Fs[0], Fs[1], Fs[2], Ds, D, pf, hmax, dk, 10); } }

    { static float otgt[3]; static int ohave = 0, osm = 0; static float ks = -1.f;
      if (ks < 0.f) { const char *e = getenv("DC_CAM_GRABFOCUS_SMOOTH"); ks = e ? (float)atof(e) : 0.20f;
                      if (ks <= 0.f || ks > 1.f) ks = 1.f; }
      float want[3];
      for (int q = 0; q < 3; q++) want[q] = tgt[q] + (Fs[q] - tgt[q]) * w;

      if (on || w > 0.f || react || link) osm = 1;
      if (osm && ohave) {
          const float kk = (on || react || link) ? ks_in : ks;
          if (cam_step) for (int q = 0; q < 3; q++) otgt[q] += (want[q] - otgt[q]) * kk;
          float err = fabsf(want[0] - otgt[0]) + fabsf(want[1] - otgt[1]) + fabsf(want[2] - otgt[2]);
          if (!on && w <= 0.f && err < 2.f) osm = 0;
          for (int q = 0; q < 3; q++) tgt[q] = otgt[q];
      } else {
          for (int q = 0; q < 3; q++) { tgt[q] = want[q]; otgt[q] = want[q]; }
          ohave = 1;
      }
      if (!osm) { for (int q = 0; q < 3; q++) otgt[q] = tgt[q]; } }
    for (int q = 0; q < 3; q++) s_gf_lt[q] = tgt[q];
    s_gf_lhave = 1;
    if (w <= 0.f) { s_gf_lD = *dist_live; s_gf_lp = *pitch_io; return 0.f; }
    *dist_live += (Ds - *dist_live) * w;
    float pb = *pitch_io + (pf - *pitch_io) * w;
    *pitch_io = pb;
    float cpb = cosf(pb), spb = sinf(pb);
    dir[0] = sy * cpb; dir[1] = spb; dir[2] = cy * cpb;
    s_gf_lD = *dist_live; s_gf_lp = pb;
    #undef GF_S16
    #undef GF_S32
    return w;
}

void dc_geomcam_probe(CPUState* cpu) {
    g_dc_pass_id++;
    if (g_dc_passdbg < 0) { const char *e = getenv("DC_PASSDBG"); g_dc_passdbg = (e && *e == '1') ? 1 : 0; }
    if (g_dc_passdbg) {
        extern uint64_t s_frame_count; extern uint64_t gpu_flip_seq(void);
        fprintf(stderr, "passdbg: hook pass=%d flip=%llu frame=%llu\n", g_dc_pass_id,
                (unsigned long long)gpu_flip_seq(), (unsigned long long)s_frame_count);
    }
    { void dc_coll_refresh(CPUState*); dc_coll_refresh(cpu); }
    dc_camcoll_dump(cpu);

    { static int as=-1;
      if(as<0){ const char*e=getenv("DC_NOTS_ALLSECT");
                if (e) as=(*e=='1')?1:0;
                else { const char*n=getenv("DC_NATIVE_OTS"); as=(n&&*n=='1')?1:0; } }

      if (as && !dc_orig_native_draw()) {
          uint32_t ctx = cpu->read_word(0x1F800000u) & 0x1FFFFFu;
          if (ctx && ctx + 7324u < RAM_SIZE) {
              extern int dc_cut_active(void);
              extern int g_dc_ff_window;
              static uint8_t s_as_hold[11];
              static uint32_t s_as_ctx = 0;
              static int s_as_wascut = 0;
              if (ctx != s_as_ctx) { s_as_ctx = ctx; memset(s_as_hold, 0, sizeof s_as_hold); }
              int cut = dc_cut_active();

              if (s_as_wascut && !cut) memset(s_as_hold, 0, sizeof s_as_hold);
              s_as_wascut = cut;
              int mark = cut && !g_dc_ff_window;
              int seen = 0;
              for (int k = 0; k < 11; k++) {
                  uint32_t fa = ctx + 712u + 608u*(uint32_t)k - 28u;
                  if (ram[fa] & 2u)       { s_as_hold[k] = 0; seen = 1; }
                  else if (!(ram[fa] & 1u)) s_as_hold[k] = 0;
                  else if (mark)            s_as_hold[k] = 1;
              }
              if (seen)
                  for (int k = 0; k < 11; k++) {
                      if (s_as_hold[k]) continue;
                      ram[ctx + 712u + 608u*(uint32_t)k - 28] |= 2u;
                  }

              { static uint32_t lctx=0; static int lseen=-1;
                if (ctx != lctx || seen != lseen) { lctx=ctx; lseen=seen;
                    fprintf(stderr, "allsect: ctx=%05X seen=%d\n", ctx, seen); } }
          }
      } }

    { static int en = -1, ep = 0, win = 0; extern int g_savestate_loads;
      if (en < 0) { const char *e = getenv("DC_ITEM_HIDE_SAVE"); en = (e && *e == '0') ? 0 : 1; }
      if (ep != g_savestate_loads) { ep = g_savestate_loads; win = 30; }
      if (en && win > 0) {
          win--;
          const uint32_t gs = cpu->read_word(0x1F800000u) & 0x1FFFFFu;
          const uint32_t fb = ((uint32_t)ram[0x9CB58] | ((uint32_t)ram[0x9CB59] << 8) | ((uint32_t)ram[0x9CB5A] << 16) |
                               ((uint32_t)ram[0x9CB5B] << 24)) & 0x1FFFFFu;
          if (gs >= 0x1000u && gs + 0x8FFCu < RAM_SIZE && fb >= 0x1000u && fb + 4u < RAM_SIZE && ram[0xB92C8] != 4) {
              const int flag = (ram[fb + 3u] >> 3) & 1;
              int npush = 0, ndone = 0;
              for (int k = 0; k < 36; k++) {
                  const uint32_t e = gs + 0x7CDCu + 136u * (uint32_t)k;
                  if (!(ram[e + 48u] & 1u) || (int8_t)ram[e + 46u] != 1) continue;
                  npush++; if (ram[e + 116u] & 1u) ndone++;
              }
              if (!flag && npush > 0 && ndone == 0)
                  for (int k = 0; k < 36; k++) {
                      const uint32_t e = gs + 0x7CDCu + 136u * (uint32_t)k;
                      if ((int8_t)ram[e + 46u] != 2 || !ram[e + 47u] || (ram[e + 48u] & 3u) != 3u) continue;
                      ram[e + 48u] &= (uint8_t)~2u;
                      fprintf(stderr, "item hide: slot %d (marked item, model %02X%02X%02X) was lit in the snapshot by an earlier session of ours - "
                                      "the game shows it only when a push ends (flag 2/27; pushables here %d, none pushed) - hidden again "
                                      "(DC_ITEM_HIDE_SAVE=0 off)%c", k, ram[e + 62u], ram[e + 61u], ram[e + 60u], npush, 10);
                  }
          }
      } }

    { static int sh = -1, win = 0, ep = -1, was4 = 0;
      extern int g_savestate_loads;
      if (sh < 0) { const char *e = getenv("DC_ST402_CRATE_SHIFT"); sh = e ? atoi(e) : 500; if (sh < 0) sh = 0; if (sh > 800) sh = 800; }
      if (ep != g_savestate_loads) { ep = g_savestate_loads; win = 30; }
      if (ram[0xB92C8] == 4) was4 = 1; else if (was4) { was4 = 0; win = 30; }
      if (win > 0) {
          win--;
          const uint32_t gs = cpu->read_word(0x1F800000u) & 0x1FFFFFu;
          if (sh > 0 && ram[0xC12F0] == 2 && ram[0xC12F1] == 4 && ram[0xB92C8] != 4 && !dc_orig_pure() &&
              gs >= 0x1000u && gs + 0x8FFCu < RAM_SIZE) {
              static const int16_t org[2][3] = { { 500, 0, 8000 }, { 1500, 0, 6000 } };
              const int rx = (int16_t)(ram[0xB92AC] | (ram[0xB92AD] << 8)), rz = (int16_t)(ram[0xB92B0] | (ram[0xB92B1] << 8));
              for (int k = 0; k < 2; k++) {
                  const uint32_t e = gs + 0x7CDCu + 136u * (uint32_t)k;
                  const uint32_t grp = ((uint32_t)ram[e + 60u] | ((uint32_t)ram[e + 61u] << 8) | ((uint32_t)ram[e + 62u] << 16)) & 0x1FFFFFu;
                  const int px = (int16_t)(ram[e + 32u] | (ram[e + 33u] << 8)), py = (int16_t)(ram[e + 34u] | (ram[e + 35u] << 8)),
                            pz = (int16_t)(ram[e + 36u] | (ram[e + 37u] << 8));
                  if (grp != 0x115A44u || (int8_t)ram[e + 46u] != 1 || !(ram[e + 48u] & 1u) || (ram[e + 116u] & 1u)) continue;
                  if (px != org[k][0] || py != org[k][1] || pz != org[k][2]) continue;
                  const int nx = px - sh;
                  if (abs(rx - nx) < 1500 && abs(rz - pz) < 1500) {
                      static int said = 0;
                      if (!said) { said = 1; fprintf(stderr, "st402 crates: slot %d left at x %d - Regina (%d,%d) stands next to its new place%c",
                                                     k, px, rx, rz, 10); }
                      continue;
                  }
                  ram[e + 32u] = (uint8_t)(nx & 0xFF); ram[e + 33u] = (uint8_t)((nx >> 8) & 0xFF);
                  fprintf(stderr, "st402 crates: slot %d starts at x %d instead of %d (gap at the east wall after the push %d instead "
                                  "of 500; DC_ST402_CRATE_SHIFT=0 off)%c", k, nx, px, 500 + sh, 10);
              }
          }
      } }

    {
        static int fr=-1;
        if (fr<0){ const char*e=getenv("DC_NOTS_FULLROOM");
                   if (e) fr=(*e=='1')?1:0;
                   else { const char*n=getenv("DC_NATIVE_OTS"); fr=(n&&*n=='1')?1:0; } }
        if (fr) {
            g_dc_union_claim_on = 1;
            s_svh_tick++;
            static uint8_t un[90]; static int uhave=0, ucount=0;
            static uint32_t ugrp[90];
            static int occ = -1;
            if (occ < 0) { const char *eo = getenv("DC_ROOM_UNION_OCC"); occ = (eo && *eo == '0') ? 0 : 1; }
            uint32_t ctx = cpu->read_word(0x1F800000u) & 0x1FFFFFu;

            { extern int g_savestate_loads; static int uep = 0;
              if (uep != g_savestate_loads) { uep = g_savestate_loads;
                  if (ucount) fprintf(stderr, "fullroom: reset (snapshot loaded, %d claims dropped)%c", ucount, 10);
                  memset(un, 0, sizeof(un)); memset(ugrp, 0, sizeof(ugrp)); uhave = 0; ucount = 0;
                  memset(s_svh_grp, 0, sizeof s_svh_grp); memset(s_svh_run, 0, sizeof s_svh_run); } }
            extern int dc_scene_stairs_keep(void);
            extern int dc_scene_ourcam_play(void);
            if (ram[0xB92C8] == 4 && !dc_scene_stairs_keep() && !dc_scene_ourcam_play()) {
                memset(s_svh_grp, 0, sizeof s_svh_grp); memset(s_svh_run, 0, sizeof s_svh_run);
                if (uhave) { memset(un, 0, sizeof(un)); memset(ugrp, 0, sizeof(ugrp)); uhave=0;
                             if (ucount) fprintf(stderr, "fullroom: reset (mode4)\n");
                             ucount=0; }
            } else if (ctx && ctx + 0x8FFCu < RAM_SIZE) {

                extern int dc_room_section_is_static(uint32_t gp);
                extern int dc_room_state_shows(uint32_t gp);
                extern int dc_room_script_anim(int k);
                extern int dc_room_version_off(int k, uint32_t gp);
                extern int dc_room_debris_slot(int k);
                static int seed = -1;

                if (seed < 0) { const char *e2 = getenv("DC_ROOM_SEED_LIB");
                                seed = (e2 && *e2 == '1') ? 1 : 0; }
                int nseed = 0;
                int changed = 0;

                for (int k = 0; k < 36; k++) {
                    uint32_t base = ctx + 0x7CDCu + 136u*(uint32_t)k;
                    uint32_t fa = base + 48u;

                    uint32_t gpc = 0;
                    if (base + 64u < RAM_SIZE)
                        gpc = ((uint32_t)ram[base+60] | ((uint32_t)ram[base+61]<<8) |
                               ((uint32_t)ram[base+62]<<16) | ((uint32_t)ram[base+63]<<24))
                              & 0x1FFFFFFFu;
                    if (occ && un[k] && ugrp[k] && gpc != ugrp[k]) {
                        un[k] = 0; ucount--;
                        fprintf(stderr, "fullroom: slot %d changed occupant %06X -> %06X, "
                                        "union claim dropped%c",
                                k, ugrp[k], gpc, 10);
                        ugrp[k] = 0;
                    }

                    static uint16_t uclear[64];
                    static int uhold = -1;
                    if (uhold < 0) { const char *eh = getenv("DC_UNION_HOLD");
                                     uhold = eh ? atoi(eh) : 20; if (uhold < 1) uhold = 1; }
                    static uint8_t uon[64]; static uint32_t uongrp[64];
                    static int uclaim = -1;
                    if (uclaim < 0) { const char *ec = getenv("DC_UNION_CLAIM");
                                      uclaim = ec ? atoi(ec) : 4; if (uclaim < 1) uclaim = 1; if (uclaim > 250) uclaim = 250; }
                    static int upush = -1;
                    if (upush < 0) { const char *ep = getenv("DC_UNION_PUSHABLE"); upush = (ep && *ep == '0') ? 0 : 1; }

                    int indicator = 0;
                    { static int minpoly = -1;
                      if (minpoly < 0) { const char *em = getenv("DC_SHOWALL_MINPOLY"); minpoly = em ? atoi(em) : 3; }
                      if (minpoly > 0 && gpc >= 0x100000u && gpc + 12u < RAM_SIZE) {
                          int np = (int)(ram[gpc+8] | (ram[gpc+9] << 8)) + (int)(ram[gpc+10] | (ram[gpc+11] << 8));
                          if (np < minpoly) indicator = 1;
                      } }

                    int state_hidden = (gpc >= 0x100000u && dc_room_state_shows(gpc) == 0) ? 1 : 0;

                    { extern int dc_room_slot_story_hidden(int k, uint32_t gp);
                      extern int g_savestate_loads; static int sl_ep = 0; static int sl_win = 0;
                      if (k == 0) { if (sl_ep != g_savestate_loads) { sl_ep = g_savestate_loads; sl_win = 120; } else if (sl_win > 0) sl_win--; }
                      if (gpc >= 0x100000u && dc_room_slot_story_hidden(k, gpc)) {
                          state_hidden = 1;
                          if (sl_win > 0 && (ram[fa] & 2u)) {
                              ram[fa] &= (uint8_t)~2u;
                              fprintf(stderr, "fullroom: slot %d grp %06X - hidden by the story; the visibility bit left in the snapshot by an "
                                              "earlier session of ours cleared%c", k, gpc, 10);
                          }
                      } }
                    if (state_hidden) {
                        if (un[k]) { un[k] = 0; ugrp[k] = 0; ucount--; changed = 1; }
                        if (k < 64) { uon[k] = 0; uclear[k] = 0; }
                        { static uint32_t lk = 0xFFFFFFFFu, lg = 0; static long nst = 0;
                          if ((lk != (uint32_t)k || lg != gpc) && ++nst < 200) { lk = (uint32_t)k; lg = gpc;
                              fprintf(stderr, "fullroom: slot %d grp %06X - hidden by the room script in every shot at the "
                                              "current flags (state variant), union claim refused%c", k, gpc, 10); } }
                    } else if (gpc >= 0x100000u && (dc_room_script_anim(k) || dc_room_version_off(k, gpc) || dc_room_debris_slot(k))) {

                        if (un[k]) { un[k] = 0; ugrp[k] = 0; ucount--; changed = 1;
                                     fprintf(stderr, "fullroom: slot %d grp %06X - %s, union claim dropped%c", k, gpc,
                                             dc_room_script_anim(k) ? "the script animates its visibility" : "a version of its place the current shot hides", 10); }
                        if (k < 64) { uon[k] = 0; uclear[k] = 0; }
                    } else if (indicator) {
                        if (un[k]) { un[k] = 0; ugrp[k] = 0; ucount--; changed = 1;
                                     fprintf(stderr, "fullroom: slot %d grp %06X - indicator variant (1-2 polys), union claim dropped%c", k, gpc, 10); }
                    } else if (union_handler_driven(k, gpc)) {

                        if (un[k]) { un[k] = 0; ugrp[k] = 0; ucount--; changed = 1;
                                     fprintf(stderr, "fullroom: slot %d grp %06X - handler-driven, union claim dropped%c", k, gpc, 10); }
                        if (k < 64) { uon[k] = 0; uclear[k] = 0; }
                    } else if (upush && !un[k] && k < 64 && !g_dc_orig_mode && gpc >= 0x100000u && (ram[fa] & 1u) && (int8_t)ram[base + 46u] == 1) {

                        un[k] = 1; ugrp[k] = gpc; ucount++; changed = 1; uon[k] = 0; uclear[k] = 0;
                        if (g_dc_fullroom_force) ram[fa] |= 2u;
                        { static long npc = 0;
                          if (++npc < 200)
                              fprintf(stderr, "fullroom: slot %d grp %06X - pushable object (type 1), claimed at once, drawn from any "
                                              "camera (DC_UNION_PUSHABLE=0 off)%c", k, gpc, 10); }
                    } else if (ram[fa] & 2u) {
                        if (k < 64) uclear[k] = 0;

                        if (!un[k]) {
                            if (k < 64) {
                                if (uongrp[k] != gpc) { uongrp[k] = gpc; uon[k] = 0; }
                                if (uon[k] < 255) uon[k]++;
                                if (uon[k] >= (uint8_t)uclaim) { un[k]=1; changed=1; ucount++; ugrp[k] = gpc; }
                            }
                        } else ugrp[k] = gpc;
                    } else if (!un[k] && k < 64 && uon[k] > 0) {
                        static long ntr = 0;
                        if (++ntr < 120)
                            fprintf(stderr, "fullroom: slot %d grp %06X transient ON run %d frames, not claimed%c",
                                    k, gpc, uon[k], 10);
                        uon[k] = 0;
                    } else if (un[k] && k < 64 && ++uclear[k] > (uint16_t)uhold) {
                        un[k] = 0; ugrp[k] = 0; ucount--; uclear[k] = 0;
                        fprintf(stderr, "fullroom: slot %d grp %06X held OFF %d frames, "
                                        "union claim released%c", k, gpc, uhold, 10);
                    } else if (un[k]) {

                        extern int g_dc_fullroom_force;
                        { static int fe = -1;
                          if (fe < 0) { const char *ef = getenv("DC_FULLROOM_FORCE"); fe = 1; if (ef && *ef == '0') g_dc_fullroom_force = 0; } }
                        if (g_dc_fullroom_force) {
                            ram[fa] |= 2u;
                            static long nre = 0;
                            static uint32_t lastk = 0xFFFFFFFFu, lastg = 0;
                            if ((k != (int)lastk || gpc != lastg) && ++nre < 200) {
                                lastk = (uint32_t)k; lastg = gpc;
                                fprintf(stderr, "fullroom: RE-ASSERT slot %d grp %06X was=%02X -> %02X (n=%ld)%c",
                                        k, gpc, ram[fa] & ~2u, ram[fa], nre, 10);
                            }
                        }
                    } else if (seed && base + 64u < RAM_SIZE) {
                        uint32_t gp = ((uint32_t)ram[base+60] | ((uint32_t)ram[base+61]<<8) |
                                       ((uint32_t)ram[base+62]<<16) | ((uint32_t)ram[base+63]<<24))
                                      & 0x1FFFFFFFu;
                        if (gp >= 0x100000u && gp < 0x200000u &&
                            dc_room_section_is_static(gp)) {
                            un[k] = 1; ram[fa] |= 2u; changed = 1; ucount++; nseed++;
                        }
                    }

                    if (k < 90) { g_dc_union_claim[k] = un[k]; g_dc_union_claim_grp[k] = ugrp[k]; }
                }
                uhave = 1;
                g_dc_union_claim_ready = 1;
                if (changed) fprintf(stderr, "fullroom: union=%d entries (%d seeded from library)%c", ucount, nseed, 10);
            }
        }
    }

    static int nen=-1;
    if (nen<0){ const char*e=getenv("DC_NATIVE_OTS");    nen=(e&&*e=='1')?1:0; }
    if (!nen) return;
    uint32_t mvirt = cpu->gpr[2];
    uint32_t mphys = mvirt & 0x1FFFFFu;
    uint32_t* c = cpu->gte_ctrl;

    if (nen) {

        static int ld=0;
        if(!ld){ ld=1; const char*e;
            if((e=getenv("DC_NOTS_DIST")))     s_nd_dist=(float)atof(e);
            if((e=getenv("DC_NOTS_PITCH")))    s_nots_opitch=(float)atof(e);

            if((e=getenv("DC_NOTS_SHOULDER"))) s_nd_shoulder=(float)atof(e);

            else if (nots_true_shoulder())     s_nd_shoulder=300.f;
            if((e=getenv("DC_NOTS_TGTUP")))    s_nd_tgtup=(float)atof(e);
            if((e=getenv("DC_NOTS_HIP_DROP"))) s_nd_hip_drop=(float)atof(e);
            if((e=getenv("DC_CAM_HEIGHT")))    g_opt_cam_height=atoi(e) ? 1 : 0; }

        { extern int dc_strafe_hold_pos(int *x, int *z); int hx = 0, hz = 0;
          if (dc_strafe_hold_pos(&hx, &hz)) {
              const int cx0 = (int16_t)(ram[0xB92AC]|(ram[0xB92AD]<<8)), cz0 = (int16_t)(ram[0xB92B0]|(ram[0xB92B1]<<8));
              const int ddx = hx - cx0, ddz = hz - cz0;
              if ((ddx || ddz) && ddx > -400 && ddx < 400 && ddz > -400 && ddz < 400) {
                  static const uint32_t ax[3] = { 0xB92ACu, 0xB92C0u, 0xB93C8u }, az[3] = { 0xB92B0u, 0xB92C4u, 0xB93CAu };
                  for (int i = 0; i < 3; i++) {
                      int v = (int16_t)(ram[ax[i]]|(ram[ax[i]+1]<<8)) + ddx; ram[ax[i]] = (uint8_t)(v & 0xFF); ram[ax[i]+1] = (uint8_t)((v >> 8) & 0xFF);
                      v = (int16_t)(ram[az[i]]|(ram[az[i]+1]<<8)) + ddz;     ram[az[i]] = (uint8_t)(v & 0xFF); ram[az[i]+1] = (uint8_t)((v >> 8) & 0xFF);
                  }
              } } }
        float px=(float)(int16_t)(ram[0xB92C0]|(ram[0xB92C1]<<8));

        float py;
        { static int cy = -1;
          if (cy < 0) { const char *e = getenv("DC_CAM_Y"); cy = (e && *e == '0') ? 0 : 1; }
          py = cy ? (float)(int16_t)(ram[0xB92C2]|(ram[0xB92C3]<<8))
                  : (float)(int16_t)(ram[0xB92AA]|(ram[0xB92AB]<<8)); }
        float pz=(float)(int16_t)(ram[0xB92B0]|(ram[0xB92B1]<<8));

        { static int cp = -1;
          if (cp < 0) { const char *e = getenv("DC_CAM_PIVOT"); cp = (e && *e == '0') ? 0 : 1; }
          if (cp) {
              float cx = (float)(int16_t)(ram[0xB93C8]|(ram[0xB93C9]<<8));
              float cz = (float)(int16_t)(ram[0xB93CA]|(ram[0xB93CB]<<8));
              if (fabsf(cx - px) < 2000.f && fabsf(cz - pz) < 2000.f) {

                  { extern int dc_strafe_body_yaw(int *yaw); static int sb = -1; int by = 0;
                    if (sb < 0) { const char *e = getenv("DC_CAM_STRAFE_BODY"); sb = (e && *e == '0') ? 0 : 1; }
                    if (sb && dc_strafe_body_yaw(&by)) {
                        const float bx = (float)(int16_t)(ram[0xB92AC]|(ram[0xB92AD]<<8)), bz = (float)(int16_t)(ram[0xB92B0]|(ram[0xB92B1]<<8));
                        const int lyaw = (int16_t)(ram[0xB92B6]|(ram[0xB92B7]<<8)) & 4095;
                        const float ox = cx - bx, oz = cz - bz;
                        const float al = (float)lyaw * (6.2831853f / 4096.f), ab = (float)by * (6.2831853f / 4096.f);
                        const float fw = ox * sinf(al) + oz * cosf(al), rt = ox * cosf(al) - oz * sinf(al);
                        cx = bx + fw * sinf(ab) + rt * cosf(ab); cz = bz + fw * cosf(ab) - rt * sinf(ab);
                    } }
                  px = cx; pz = cz;
              }
          } }

        { extern int dc_aim_walk_mode(void); extern uint64_t gpu_flip_seq(void);
          static float om = -1.f; static float sox = 0.f, soz = 0.f, svx = 0.f, svz = 0.f; static int sinit = 0; static unsigned long long sfl = ~0ull;
          if (om < 0.f) { const char *e = getenv("DC_CAM_STRAFE_OFFSET_OMEGA"); om = e ? (float)atof(e) : 14.f; }
          if (om > 0.f && (dc_aim_walk_mode() == 1 || dc_aim_walk_mode() == 2 || dc_aim_walk_mode() == 3) && ram[0xB92C8] == 1) {
              const float bx = (float)(int16_t)(ram[0xB92AC]|(ram[0xB92AD]<<8)), bz = (float)(int16_t)(ram[0xB92B0]|(ram[0xB92B1]<<8));
              const float ox = px - bx, oz = pz - bz;
              const unsigned long long fq = (unsigned long long)gpu_flip_seq();
              if (!sinit || fabsf(ox - sox) + fabsf(oz - soz) > 400.f) { sox = ox; soz = oz; svx = svz = 0.f; sinit = 1; sfl = fq; }
              else if (fq != sfl) {
                  sfl = fq;
                  const float dt = 1.f / 30.f, ex = expf(-om * dt);
                  float e = sox - ox, c = svx + om * e; sox = ox + (e + c * dt) * ex; svx = (svx - om * c * dt) * ex;
                  e = soz - oz; c = svz + om * e; soz = oz + (e + c * dt) * ex; svz = (svz - om * c * dt) * ex;
              }
              px = bx + sox; pz = bz + soz;
          } else sinit = 0; }
        int16_t yws=(int16_t)(ram[0xB92B6]|(ram[0xB92B7]<<8));
        float rpx=px, rpz=pz; int ryaw=(int)yws;

        {
            static unsigned long long alf=~0ull, lastlog=0;
            static float apx=0, apz=0; static int ayaw=0, ahave=0;
            extern uint64_t s_frame_count;
            unsigned long long afq=(unsigned long long)s_frame_count;
            if (afq != alf) {
                if (ahave) {
                    float dp = fabsf(px-apx)+fabsf(pz-apz);
                    int dy = (((int)yws - ayaw) & 4095); if (dy > 2048) dy -= 4096;
                    if ((dp > 800.f || dy > 400 || dy < -400) && afq - lastlog > 30) {
                        lastlog = afq;
                        fprintf(stderr, "notsanom: f=%llu p=%.0f,%.0f prev=%.0f,%.0f dp=%.0f yaw=%d dyaw=%d\n",
                                afq, px, pz, apx, apz, dp, (int)yws, dy);
                    }
                }
                alf=afq; apx=px; apz=pz; ayaw=(int)yws; ahave=1;
            }
        }

        {
            static int gen=-1;
            if (gen<0){ const char*e=getenv("DC_NOTS_TGTGATE"); gen=(e&&*e=='0')?0:1; }
            if (gen) {
                static float fx=0, fy=0, fz=0; static int fyaw=0;
                static int fhave=0, far_n=0, yrej_n=0;
                static unsigned long long flf=~0ull;
                extern uint64_t s_frame_count;
                unsigned long long gfq=(unsigned long long)s_frame_count;
                dc_zone_change_tick(gfq);
                dc_cam_snap_hold_tick(gfq);

                int pzero = (px > -1.f && px < 1.f && pz > -1.f && pz < 1.f);
                {
                  static int st = -1, win = 0; static unsigned long long slf = ~0ull; static int pv[8]; static float ppx = 0, ppz = 0;
                  if (st < 0) { const char *e = getenv("DC_SCENE_TRACE"); st = (e && *e == '1') ? 1 : 0; }
                  if (st && gfq != slf) {
                      slf = gfq;
                      int v[8] = { ram[0xB8F30], ram[0xB7890], ram[0xB92C8], ram[0xB796C], ram[0xB796E], ram[0xB7878], ram[0xB93AA], ram[0xB93DE] };
                      int ch = fabsf(px - ppx) > 300.f || fabsf(pz - ppz) > 300.f;
                      for (int i = 0; i < 8; i++) if (v[i] != pv[i]) ch = 1;
                      if (ch) win = 24; else if (win > 0) win--;
                      extern float g_dc_cam_eye[3];
                      if (ch || win > 0) fprintf(stderr, "scenetrace: f=%llu B8F30=%02X GS64=%02X mode=%d cam=%d rec=%d screen=%d grab=%d,%d p=%.0f,%.0f yaw=%d eye=%.0f,%.0f,%.0f%s%c",
                                      gfq, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], px, pz, (int)yws,
                                      g_dc_cam_eye[0], g_dc_cam_eye[1], g_dc_cam_eye[2], ch ? " *" : "", 10);
                      for (int i = 0; i < 8; i++) pv[i] = v[i];
                      ppx = px; ppz = pz;
                  } }

                { static int zok = -1, zrun = 0; static unsigned long long zlf = ~0ull;
                  if (zok < 0) { const char *e = getenv("DC_CAM_ZERO_OK"); zok = e ? atoi(e) : 8; }
                  if (gfq != zlf) { zlf = gfq; zrun = (pzero && ram[0xB92C8] == 1) ? zrun + 1 : 0; }
                  if (zok > 0 && pzero && zrun >= zok) {
                      if (zrun == zok) fprintf(stderr, "notsgate: f=%llu her place is the origin (zero held for %d frames in mode 1), accepted%c", gfq, zrun, 10);
                      pzero = 0; } }

                { static int gg = -1, pgrab2 = -1;
                  if (gg < 0) { const char *e = getenv("DC_CAM_GRABGATE"); gg = (e && *e == '0') ? 0 : 1; }
                  int grab2 = (ram[0xB93AA] || ram[0xB93DE]) ? 1 : 0;
                  int gedge = (pgrab2 >= 0 && grab2 != pgrab2);
                  pgrab2 = grab2;
                  if (gg && !pzero && (grab2 || gedge)) {
                      fx=px; fy=py; fz=pz; fyaw=yws; fhave=1; flf=gfq; far_n=0; yrej_n=0; s_nots_gate_far=0;
                      if (gedge) { static long n = 0; if (++n <= 8) fprintf(stderr, "notsgate: GRABSNAP grab=%d p=%.0f,%.0f%c", grab2, fx, fz, 10); }
                  } }

                { static int ds = -1;
                  if (ds < 0) { const char *e = getenv("DC_CAM_DOORSNAP"); ds = (e && *e == '0') ? 0 : 1; }
                  if (ds && !pzero && dc_scripted_edge() && !dc_scripted_now()) {
                      fx=px; fy=py; fz=pz; fyaw=yws; fhave=1; flf=gfq; far_n=0; yrej_n=0; s_nots_gate_far=0;
                      static long n = 0;
                      if (++n <= 8) fprintf(stderr, "notsgate: DOORSNAP p=%.0f,%.0f yaw=%d%c", fx, fz, fyaw, 10);
                  } }

                { static int ds2 = -1, pcut = 0, arm = 0;
                  static unsigned long long armf = 0, qlf = ~0ull;
                  static float qx = 0, qz = 0; static int qyaw = 0, qhave = 0;
                  if (ds2 < 0) { const char *e = getenv("DC_CAM_DOORSNAP"); ds2 = (e && *e == '0') ? 0 : 1; }
                  extern int dc_cut_camera(void);

                  int cutn = ((ram[0xB92C8] == 4) || dc_cut_camera()) && !dc_scene_zonecam_ours();
                  if (pcut && !cutn) { arm = 1; armf = gfq; qhave = 0; s_takeover_pend = 1; s_takeover_f = gfq; }
                  pcut = cutn;
                  if (cutn) { arm = 0; qhave = 0;

                      if (s_takeover_pend || s_snap_arm_f || s_takeover_left || s_takeover_steady) {
                          static long n = 0;
                          if (++n <= 12) fprintf(stderr, "takeover: f=%llu the frame is with the game's camera again - hold released%c", gfq, 10);
                          s_takeover_pend = 0; s_snap_arm_f = 0; s_takeover_left = 0; s_takeover_steady = 0;
                      } }
                  if (arm && gfq != qlf) {
                      qlf = gfq;
                      int dq = (((int)yws - qyaw) & 4095); if (dq > 2048) dq -= 4096;
                      int same = qhave && fabsf(px-qx) < 120.f && fabsf(pz-qz) < 120.f
                                 && dq > -64 && dq < 64;
                      if (!pzero && same) {
                          if (ds2) {
                              fx=px; fy=py; fz=pz; fyaw=yws; fhave=1; flf=gfq;
                              far_n=0; yrej_n=0; s_nots_gate_far=0;
                          }
                          arm = 0; s_rec_arm = 1;
                          fprintf(stderr, "notsgate: CUTSNAP f=%llu p=%.0f,%.0f yaw=%d%c",
                                  gfq, px, pz, (int)yws, 10);
                      } else if (gfq - armf > 30) {
                          arm = 0; s_rec_arm = 1;
                          fprintf(stderr, "notsgate: CUTSNAP f=%llu TIMEOUT (the sample did not settle)%c",
                                  gfq, 10);
                      }
                      qx=px; qz=pz; qyaw=(int)yws; qhave=1;
                  } }
                if (!fhave) { if (!pzero) { fx=px; fy=py; fz=pz; fyaw=yws; fhave=1; flf=gfq; } }
                else if (gfq != flf && !pzero) {
                    flf = gfq;
                    float dx=px-fx, dy2=py-fy, dz=pz-fz;
                    float d2=dx*dx+dy2*dy2+dz*dz;
                    int dyw=(((int)yws-fyaw)&4095); if(dyw>2048) dyw-=4096;
                    int lw=0, age=99999; dc_ots_last_yaw_write(&lw, &age);
                    int dlw=(((int)yws-lw)&4095); if(dlw>2048) dlw-=4096;

                    int yok = (dyw>-400 && dyw<400) ||
                              (age<30 && dlw>-300 && dlw<300);
                    if (d2 < 120.f*120.f) {

                        fx=px; fy=py; fz=pz;

                        int in_cut_g = (ram[0xB92C8] == 4);
                        if (in_cut_g) { yrej_n = 0; }
                        else if (yok) { fyaw=yws; yrej_n=0; }
                        else if (++yrej_n >= 45) {
                            fyaw=yws; yrej_n=0;
                            fprintf(stderr, "notsgate: f=%llu YAWADOPT %d\n",
                                    gfq, (int)yws);
                        }
                        far_n=0; s_nots_gate_far=0;
                    } else if (d2 < 500.f*500.f) {

                        if (yok) {
                            fx=px; fy=py; fz=pz; fyaw=yws;
                            far_n=0; s_nots_gate_far=0; yrej_n=0;
                        }
                    } else if (dc_cam_scriptsnap_ok(ram, px, pz, gfq)) {

                        fx=px; fy=py; fz=pz; fyaw=yws; far_n=0; yrej_n=0; s_nots_gate_far=0;
                        { static long n = 0;
                          if (++n <= 12) fprintf(stderr, "notsgate: f=%llu SCRIPTSNAP p=%.0f,%.0f yaw=%d (the script moved her, d=%.0f)%c",
                                                 gfq, fx, fz, fyaw, sqrtf(d2), 10); }

                        if (dc_ours_shown_now()) dc_cam_snap_hold_arm(gfq);
                    } else if ((s_nots_gate_far = ++far_n) >= 20) {
                        if (age>120 || (dlw>-300 && dlw<300) || far_n>=40) {
                            fx=px; fy=py; fz=pz; fyaw=yws; far_n=0; yrej_n=0;
                            fprintf(stderr, "notsgate: f=%llu ADOPT p=%.0f,%.0f yaw=%d age=%d\n",
                                    gfq, fx, fz, fyaw, age);
                        }
                    }
                }
                if (fhave) { px=fx; py=fy; pz=fz; yws=(int16_t)fyaw; }
                { extern int g_dcns_gate[6];
                  g_dcns_gate[0] = (int)fx; g_dcns_gate[1] = (int)fz; g_dcns_gate[2] = fyaw; g_dcns_gate[3] = fhave;
                  g_dcns_gate[4] = far_n; g_dcns_gate[5] = yrej_n; }
            }

        {
            static int tl = -1;
            if (tl < 0) { const char *e = getenv("DC_CAM_TICKLEAD"); tl = (e && *e == '1') ? 1 : 0; }
            static float ppx=0, ppy=0, ppz=0; static int pph=0;
            static float dpx=0, dpy=0, dpz=0;
            extern uint64_t gpu_flip_seq(void);
            static unsigned long long plf=~0ull;
            unsigned long long q=(unsigned long long)gpu_flip_seq();
            if (q != plf) {
                plf = q;
                if (pph) { dpx=px-ppx; dpy=py-ppy; dpz=pz-ppz;
                           if (dpx>200.f||dpx<-200.f||dpz>200.f||dpz<-200.f||dpy>200.f||dpy<-200.f)
                               { dpx=dpy=dpz=0.f; } }
                ppx=px; ppy=py; ppz=pz; pph=1;
            }
            if (tl) { px+=dpx; py+=dpy; pz+=dpz; }
        }
        }

        static float yawoff=-1e9f;
        if(yawoff<-1e8f){ const char*e=getenv("DC_NOTS_YAWOFF"); yawoff=e?(float)atof(e):0.0f; }
        float wt=((float)yws+yawoff)*(6.2831853f/4096.0f);

        extern uint64_t s_frame_count;
        static float corr=0.f, lpx=0.f, lpz=0.f;
        static int swh=0, acal=-1; static unsigned long long lf=~0ull;
        if(acal<0){ const char*e=getenv("DC_NOTS_AUTOCAL"); acal=(e&&*e=='0')?0:1; }
        unsigned long long fseq=(unsigned long long)s_frame_count;
        if(!swh){ swh=1; lf=fseq; lpx=px; lpz=pz; }
        else if(fseq!=lf){ lf=fseq;
            float mdx=px-lpx, mdz=pz-lpz; lpx=px; lpz=pz;
            float m2=mdx*mdx+mdz*mdz;
            if(acal && m2>64.0f && m2<40000.0f){
                float me=atan2f(mdx,mdz)-(wt+corr);
                while(me> 3.14159265f) me-=6.2831853f;
                while(me<-3.14159265f) me+=6.2831853f;

                static float acwin=-1.f; static int acwall=-1;
                if(acwin<0.f){ const char*e=getenv("DC_NOTS_AUTOCAL_WIN");
                               acwin = e ? (float)atof(e) : 0.25f;
                               e=getenv("DC_NOTS_AUTOCAL_WALL"); acwall=(e&&*e=='1')?1:0; }
                int touching = (!acwall) && (ram[0xB93CE] || ram[0xB93EE]);

                int walking_act = (ram[0xB92C9] == 1 || ram[0xB92C9] == 5);

                static int acplay=-1;
                if(acplay<0){ const char*e=getenv("DC_NOTS_AUTOCAL_PLAY"); acplay=(e&&*e=='0')?0:1; }
                int playing = !acplay || (ram[0xB92C8] == 1);
                if(!touching && playing && walking_act && fabsf(me)<acwin) corr+=0.10f*me;
            }
        }
        { extern float g_dcns_corr; g_dcns_corr = corr; }

        if (!s_nots_oinit) { s_nots_oyaw = wt; s_nots_oinit = 1; }

        {
            static int rc = -1;
            if (rc < 0) { const char *e = getenv("DC_CAM_RECENTER"); rc = (e && *e == '0') ? 0 : 1; }

            if (rc && s_rec_arm) {
                s_rec_arm = 0;
                dc_cam_takeover_done();

                { static int cs = -1, pc = -1;
                  if (cs < 0) { const char *e = getenv("DC_CAM_CUTSNAP"); cs = (e && *e == '1') ? 1 : 0; }
                  if (pc < 0) { const char *e = getenv("DC_CAM_POSTCUT"); pc = e ? atoi(e) : 0; }
                  s_cam_cut_snap = cs;
                  s_cam_postcut = pc; }
                s_nots_oyaw = wt + corr;
                while (s_nots_oyaw >  3.14159265f) s_nots_oyaw -= 6.2831853f;
                while (s_nots_oyaw < -3.14159265f) s_nots_oyaw += 6.2831853f;
                fprintf(stderr, "cam recenter: f=%llu scene over -> yaw %.1f deg (behind her)"
                                " raw=%d gate=%d%c",
                        (unsigned long long)s_frame_count,
                        (wt + corr) * 57.2957795f, ryaw, (int)yws, 10);

                { static int en_e = -1; static float rr = 12000.f;
                  if (en_e < 0) { const char *e = getenv("DC_CAM_RECENTER_ENEMY"); en_e = (e && *e == '0') ? 0 : 1;
                                  e = getenv("DC_CAM_RECENTER_ENEMY_R"); if (e) rr = (float)atof(e); }
                  if (en_e) {
                      const float hx = (float)(int16_t)(ram[0xB92C0] | (ram[0xB92C1] << 8));
                      const float hz = (float)(int16_t)(ram[0xB92B0] | (ram[0xB92B1] << 8));
                      int bi = -1; float bd2 = rr * rr, rx0 = 0.f, rz0 = 0.f;
                      for (int i = 0; i < 10; i++) {
                          const uint32_t b = 0xB7ACCu + (uint32_t)i * 608u;
                          const uint32_t fl = (uint32_t)ram[b + 48] | ((uint32_t)ram[b + 49] << 8);
                          const int hp = (int16_t)(ram[b + 280] | (ram[b + 281] << 8));
                          if (!(fl & 1u) || hp <= 0 || !dc_actor_is_trex(ram, i)) continue;
                          const float ax = (float)(int16_t)(ram[b + 32] | (ram[b + 33] << 8));
                          const float az = (float)(int16_t)(ram[b + 36] | (ram[b + 37] << 8));
                          const float d2 = (ax - hx) * (ax - hx) + (az - hz) * (az - hz);
                          if (d2 < bd2) { bd2 = d2; bi = i; rx0 = ax; rz0 = az; }
                      }
                      if (bi >= 0) {
                          extern int dc_actor_box(int slot, float lo[3], float hi[3], float (*pts)[3], int maxp, int *np);
                          float lo[3], hi[3], bp[40][3]; int nb = 0;
                          float tx = rx0, tz = rz0; const char *src = "root";
                          if (dc_actor_box(bi, lo, hi, bp, 40, &nb) && nb > 0) {
                              int nn = 0; float nd = 1e18f;
                              for (int k = 0; k < nb; k++) { const float d = (bp[k][0] - hx) * (bp[k][0] - hx) + (bp[k][2] - hz) * (bp[k][2] - hz);
                                                             if (d < nd) { nd = d; nn = k; } }
                              float sx = 0.f, sz = 0.f; int ns = 0;
                              for (int k = 0; k < nb; k++) { const float dx = bp[k][0] - bp[nn][0], dz = bp[k][2] - bp[nn][2];
                                                             if (dx * dx + dz * dz < 700.f * 700.f) { sx += bp[k][0]; sz += bp[k][2]; ns++; } }
                              if (ns) { tx = sx / (float)ns; tz = sz / (float)ns; src = "head (bones nearest to her)"; }
                          }
                          if ((tx - hx) * (tx - hx) + (tz - hz) * (tz - hz) > 200.f * 200.f) {
                              s_nots_oyaw = atan2f(tx - hx, tz - hz);

                              static float up_deg = -1000.f;
                              if (up_deg < -999.f) { const char *e = getenv("DC_CAM_RECENTER_ENEMY_PITCH"); up_deg = e ? (float)atof(e) : 15.f; }
                              const float p0 = s_nots_opitch;
                              s_nots_opitch -= up_deg * 0.0174532925f;
                              if (s_nots_opitch < -1.26f) s_nots_opitch = -1.26f;
                              if (s_nots_opitch >  1.31f) s_nots_opitch =  1.31f;
                              fprintf(stderr, "cam recenter: f=%llu enemy %d at %.0f units - yaw %.1f deg at its %s, pitch %.1f -> %.1f deg (up %.0f)%c",
                                      (unsigned long long)s_frame_count, bi, sqrtf(bd2), s_nots_oyaw * 57.2957795f, src,
                                      p0 * 57.2957795f, s_nots_opitch * 57.2957795f, up_deg, 10);
                          }
                      }
                  } }
            }
            if (!rc) s_rec_arm = 0;
        }

        {
            static int rdbg = -1;
            if (rdbg < 0) { const char *e = getenv("DC_RECDBG"); rdbg = (e && *e == '1') ? 1 : 0; }
            if (rdbg) {
                static unsigned long long rlf = ~0ull, tail = 0;
                unsigned long long rf = (unsigned long long)s_frame_count;
                int m = ram[0xB92C8];
                if (m == 4) tail = rf + 150;
                if (rf != rlf && (m == 4 || rf < tail)) {
                    rlf = rf;
                    fprintf(stderr, "recdbg: f=%llu mode=%d act=%d raw=%.0f,%.0f/%d "
                                    "gate=%.0f,%.0f/%d corr=%.3f oyaw=%.1f%c",
                            rf, m, ram[0xB92C9], rpx, rpz, ryaw, px, pz, (int)yws,
                            corr, s_nots_oyaw * 57.2957795f, 10);
                }
            }
        }

        int cam_step = 0;
        {
            extern uint64_t gpu_flip_seq(void);
            static unsigned long long clf2=~0ull;
            unsigned long long q2=(unsigned long long)gpu_flip_seq();
            if (q2 != clf2) { clf2=q2; cam_step=1; }
        }

        if (cam_step) {
            extern int dc_tank_cam_follow(void);
            int tf = dc_tank_cam_follow();
            static int t_settle = 0; static uint64_t t_arm_vb = 0;
            if (tf == 1 || tf == 2) { t_settle = 1; t_arm_vb = s_frame_count; }
            else if (tf == 3) {
                if (!t_settle || s_nots_look_vb > t_arm_vb) { t_settle = 0; tf = 0; }
                else { float d0 = (wt + corr) - s_nots_oyaw;
                       while (d0 >  3.14159265f) d0 -= 6.2831853f;
                       while (d0 < -3.14159265f) d0 += 6.2831853f;
                       if (fabsf(d0) < 0.0087f) { t_settle = 0; tf = 0; } else tf = 1; }
            } else t_settle = 0;
            static int t_prev_yaw = 0x7FFFFFFF;
            const int t_turned = (t_prev_yaw != 0x7FFFFFFF && (int)yws != t_prev_yaw);
            t_prev_yaw = (int)yws;
            if (tf) {
                static float tk1 = -1.f, tk2 = 0.30f, tp2 = 0.35f; static int tlh = 45;
                if (tk1 < 0.f) { const char *e = getenv("DC_TANK_CAM_FOLLOW"); tk1 = e ? (float)atof(e) : 0.15f;
                                 e = getenv("DC_RE4_CAM_FOLLOW"); if (e) tk2 = (float)atof(e);
                                 e = getenv("DC_RE4_CAM_PITCH"); if (e) tp2 = (float)atof(e);
                                 e = getenv("DC_TANK_CAM_LOOKHOLD"); if (e) tlh = atoi(e); }
                extern int dc_game_paused(void);
                if (s_frame_count - s_nots_look_vb > (uint64_t)(tlh > 0 ? tlh : 0) && !dc_game_paused()) {
                    const float k = (tf == 2) ? tk2 : tk1;
                    float d = (wt + corr) - s_nots_oyaw;
                    while (d >  3.14159265f) d -= 6.2831853f;
                    while (d < -3.14159265f) d += 6.2831853f;
                    s_nots_oyaw += d * k;
                    while (s_nots_oyaw >  3.14159265f) s_nots_oyaw -= 6.2831853f;
                    while (s_nots_oyaw < -3.14159265f) s_nots_oyaw += 6.2831853f;
                    if (tf == 2 && tp2 >= 0.f) s_nots_opitch += (tp2 - s_nots_opitch) * k;
                    if (t_turned) s_tank_rigid_vb = s_frame_count;
                }
            }
        }

        static float lgx=0, lgy=0, lgz=0; static int lgh=0;
        static float vgx=0, vgy=0, vgz=0;
        static float vsx=0, vsy=0, vsz=0;
        static float rsx=0, rsy=0, rsz=0; static unsigned long long rsf=0;
        {
            unsigned long long q2=(unsigned long long)s_frame_count_ref();
            float dx=px-lgx, dy=py-lgy, dz=pz-lgz;

            int snap_state = 0;
            { static int pgrab = -1, en = -1;
              if (en < 0) { const char *e = getenv("DC_CAM_GRABSNAP"); en = (e && *e == '0') ? 0 : 1; }
              int grab = (ram[0xB93AA] || ram[0xB93DE]) ? 1 : 0;

              if (en && dc_scripted_edge() && !dc_scripted_now()) snap_state = 1;
              if (en && pgrab >= 0 && grab != pgrab) {
                  snap_state = 1;
                  static long n = 0;
                  if (++n <= 8)
                      fprintf(stderr, "camsnap: control state %d -> %d, re-centring%c",
                              pgrab, grab, 10);
              }
              pgrab = grab; }

            if (!lgh || snap_state || dx*dx+dy*dy+dz*dz > 600.f*600.f) {
                lgx=px; lgy=py; lgz=pz; lgh=1;
                vgx=vgy=vgz=0; vsx=vsy=vsz=0;
                rsx=px; rsy=py; rsz=pz; rsf=q2;
            } else {
                if (cam_step) {

                    vgx=px-rsx; vgy=py-rsy; vgz=pz-rsz;
                    if (vgx*vgx+vgy*vgy+vgz*vgz > 120.f*120.f) { vgx=vgy=vgz=0; }
                    rsx=px; rsy=py; rsz=pz;

                    float vs2 = vsx*vsx + vsy*vsy + vsz*vsz;
                    float vg2 = vgx*vgx + vgy*vgy + vgz*vgz;
                    static int kick = -1;

                    if (kick < 0) { const char *e = getenv("DC_CAM_KICK"); kick = (e && *e == '0') ? 0 : 1; }
                    int kicked = 0;
                    if (kick && vs2 < 4.f && vg2 > 64.f) {
                        vsx = vgx; vsy = vgy; vsz = vgz; kicked = 1;
                    } else {
                        vsx += (vgx - vsx) * s_cam_vlp();
                        vsy += (vgy - vsy) * s_cam_vlp();
                        vsz += (vgz - vsz) * s_cam_vlp();
                    }

                    if (s_cam_lp() > 0.f) {
                        float k = s_cam_lp();

                        static float lpx = 0, lpy = 0, lpz = 0; static int lph = 0;
                        static float lvx = 0, lvy = 0, lvz = 0;
                        if (!lph) { lpx = px; lpy = py; lpz = pz; lph = 1; }
                        if (cam_step) {
                            float ddx = px - lpx, ddy = py - lpy, ddz = pz - lpz;
                            lpx = px; lpy = py; lpz = pz;

                            static float lsnap = -1.f;
                            if (lsnap < 0.f) { const char *e2 = getenv("DC_CAM_LEAD_SNAP");
                                               lsnap = e2 ? (float)atof(e2) : 0.55f; }
                            float dv = fabsf(ddx - lvx) + fabsf(ddz - lvz);
                            float ar = (lsnap > 0.f && dv > 20.f) ? lsnap : 0.20f;
                            lvx += (ddx - lvx) * ar;
                            lvy += (ddy - lvy) * ar;
                            lvz += (ddz - lvz) * ar;

                            float lead = s_cam_lead();
                            float tx = px + lvx * lead;
                            float ty = py + lvy * lead;
                            float tz = pz + lvz * lead;
                            lgx += (tx - lgx) * k;
                            lgy += (ty - lgy) * k;
                            lgz += (tz - lgz) * k;
                        }
                        vsx = vgx; vsy = vgy; vsz = vgz;
                    } else if (kicked) {
                        lgx += vsx; lgy += vsy; lgz += vsz;
                    } else {
                        lgx += vsx + (px-lgx)*s_cam_spring();
                        lgy += vsy + (py-lgy)*s_cam_spring()*1.12f;
                        lgz += vsz + (pz-lgz)*s_cam_spring();
                    }

                    if (s_cam_lock()) { lgx = px; lgy = py; lgz = pz;
                                        vsx = vgx; vsy = vgy; vsz = vgz; }
                }
                (void)rsf;
            }
        }
        float cy=cosf(s_nots_oyaw), sy=sinf(s_nots_oyaw);
        float cp=cosf(s_nots_opitch), sp=sinf(s_nots_opitch);
        float dir[3]={ sy*cp, sp, cy*cp };

        static float s_tgt_k = 1.f;
        { static int en = -1, anyr = 0; static float lowk = 0.55f;
          static int s_grab_latch = 0; static uint64_t s_link_seen = 0;
          if (en < 0) { const char *e = getenv("DC_CAM_GRABTGT"); en = (e && *e == '0') ? 0 : 1;
                        e = getenv("DC_CAM_GRABTGT_K"); if (e) lowk = (float)atof(e);
                        e = getenv("DC_CAM_GRABTGT_ANY"); anyr = (e && *e == '1') ? 1 : 0; }

          extern uint64_t s_frame_count;

          int link = (ram[0xB93DE] || ram[0xB93DF]);

          { static int sm = -1, was = 0;
            if (sm < 0) { const char *e = getenv("DC_CAM_SCRIPTMOVE_TGT"); sm = (e && *e == '0') ? 0 : 1; }
            int smv = (sm && link && ram[0xB93DE] == 2 && ram[0xB93DF] == 0 &&
                       (ram[0xB92C8] == 4 || dc_cam_scriptmove_now(ram)));
            if (smv && !was)
                fprintf(stderr, "camtgt: scripted motion (0xB93DE=2, mode %d) - not a grab, look-at stays at the shoulders%c", ram[0xB92C8], 10);
            was = smv;
            if (smv) link = 0; }
          int react = (ram[0xB93AA] != 0);
          if (link) { s_grab_latch = 1; s_link_seen = s_frame_count; }
          else if (!react && (uint64_t)(s_frame_count - s_link_seen) > 30ull) s_grab_latch = 0;
          float want = (en && (anyr ? (react || link)
                                    : (link || (s_grab_latch && react)))) ? lowk : 1.f;
          s_tgt_k += (want - s_tgt_k) * 0.12f; }
        float tgt[3]={ lgx, lgy - nots_tgtup_eff() * s_tgt_k, lgz };

        {   float want = g_dc_cam_up;
            if (want > 0.f && s_cammesh_n > 0) {
                float upv[3] = { 0.f, -4000.f, 0.f };
                float t = cammesh_ray_t(s_cammesh, s_cammesh_n, tgt, upv);
                if (t < 1.f) { float room = 4000.f * t - 200.f; if (room < 0.f) room = 0.f; if (want > room) want = room; }
            }
            if (cam_step) { s_cam_up_live += (want - s_cam_up_live) * 0.25f;
                            if (fabsf(want - s_cam_up_live) < 0.5f) s_cam_up_live = want; }
            tgt[1] -= s_cam_up_live; }
        const float cam_up_k = s_cam_up_live;

        {
            static int en = -1; static float pad = 120.f, tau = 0.25f;
            static float s_clamp = 0.f;
            if (en < 0) { const char *e = getenv("DC_CAM_CEIL"); en = (e && *e == '1') ? 1 : 0;
                          e = getenv("DC_CAM_CEIL_PAD"); if (e) pad = (float)atof(e);
                          e = getenv("DC_CAM_CEIL_TAU"); if (e) tau = (float)atof(e); }
            float cwant = 0.f;
            if (en && s_cammesh_n > 0 && s_nots_opitch > 0.f) {
                float up[3] = { 0.f, -4000.f, 0.f };
                float t = cammesh_ray_t(s_cammesh, s_cammesh_n, tgt, up);
                if (t < 1.f) {
                    float headroom = 4000.f * t - pad;
                    float smax = (headroom > 0.f) ? headroom / (s_nd_dist > 1.f ? s_nd_dist : 1.f) : 0.f;
                    if (smax > 1.f) smax = 1.f;
                    float pmax = asinf(smax);
                    if (s_nots_opitch > pmax) cwant = s_nots_opitch - pmax;
                }
            }
            if (cam_step) {
                float k = (tau > 0.01f) ? (1.f - powf(0.05f, 1.f / (tau * 30.f))) : 1.f;
                s_clamp += (cwant - s_clamp) * k;
                if (fabsf(cwant - s_clamp) < 0.002f) s_clamp = cwant;
            }
            float pe = s_nots_opitch - s_clamp;
            if (pe > s_nots_opitch) pe = s_nots_opitch;

            s_nots_pitch_eff = pe;
            cp = cosf(pe); sp = sinf(pe);
            dir[0] = sy*cp; dir[1] = sp; dir[2] = cy*cp;
            { static int dbg = -1; static float last = -1.f;
              if (dbg < 0) { const char *e = getenv("DC_CAM_NEAR_DBG"); dbg = (e && *e != '0') ? 1 : 0; }
              if (dbg && fabsf(s_clamp - last) > 0.01f) { last = s_clamp;
                  fprintf(stderr, "camceil: pitch %.3f -> %.3f (clamp %.3f want %.3f)%c",
                          s_nots_opitch, pe, s_clamp, cwant, 10); } }
        }
        float rtl[3]={ cy, 0.f, -sy };

        static float s_dolly = 0.f;
        {
            static int en = -1;
            static float aim_dist = 650.f, run_amt = 400.f;
            static float aim_secs = 1.0f, run_secs = 0.7f;

            static float run_scale = 1.20f;
            if (en < 0) {
                const char *e = getenv("DC_CAM_DOLLY"); en = (e && *e == '0') ? 0 : 1;

                e = getenv("DC_CAM_AIM_DIST");    if (e) aim_dist = (float)atof(e);
                s_cam_aim_dist_pub = aim_dist;
                e = getenv("DC_CAM_DOLLY_RUN");   if (e) run_amt  = (float)atof(e);
                e = getenv("DC_CAM_DOLLY_RUN_SCALE"); if (e) run_scale = (float)atof(e);
                e = getenv("DC_CAM_DOLLY_AIMSEC");if (e) aim_secs = (float)atof(e);
                e = getenv("DC_CAM_DOLLY_RUNSEC");if (e) run_secs = (float)atof(e);
            }

            extern uint64_t s_frame_count;
            static uint64_t s_dlf = ~0ull;
            int newflip = ((uint64_t)s_frame_count != s_dlf);
            if (newflip) s_dlf = (uint64_t)s_frame_count;

            int act = ram[0xB92C9];
            extern uint16_t sio_get_pad_buttons(void);
            int run_held = ((sio_get_pad_buttons() & (1u << 15)) == 0);
            int aiming   = (act == 6);

            { extern int dc_cut_camera(void);
              if (dc_cut_camera()) { aiming = 0; run_held = 0; } }

            { extern int dc_aim_walkcam_now(void);
              if (aiming && dc_aim_walkcam_now()) aiming = 0; }

            int running  = (act == 5) || (run_held && act == 1);

            if (newflip) {
                float w = aiming ? 1.f : 0.f;
                s_cam_aim_k += (w - s_cam_aim_k) * 0.18f;
                if (s_cam_aim_k < 0.001f) s_cam_aim_k = 0.f;
                if (s_cam_aim_k > 0.999f) s_cam_aim_k = 1.f;
            }

            static float s_dolly_abs = -1.f;
            static float s_leg_from = 0.f, s_leg_t = 1.f, s_leg_target = -1e9f, s_leg_secs = 1.f, s_leg_wait = 0.f;
            static int   s_leg_aim = 0, s_leg_tr = TR_BACK_RUN, s_leg_st = 0, s_leg_prev = 0, s_leg_age = 1000;
            if (s_dolly_abs < 0.f) s_dolly_abs = s_nd_dist;

            extern int dc_aim_camback_now(void);
            static float ready_dist = -1.f;
            if (ready_dist < 0.f) { const char *e = getenv("DC_AIM_READY_DIST"); ready_dist = e ? (float)atof(e) : 1150.f; }
            const float aimk = g_dc_cam_aim_pct * 0.01f, runk = g_dc_cam_run_pct * 0.01f;
            const float aim_d = (dc_aim_camback_now() ? ready_dist : aim_dist) * aimk;
            s_cam_aim_dist_pub = aim_dist * aimk;
            s_cam_aim_floor = ((aim_dist < ready_dist) ? aim_dist : ready_dist) * aimk;
            const float run_d = (s_nd_dist + run_amt) * run_scale;
            float want  = (en && aiming)  ? aim_d
                        : (en && running) ? s_nd_dist + (run_d - s_nd_dist) * runk
                        :                   s_nd_dist;
            const int one = dc_cam_one_curve();
            const int lst = (en && aiming) ? 2 : (en && running) ? 1 : 0;
            if (newflip) {
                if (fabsf(want - s_leg_target) > 0.5f) {

                    const float start = s_dolly_abs;
                    int from = s_leg_st; if (from == 0 && s_leg_age < 4) from = s_leg_prev;
                    const int tr = (lst == 2) ? (from == 1 ? TR_AIM_RUN : TR_AIM) : (lst == 1) ? TR_RUN
                                 : (from == 2 ? TR_BACK_AIM : TR_BACK_RUN);
                    { static long nl = 0;
                      if (++nl <= 60)
                          fprintf(stderr, "dollyleg: cut at %.0f -> %.0f (aim=%d run=%d)%s%s%c",
                                  start, want, aiming, running, one ? " leg " : "", one ? s_camtr[tr].key : "", 10); }
                    s_leg_target = want;
                    s_leg_from   = start;
                    s_leg_t      = 0.f;
                    s_leg_aim    = (en && aiming);
                    s_leg_secs   = s_leg_aim ? aim_secs : run_secs;
                    s_leg_wait   = 0.f;
                    if (one) { s_leg_tr = tr; s_leg_secs = dc_camtr_dur(tr, want - start); s_leg_wait = s_camtr[tr].delay; }
                    if (lst != s_leg_st) { s_leg_prev = s_leg_st; s_leg_st = lst; s_leg_age = 0; }
                }
                if (s_leg_age < 100000) s_leg_age++;
                if (one && s_leg_wait > 0.f) s_leg_wait -= 1.f / 30.f;
                else if (s_leg_t < 1.f) {
                    s_leg_t += 1.f / (s_leg_secs > 0.05f ? s_leg_secs * 30.f : 1.f);
                    if (s_leg_t > 1.f) s_leg_t = 1.f;
                }
                float k = one ? dc_camtr_ease(s_leg_tr, s_leg_t)
                        : s_leg_aim
                        ? 1.f - (1.f - s_leg_t) * (1.f - s_leg_t)
                        : s_leg_t * s_leg_t * (3.f - 2.f * s_leg_t);
                s_dolly_abs = s_leg_from + (s_leg_target - s_leg_from) * k;
            }
            s_dolly = s_dolly_abs - s_nd_dist;

            { static int dbg = -1; static long n = 0;
              if (dbg < 0) { const char *e = getenv("DC_CAM_DOLLY_DBG"); dbg = (e && *e == '1') ? 1 : 0; }
              if (dbg && (++n % 30) == 0)
                  fprintf(stderr, "dolly: act=%d runbtn=%d run=%d aim=%d t=%.2f boom=%.0f -> %.0f%c",
                          act, run_held, running, aiming, s_leg_t,
                          s_dolly_abs, s_leg_target, 10); }
        }
        float dist_live = s_nd_dist + s_dolly;

        { static int f200 = -1;
          if (f200 < 0) { const char *e = getenv("DC_CAM_DIST_FLOOR200"); f200 = (e && *e == '1') ? 1 : 0; }
          float fl = 200.f;
          if (!f200 && s_nd_dist < 200.f) fl = (s_nd_dist > 1.f) ? s_nd_dist : 1.f;
          if (!f200 && s_cam_aim_floor < fl) fl = (s_cam_aim_floor > 1.f) ? s_cam_aim_floor : 1.f;
          if (dist_live < fl) dist_live = fl; }

        float gf_w = 0.f;
        { float pe_gf = s_nots_pitch_eff;
          gf_w = grabfocus_step(ram, cam_step, px, py, pz, s_nots_oyaw, tgt, dir, &dist_live, &pe_gf);
          if (gf_w > 0.f) s_nots_pitch_eff = pe_gf; }

        if (gf_w <= 0.f) { float pe_p = s_nots_pitch_eff;
          if (pushcam_step(ram, cam_step, tgt, dir, &dist_live, &pe_p) > 0.f) s_nots_pitch_eff = pe_p; }

        static int prfix = -1;
        if (prfix < 0) { const char *e = getenv("DC_CAM_PROBE_FIX");
                         prfix = (e && *e == '0') ? 0 : 1; }
        float sh_eff = prfix
                     ? nots_shoulder_eff_d(dist_live)
                     : nots_shoulder_eff_d(dist_live * s_cam_eff_t) *
                       (s_cam_eff_t > 0.05f ? (1.f / s_cam_eff_t) : 20.f);
        sh_eff *= (1.f - gf_w);
        float eyev[3]={ tgt[0]-dir[0]*dist_live + rtl[0]*sh_eff,
                        tgt[1]-dir[1]*dist_live,
                        tgt[2]-dir[2]*dist_live + rtl[2]*sh_eff };

        {
            float thit = 1.f, probe_hand = 1.f, wsk_raw = 1.f;
            float probe_eye[3] = { eyev[0], eyev[1], eyev[2] };

            uint32_t camroom = (uint32_t)((uint32_t)ram[0xB0FA0] | ((uint32_t)ram[0xB0FA1]<<8)
                                        | ((uint32_t)ram[0xB0FA2]<<16) | ((uint32_t)ram[0xB0FA3]<<24));
            cambox_load();

            extern int dc_room_live_on(void);
            int hand = dc_room_live_on() ? 1 : 0;
            for (int i = 0; i < s_cambox_n; i++) if (s_cambox[i].room == camroom) { hand = 1; break; }
            { extern int dc_room_library_index(void); extern int dc_room_effective(void);

              static int bydisc = -1;
              if (bydisc < 0) { const char *e = getenv("DC_CAMCOLL_DISCID"); bydisc = (e && *e == '0') ? 0 : 1; }
              int croom = dc_room_library_index();
              if (bydisc) {
                  static int nofile = -2;
                  const int dr = dc_room_effective();
                  if (dr >= 0 && dr != nofile) {
                      cammesh_load(dr);
                      if (s_cammesh_n > 0) croom = dr; else nofile = dr;
                  }
              }
              cammesh_load(croom);
              cammesh_tags_tick();
              if (s_cammesh_n > 0) hand = 1; }

            static int nocoll = -1;
            if (nocoll < 0) { extern int dc_room_live_on(void);
                              const char* e = getenv("DC_NOTS_NOCOLL");

                              nocoll = (e && *e == '1') ? 1 : 0; }

            {
                extern float pt_room_raycast(const float o[3], const float d[3], float maxt);
                extern int pt_room_has_mesh(void);
                extern int pt_room_coll_pick(float x, float y, float z);

                static int mc = -1; static float pad = 180.f;
                if (mc < 0) { extern int dc_room_live_on(void);
                              const char* e = getenv("DC_CAM_MESH_COLL");
                              mc = e ? (*e != '0') : (dc_room_live_on() ? 0 : 1);
                              e = getenv("DC_CAM_MESH_PAD"); if (e) pad = (float)atof(e); }
                { static int k = 0, st = -1;
                  if (st < 0) { const char* e = getenv("DC_CAM_MESH_STAT"); st = (e && *e == '1') ? 1 : 0; }
                  if (st && (k++ % 120) == 0)
                    { float tv2[3]={0,0,0}, ev2[3]={0,0,0}; extern int pt_camgeo_to_view(const float*, float*);
                      int okv = pt_camgeo_to_view(tgt, tv2) && pt_camgeo_to_view(eyev, ev2);
                      fprintf(stderr, "camcoll: ok=%d tgtV=(%.0f,%.0f,%.0f) eyeV=(%.0f,%.0f,%.0f) thit=%.3f%c",
                              okv, tv2[0], tv2[1], tv2[2], ev2[0], ev2[1], ev2[2], thit, 10); } }
                extern int pt_camgeo_count(void);
                extern float pt_camgeo_raycast(const float o[3], const float d[3], float maxt);
                extern int pt_camgeo_to_view(const float world[3], float out[3]);

                { extern int dc_coll_count(void);
                  extern float dc_coll_raycast(float,float,float,float,float);

                  if (!hand && !nocoll && dc_coll_count() > 0) {

                      float Rc[9], Tc[3], to[3], eo[3];
                      for (int q = 0; q < 9; q++)
                          Rc[q] = (float)(int16_t)(ram[0xBF9F4 + q*2] | (ram[0xBF9F5 + q*2] << 8)) / 4096.0f;
                      Tc[0] = (float)(int32_t)((uint32_t)ram[0xBFA08] | ((uint32_t)ram[0xBFA09]<<8) | ((uint32_t)ram[0xBFA0A]<<16) | ((uint32_t)ram[0xBFA0B]<<24));
                      Tc[1] = (float)(int32_t)((uint32_t)ram[0xBFA0C] | ((uint32_t)ram[0xBFA0D]<<8) | ((uint32_t)ram[0xBFA0E]<<16) | ((uint32_t)ram[0xBFA0F]<<24));
                      Tc[2] = (float)(int32_t)((uint32_t)ram[0xBFA10] | ((uint32_t)ram[0xBFA11]<<8) | ((uint32_t)ram[0xBFA12]<<16) | ((uint32_t)ram[0xBFA13]<<24));
                      { float d0[3] = { tgt[0]-Tc[0], tgt[1]-Tc[1], tgt[2]-Tc[2] };
                        float d1[3] = { eyev[0]-Tc[0], eyev[1]-Tc[1], eyev[2]-Tc[2] };
                        for (int q = 0; q < 3; q++) {
                            to[q] = Rc[0*3+q]*d0[0] + Rc[1*3+q]*d0[1] + Rc[2*3+q]*d0[2];
                            eo[q] = Rc[0*3+q]*d1[0] + Rc[1*3+q]*d1[1] + Rc[2*3+q]*d1[2];
                        } }

                      (void)to; (void)eo;
                      float twx = (float)(int16_t)(ram[0xB92B0] | (ram[0xB92B1] << 8));
                      float twz = (float)(int16_t)(ram[0xB92C0] | (ram[0xB92C1] << 8));
                      float dv[3] = { eyev[0]-tgt[0], eyev[1]-tgt[1], eyev[2]-tgt[2] };
                      float dw[3];
                      for (int q = 0; q < 3; q++)
                          dw[q] = Rc[0*3+q]*dv[0] + Rc[1*3+q]*dv[1] + Rc[2*3+q]*dv[2];
                      s_camwhy_map = 1;
                      float rx = dw[2], rz = dw[0];
                      float len = sqrtf(rx*rx + rz*rz);
                      if (len > 1.f) {
                          float hit = dc_coll_raycast(twx, twz, rx/len, rz/len, len);
                          if (hit < len) {
                              float keep = hit - pad; if (keep < 0.f) keep = 0.f;
                              float t = keep / len;
                              if (t < thit) thit = t;
                          }
                      }
                  } }

                static int meshray = -1;
                if (meshray < 0) { const char* e = getenv("DC_CAM_MESH_RAY"); meshray = (e && *e=='1') ? 1 : 0; }
                if (!hand && meshray && !nocoll && pt_camgeo_count() > 0) {
                    const float *tv2 = tgt, *ev2 = eyev;
                    s_camwhy_mesh = 1;
                    float ray[3] = { ev2[0]-tv2[0], ev2[1]-tv2[1], ev2[2]-tv2[2] };
                    float len = sqrtf(ray[0]*ray[0] + ray[1]*ray[1] + ray[2]*ray[2]);
                    if (len > 1.f) {
                        float dn[3] = { ray[0]/len, ray[1]/len, ray[2]/len };
                        float hit = pt_camgeo_raycast(tv2, dn, len);
                        if (hit < len) {
                            float keep = hit - pad; if (keep < 0.f) keep = 0.f;
                            float t = keep / len;
                            if (t < thit) thit = t;
                        }
                    }
                }
                if (!hand && mc && !nocoll) pt_room_coll_pick(tgt[0], tgt[1], tgt[2]);
                if (!hand && mc && !nocoll && pt_room_has_mesh()) {
                    float ray[3] = { eyev[0]-tgt[0], eyev[1]-tgt[1], eyev[2]-tgt[2] };
                    float len = sqrtf(ray[0]*ray[0] + ray[1]*ray[1] + ray[2]*ray[2]);
                    if (len > 1.f) {
                        float dnorm[3] = { ray[0]/len, ray[1]/len, ray[2]/len };
                        float hit = pt_room_raycast(tgt, dnorm, len);
                        if (hit < len) {
                            float keep = hit - pad; if (keep < 0.f) keep = 0.f;
                            float t = keep / len;
                            if (t < thit) thit = t;
                        }
                    }
                }
            }
            if (hand && !nocoll) {
                float t;
                if (s_cammesh_n > 0)      t = cammesh_hit(tgt, eyev);
                else if (camfb_enabled() && dc_camfb_count() > 0) {

                    static float ct[3] = {1e30f,0,0}, ce[3] = {0,0,0}, cres = 1.f; static int cbuf = -1, cn = 0;
                    if (cbuf == s_camfb_r && cn == s_camfb_n[s_camfb_r] &&
                        fabsf(ct[0]-tgt[0]) < 0.5f && fabsf(ct[1]-tgt[1]) < 0.5f && fabsf(ct[2]-tgt[2]) < 0.5f &&
                        fabsf(ce[0]-eyev[0]) < 0.5f && fabsf(ce[1]-eyev[1]) < 0.5f && fabsf(ce[2]-eyev[2]) < 0.5f)
                        t = cres;
                    else {
                        t = cammesh_hit_t(s_camfb[s_camfb_r], s_camfb_n[s_camfb_r], tgt, eyev);
                        for (int q = 0; q < 3; q++) { ct[q] = tgt[q]; ce[q] = eyev[q]; }
                        cres = t; cbuf = s_camfb_r; cn = s_camfb_n[s_camfb_r];
                    }
                }
                else                      t = cambox_hit(camroom, tgt, eyev);
                probe_hand = t;
                if (t < thit) { thit = t; s_camwhy_hand = 1; }
                { static int fbdbg = -1, fbk = 0;
                  if (fbdbg < 0) { const char *e = getenv("DC_CAMFB_DBG"); fbdbg = (e && *e == '1') ? 1 : 0; }
                  if (fbdbg && (fbk++ % 60) == 0)
                      fprintf(stderr, "camfb: authored=%d fallback_tris=%d thit=%.3f pivot=%.0f,%.0f,%.0f eye=%.0f,%.0f,%.0f%c",
                              s_cammesh_n, dc_camfb_count(), t, tgt[0], tgt[1], tgt[2], eyev[0], eyev[1], eyev[2], 10); }
            }
            if (nocoll || dc_room_live_on()) { if (nocoll) thit = 1.f; } else if (s_nots_bbhave) {
                float thit_before_box = thit;

                float Rc[9], Tc[3], tw[3], ew[3];
                for (int q = 0; q < 9; q++)
                    Rc[q] = (float)(int16_t)(ram[0xBF9F4 + q*2] | (ram[0xBF9F5 + q*2] << 8)) / 4096.0f;
                Tc[0] = (float)(int32_t)((uint32_t)ram[0xBFA08] | ((uint32_t)ram[0xBFA09]<<8) | ((uint32_t)ram[0xBFA0A]<<16) | ((uint32_t)ram[0xBFA0B]<<24));
                Tc[1] = (float)(int32_t)((uint32_t)ram[0xBFA0C] | ((uint32_t)ram[0xBFA0D]<<8) | ((uint32_t)ram[0xBFA0E]<<16) | ((uint32_t)ram[0xBFA0F]<<24));
                Tc[2] = (float)(int32_t)((uint32_t)ram[0xBFA10] | ((uint32_t)ram[0xBFA11]<<8) | ((uint32_t)ram[0xBFA12]<<16) | ((uint32_t)ram[0xBFA13]<<24));
                { float d0[3] = { tgt[0]-Tc[0], tgt[1]-Tc[1], tgt[2]-Tc[2] };
                  float d1[3] = { eyev[0]-Tc[0], eyev[1]-Tc[1], eyev[2]-Tc[2] };
                  float a[3], b[3];
                  for (int q = 0; q < 3; q++) {
                      a[q] = Rc[0*3+q]*d0[0] + Rc[1*3+q]*d0[1] + Rc[2*3+q]*d0[2];
                      b[q] = Rc[0*3+q]*d1[0] + Rc[1*3+q]*d1[1] + Rc[2*3+q]*d1[2];
                  }

                  for (int q = 0; q < 3; q++) { tw[q] = a[q]; ew[q] = b[q];
                                                s_camdbg_tw[q] = a[q]; s_camdbg_ew[q] = b[q]; } }
                for (int i=0;i<3;i++) {
                    float mn=s_nots_bbmin[i]+150.f, mx=s_nots_bbmax[i]-150.f;
                    if (mn>=mx) continue;
                    float d = ew[i]-tw[i];
                    if (d > 1.f || d < -1.f) {
                        float lim = (d > 0.f) ? mx : mn;
                        float t = (lim - tw[i]) / d;
                        if (t < thit) thit = t;
                    }
                }
                if (thit < 0.f) thit = 0.f;
                if (thit > 1.f) thit = 1.f;
                if (thit < thit_before_box - 0.001f) s_camwhy_box = 1;
            }
            { static int pdbg = -1;
              if (pdbg < 0) { const char* e = getenv("DC_CAMBOX_PROBE"); pdbg = (e && *e=='1') ? 1 : 0; }
              if (pdbg) { static unsigned long long k = 0;
                  if ((k++ % 30) == 0)
                      fprintf(stderr, "cambox room=%u  pivot=%.0f,%.0f,%.0f  eye=%.0f,%.0f,%.0f  boxes=%d hand=%d thit=%.2f%c",
                              camroom, tgt[0], tgt[1], tgt[2], eyev[0], eyev[1], eyev[2],
                              s_cambox_n, hand, thit, 10); } }
            { static int cdbg = -1;
              if (cdbg < 0) { const char* e = getenv("DC_CAMDBG"); cdbg = (e && *e=='1') ? 1 : 0; }
              if (cdbg) {
                  extern uint64_t gpu_flip_seq(void);
                  static unsigned long long lf = ~0ull;
                  unsigned long long fs = gpu_flip_seq();
                  if (fs != lf) { lf = fs;
                      fprintf(stderr, "camarm f=%llu want=%.0f thit=%.3f -> %.0f  map=%d box=%d coll=%d | pivotW=%.0f,%.0f,%.0f eyeW=%.0f,%.0f,%.0f | box=%.0f,%.0f,%.0f .. %.0f,%.0f,%.0f | pivotV=%.0f,%.0f,%.0f%c",
                              (unsigned long long)fs, s_nd_dist, thit, s_nd_dist * thit,
                              s_camwhy_map, s_camwhy_box + 2*s_camwhy_mesh + 4*s_camwhy_hand, dc_coll_count(),
                              s_camdbg_tw[0], s_camdbg_tw[1], s_camdbg_tw[2],
                              s_camdbg_ew[0], s_camdbg_ew[1], s_camdbg_ew[2],
                              s_nots_bbmin[0], s_nots_bbmin[1], s_nots_bbmin[2],
                              s_nots_bbmax[0], s_nots_bbmax[1], s_nots_bbmax[2],
                              tgt[0], tgt[1], tgt[2], 10); }
                  s_camwhy_map = 0; s_camwhy_box = 0; s_camwhy_mesh = 0; s_camwhy_hand = 0;
              } }

            static float eff_t = 1.f;
            static float prev_thit = 1.f;
            static float pullrate = -1.f; static int confirm = 1;
            if (pullrate < 0.f) {
                const char* e = getenv("DC_CAM_PULL_RATE"); pullrate = e ? (float)atof(e) : 0.34f;
                e = getenv("DC_CAM_CONFIRM"); if (e) confirm = atoi(e);
            }

            { static int ss = -1;
              if (ss < 0) { const char* e = getenv("DC_CAM_SUBSAMPLE");
                            ss = (e && *e == '0') ? 0 : 1; }
              static float sub_min = 1.f;
              if (ss) {
                  if (thit < sub_min) sub_min = thit;
                  if (cam_step) { thit = sub_min; sub_min = 1.f; }
              } }
            int trusted = (s_cammesh_n > 0);
            float want = thit;
            if (confirm && !trusted && thit < prev_thit - 0.02f) want = prev_thit;
            prev_thit = thit;

            float want_trk = want;
            { static int med = -1;
              if (med < 0) { const char* e = getenv("DC_CAM_MEDIAN");
                             med = (e && *e == '0') ? 0 : 1; }
              static float w0 = -1.f, w1 = -1.f;
              if (med) {
                  if (w0 < 0.f) { w0 = want; w1 = want; }
                  float a = w0, b = w1, c = want;
                  float mx = a > b ? a : b; if (c > mx) mx = c;
                  float mn = a < b ? a : b; if (c < mn) mn = c;

                  if (s_gfp_fast <= 0.02f) want_trk = a + b + c - mx - mn;
                  if (cam_step) { w0 = w1; w1 = want; }
              } }

            {
                static int wen = -1; static float wdeg = 22.f, wext = 1.25f, wrate = 45.f, wup = 0.6f;
                if (wen < 0) { const char *e = getenv("DC_CAM_WHISKER"); wen = (e && *e == '0') ? 0 : 1;
                               e = getenv("DC_CAM_WHISKER_DEG");  if (e) wdeg  = (float)atof(e);
                               e = getenv("DC_CAM_WHISKER_EXT");  if (e) wext  = (float)atof(e);
                               e = getenv("DC_CAM_WHISKER_RATE"); if (e) wrate = (float)atof(e);
                               e = getenv("DC_CAM_WHISKER_UP");   if (e) wup   = (float)atof(e); }
                static float s_wsk = 1.f;

                static float s_wsk_len = -1.f;
                int have_src = (s_cammesh_n > 0) || (camfb_enabled() && dc_camfb_count() > 0);
                if (wen && hand && !nocoll && have_src) {
                    float bx = eyev[0]-tgt[0], by = eyev[1]-tgt[1], bz = eyev[2]-tgt[2];
                    float blen = sqrtf(bx*bx + by*by + bz*bz);
                    float tmin = 1.f;
                    if (blen > 1.f) {
                        float ca = cosf(wdeg * 0.0174533f), sa = sinf(wdeg * 0.0174533f);
                        for (int w = -1; w <= 1; w += 2) {

                            float s = (w < 0) ? -sa : sa;
                            float rx = bx * ca - bz * s, rz = bx * s + bz * ca;
                            float d[3] = { rx * wext, by * wext, rz * wext };
                            float t = (s_cammesh_n > 0)
                                    ? cammesh_ray_pivot(s_cammesh, s_cammesh_n, tgt, d)
                                    : cammesh_ray_pivot(s_camfb[s_camfb_r], s_camfb_n[s_camfb_r], tgt, d);
                            t *= wext;
                            if (t < tmin) tmin = t;
                        }
                    }
                    if (tmin > 1.f) tmin = 1.f;
                    wsk_raw = tmin;
                    float tmin_len = tmin * blen;
                    if (s_wsk_len < 0.f) s_wsk_len = tmin_len;
                    if (cam_step) {

                        if (dc_cam_one_curve() && tmin >= 0.999f) s_wsk_len = tmin_len;
                        else if (tmin_len < s_wsk_len) { s_wsk_len -= wrate; if (s_wsk_len < tmin_len) s_wsk_len = tmin_len; }
                        else if (s_gfp_fast > 0.02f) s_wsk_len = tmin_len;
                        else { float k = (wup > 0.01f) ? (1.f - powf(0.05f, 1.f / (wup * 30.f))) : 1.f;
                               s_wsk_len += (tmin_len - s_wsk_len) * k; }
                        if (s_wsk_len < 20.f) s_wsk_len = 20.f;
                    }

                    if (s_wsk_len > tmin_len + 1.f && tmin < 1.f && tmin_len < s_wsk_len - wrate * 4.f) s_wsk_len = tmin_len + wrate * 4.f;
                    s_wsk = (blen > 1.f) ? (s_wsk_len / blen) : 1.f;
                    if (s_wsk < 0.02f) s_wsk = 0.02f; if (s_wsk > 1.f) s_wsk = 1.f;
                    if (s_wsk < want_trk && s_gfp_fast <= 0.02f) want_trk = s_wsk;
                    s_cam_wsk_len_pub = s_wsk_len;
                } else { s_wsk = 1.f; s_wsk_len = -1.f; s_cam_wsk_len_pub = -1.f; }
            }
            if (cam_step) s_cam_probe_pub = (thit < wsk_raw) ? thit : wsk_raw;

            { static int pdbg = -1;
              if (pdbg < 0) { const char *e = getenv("DC_CAM_PROBE_DBG"); pdbg = (e && *e == '1') ? 1 : 0; }
              if (pdbg && cam_step) {
                  extern uint64_t gpu_flip_seq(void);
                  fprintf(stderr, "camprobe f=%llu tgt=%.0f,%.0f,%.0f eye=%.0f,%.0f,%.0f hand=%.3f thit=%.3f wsk=%.3f want=%.3f eff=%.3f gfw=%.2f mesh=%d/%d%c",
                          (unsigned long long)gpu_flip_seq(), tgt[0], tgt[1], tgt[2], probe_eye[0], probe_eye[1], probe_eye[2],
                          probe_hand, thit, wsk_raw, want_trk, eff_t, gf_w, s_cammesh_room, s_cammesh_n, 10);
              } }

            static float hold_s = -1.f, out_s = 0.5f;
            if (hold_s < 0.f) {

                const char* e = getenv("DC_CAM_HOLD"); hold_s = e ? (float)atof(e) : 0.25f;

                e = getenv("DC_CAM_OUT");  out_s  = e ? (float)atof(e) : 2.0f;
                if (hold_s < 0.f) hold_s = 0.f;
                if (out_s < 0.05f) out_s = 0.05f;
            }
            const float CAM_HZ = 30.f;
            static int   hold_left = 0;
            static int   out_on = 0;
            static float out_from = 1.f, out_p = 0.f;

            static float deadband = -1.f;
            if (deadband < 0.f) { const char* e = getenv("DC_CAM_PULL_DEADBAND");
                                  deadband = e ? (float)atof(e) : 150.f; }
            float dead_t;
            { float bx = eyev[0]-tgt[0], by = eyev[1]-tgt[1], bz = eyev[2]-tgt[2];
              float blen = sqrtf(bx*bx + by*by + bz*bz);
              dead_t = (blen > 1.f) ? (deadband / blen) : 0.0005f;
              if (dead_t < 0.0005f) dead_t = 0.0005f; }

            static float s_eff_len = -1.f, s_out_from_len = -1.f;
            float blen_u;
            { float bx = eyev[0]-tgt[0], by = eyev[1]-tgt[1], bz = eyev[2]-tgt[2]; blen_u = sqrtf(bx*bx + by*by + bz*bz); }
            const int eunits = dc_cam_one_curve() && blen_u > 1.f;
            if (eunits) {
                eff_t = (s_eff_len < 0.f) ? 1.f : s_eff_len / blen_u;
                if (eff_t > 1.f) eff_t = 1.f;
                if (out_on && s_out_from_len >= 0.f) { out_from = s_out_from_len / blen_u; if (out_from > 1.f) out_from = 1.f; }
            }
            const int cut_snap_now = s_cam_cut_snap;
            if (s_cam_cut_snap) {
                s_cam_cut_snap = 0;
                if (!nocoll) { eff_t = want_trk; out_on = 0; hold_left = 0; }
                { static int dbg = -1;
                  if (dbg < 0) { const char* e = getenv("DC_CAM_PULL_DBG"); dbg = (e && *e=='1') ? 1 : 0; }
                  if (dbg) fprintf(stderr, "cutsnap: arm -> eff_t=%.3f (no hold, no glide)%c", eff_t, 10); }
            }
            if (nocoll) { eff_t = 1.f; out_on = 0; hold_left = 0; }

            else if (want_trk < eff_t - dead_t) {
                eff_t = want_trk;
                out_on = 0;
                hold_left = (int)(hold_s * CAM_HZ);
                (void)pullrate;
                { static int dbg = -1; static long n = 0;
                  if (dbg < 0) { const char* e = getenv("DC_CAM_PULL_DBG"); dbg = (e && *e=='1') ? 1 : 0; }
                  if (dbg && cam_step) fprintf(stderr, "pullin #%ld -> eff_t=%.3f (dead=%.4f)%c",
                                               ++n, eff_t, dead_t, 10); }
            }
            else if (want_trk > eff_t + dead_t || (out_on && dc_cam_out_one() && want_trk > eff_t + 0.0005f)) {

                const int gfp_fast = (s_gfp_fast > 0.02f);
                if (gfp_fast) { hold_left = 0; out_on = 0; eff_t = want_trk; }
                else if (hold_left > 0) {
                    if (cam_step) hold_left--;
                } else {
                    if (!out_on) { out_on = 1; out_from = eff_t; out_p = 0.f; s_out_from_len = eff_t * blen_u; }
                    if (cam_step) out_p += 1.f / (((gfp_fast && out_s > 0.3f) ? 0.3f : out_s) * CAM_HZ);
                    if (out_p >= 1.f) { out_p = 1.f; out_on = 0; }

                    float p = out_p;
                    float e2 = p*p*p*(p*(p*6.f - 15.f) + 10.f);
                    eff_t = out_from + (want_trk - out_from) * e2;
                }
            } else {
                out_on = 0;

                static float track_s = -1.f;
                if (track_s < 0.f) { const char* e = getenv("DC_CAM_TRACK");
                                     track_s = e ? (float)atof(e) : 0.12f;
                                     if (track_s < 0.f) track_s = 0.f; }
                if (s_gfp_fast > 0.02f) eff_t = want_trk;
                else if (cam_step && track_s > 0.f) {
                    float k = 1.f - powf(0.05f, 1.f / (track_s * CAM_HZ));
                    if (k > 1.f) k = 1.f;
                    float step = (want_trk - eff_t) * k;

                    { static float outmax = -1.f;
                      if (outmax < 0.f) { const char* e = getenv("DC_CAM_TRACK_OUT");
                                          outmax = e ? (float)atof(e) : 45.f; }
                      if (step > 0.f && outmax > 0.f) {
                          float bx = eyev[0]-tgt[0], by = eyev[1]-tgt[1], bz = eyev[2]-tgt[2];
                          float blen = sqrtf(bx*bx + by*by + bz*bz);
                          float lim = (blen > 1.f) ? (outmax / blen) : step;
                          if (step > lim) step = lim;
                      } }
                    eff_t += step;
                }
            }

            { static int dbg = -1;
              if (dbg < 0) { const char* e = getenv("DC_CAM_DIST"); dbg = (e && *e == '1') ? 1 : 0; }
              if (dbg && cam_step) {
                  static float last = -1.f;
                  if (fabsf(eff_t - last) > 0.002f) {
                      last = eff_t;
                      fprintf(stderr, "camdist: t=%.3f want=%.3f hold=%d out=%.2f%c",
                              eff_t, want, hold_left, out_p, 10);
                  }
              } }

            { static float mind = -1.f;
              if (mind < 0.f) { const char *e = getenv("DC_CAM_MIN_DIST");
                                mind = e ? (float)atof(e) : 250.f;
                                if (mind < 0.f) mind = 0.f; }
              float bx = eyev[0]-tgt[0], by = eyev[1]-tgt[1], bz = eyev[2]-tgt[2];
              float blen = sqrtf(bx*bx + by*by + bz*bz);
              float min_t = (blen > 1.f) ? (mind / blen) : 1.f;
              if (min_t > 1.f) min_t = 1.f;
              if (min_t < 0.02f) min_t = 0.02f;

              static int fc = -1;
              if (fc < 0) { const char *e = getenv("DC_CAM_FLOOR_COLL");
                            fc = (e && *e == '0') ? 0 : 1; }
              float floor_t = min_t;
              int thru = (min_t > want);
              if (fc && !nocoll && thru) floor_t = want;
              if (floor_t < 0.02f) floor_t = 0.02f;
              if (eff_t < floor_t) eff_t = floor_t;

              { static int wdbg = -1;
                if (wdbg < 0) { const char *e = getenv("DC_CAM_WALL_DBG");
                                wdbg = (e && *e == '1') ? 1 : 0; }
                if (wdbg && cam_step)
                    fprintf(stderr,
                            "camwall: want=%.3f min=%.3f floor=%.3f efft=%.4f "
                            "arm=%.1f blen=%.1f thru=%d piv=%.1f,%.1f,%.1f "
                            "her=%d,%d,%d cap=%d,%d%c",
                            want, min_t, floor_t, eff_t, eff_t * blen, blen,
                            thru, tgt[0], tgt[1], tgt[2],
                            (int)(int16_t)(ram[0xB92AC] | (ram[0xB92AD] << 8)),
                            (int)(int16_t)(ram[0xB92AE] | (ram[0xB92AF] << 8)),
                            (int)(int16_t)(ram[0xB92B0] | (ram[0xB92B1] << 8)),
                            (int)(int16_t)(ram[0xB93C8] | (ram[0xB93C9] << 8)),
                            (int)(int16_t)(ram[0xB93CA] | (ram[0xB93CB] << 8)),
                            10); } }

            { static float prev_dl = -1.f;
              float bx = eyev[0]-tgt[0], by = eyev[1]-tgt[1], bz = eyev[2]-tgt[2];
              float blen = sqrtf(bx*bx + by*by + bz*bz);
              if (prev_dl >= 0.f && dist_live > prev_dl + 0.5f) {
                  if (eff_t > want) eff_t = want;
              }
              if (blen > 1.f) {
                  extern float dc_cam_whisker_len(void);
                  float wl = dc_cam_whisker_len();
                  if (wl >= 0.f && eff_t * blen > wl && s_gfp_fast <= 0.02f) eff_t = wl / blen;
              }
              if (cam_step) prev_dl = dist_live; }

            { static float len_prev = -1.f, dl_prev1 = -1.f;
              static long nlog = 0;
              if (cut_snap_now || nocoll || s_gfp_fast > 0.02f) len_prev = -1.f;
              if (dc_cam_out_one() && eunits && len_prev > 1.f && dl_prev1 >= 0.f && dist_live < dl_prev1 - 0.5f &&
                  eff_t * blen_u > len_prev + 0.5f) {
                  const float was = eff_t * blen_u;
                  eff_t = len_prev / blen_u;
                  if (eff_t > 1.f) eff_t = 1.f;
                  if (out_on) { s_out_from_len = eff_t * blen_u; out_from = eff_t; out_p = 0.f; }
                  { static int dbg = -1;
                    if (dbg < 0) { const char *e = getenv("DC_CAM_NEAR_DBG"); dbg = e ? atoi(e) : 0; }
                    if (dbg >= 1 && cam_step && ++nlog <= 400)
                        fprintf(stderr, "camoutone: arm %.0f -> %.0f shrinking, eye held at %.0f (would go out to %.0f)%c",
                                dl_prev1, dist_live, eff_t * blen_u, was, 10); }
              }
              if (cam_step) { len_prev = eff_t * blen_u; dl_prev1 = dist_live; } }
            if (eunits) s_eff_len = (eff_t >= 0.9995f) ? -1.f : eff_t * blen_u;
            for (int i=0;i<3;i++) eyev[i] = tgt[i] + (eyev[i]-tgt[i])*eff_t;

            if (prfix) {

                static int shfix = -1;
                if (shfix < 0) { const char *e = getenv("DC_CAM_GRABFOCUS_SHFIX"); shfix = (e && *e == '0') ? 0 : 1; }
                float want_sh = nots_shoulder_eff_d(dist_live * eff_t) * (shfix ? (1.f - gf_w) : 1.f);
                float have_sh = sh_eff * eff_t;
                float dsh = want_sh - have_sh;
                eyev[0] += rtl[0] * dsh;
                eyev[2] += rtl[2] * dsh;
            }

            {
                static int en = -1, coll = 1;
                static float d_start = 900.f, d_end = 280.f, gate = 0.90f;
                static float side_aim = 0.15f, side_walk = 0.08f, rise_frac = 0.30f;
                static float dclose = 450.f, rc_w = 150.f, rc_soft = 90.f, rc_hard = 30.f, b_rate = 60.f;
                static float hard = 230.f, upmix = 0.45f, cap = 0.75f, offpad = 60.f;
                static float lam_in = 0.10f, lam_out = 0.50f, cut_in = 0.10f, cut_out = 0.30f;

                static float shift = 0.07f;
                if (en < 0) {
                    const char *e = getenv("DC_CAM_NEAR");        en = (e && *e == '0') ? 0 : 1;
                    e = getenv("DC_CAM_NEAR_START");     if (e) d_start   = (float)atof(e);
                    e = getenv("DC_CAM_NEAR_END");       if (e) d_end     = (float)atof(e);
                    e = getenv("DC_CAM_NEAR_GATE");      if (e) gate      = (float)atof(e);
                    e = getenv("DC_CAM_NEAR_SIDE_AIM");  if (e) side_aim  = (float)atof(e);
                    e = getenv("DC_CAM_NEAR_SIDE_WALK"); if (e) side_walk = (float)atof(e);
                    e = getenv("DC_CAM_NEAR_SHIFT");     if (e) shift     = (float)atof(e);
                    e = getenv("DC_CAM_NEAR_RISE");      if (e) rise_frac = (float)atof(e);
                    e = getenv("DC_CAM_NEAR_CLOSE");     if (e) dclose    = (float)atof(e);
                    e = getenv("DC_CAM_NEAR_RC_W");      if (e) rc_w      = (float)atof(e);
                    e = getenv("DC_CAM_NEAR_RC_SOFT");   if (e) rc_soft   = (float)atof(e);
                    e = getenv("DC_CAM_NEAR_RC_HARD");   if (e) rc_hard   = (float)atof(e);
                    e = getenv("DC_CAM_NEAR_B_RATE");    if (e) b_rate    = (float)atof(e);
                    e = getenv("DC_CAM_NEAR_FLOOR");     if (e) hard      = (float)atof(e);
                    e = getenv("DC_CAM_NEAR_UPMIX");     if (e) upmix     = (float)atof(e);
                    e = getenv("DC_CAM_NEAR_SIDE_CAP");  if (e) cap       = (float)atof(e);
                    e = getenv("DC_CAM_NEAR_OFF_PAD");   if (e) offpad    = (float)atof(e);
                    e = getenv("DC_CAM_NEAR_LAM_IN");    if (e) lam_in    = (float)atof(e);
                    e = getenv("DC_CAM_NEAR_LAM_OUT");   if (e) lam_out   = (float)atof(e);
                    e = getenv("DC_CAM_NEAR_CUT_IN");    if (e) cut_in    = (float)atof(e);
                    e = getenv("DC_CAM_NEAR_CUT_OUT");   if (e) cut_out   = (float)atof(e);
                    e = getenv("DC_CAM_NEAR_COLL");      coll = (e && *e == '0') ? 0 : 1;
                    if (d_start < d_end + 1.f) d_start = d_end + 1.f;
                    if (dclose < 40.f) dclose = 40.f;
                }
                static float s_lam = 0.f, s_depthB = -1.f, s_cut = 1.f;
                extern int dc_cut_camera(void);
                int in_cut = dc_cut_camera();

                float arm_raw = dist_live * eff_t;
                float A[3] = { eyev[0], eyev[1], eyev[2] };

                float u[3] = { -dir[0], -dir[1], -dir[2] };
                float ak = s_cam_aim_k;
                float sgn = (s_nd_shoulder * cam_side_k() < 0.f) ? -1.f : 1.f;
                int have_src = hand && !nocoll && coll &&
                               ((s_cammesh_n > 0) || (camfb_enabled() && dc_camfb_count() > 0));
                const float (*tris)[9] = (s_cammesh_n > 0) ? s_cammesh : s_camfb[s_camfb_r];
                int ntri = (s_cammesh_n > 0) ? s_cammesh_n : s_camfb_n[s_camfb_r];

                float t_c = 1.f, t_min = 1.f, soft_allowed = -1.f, hard_allowed = -1.f;
                if (en && have_src) {
                    float d[3] = { u[0]*dclose, u[1]*dclose, u[2]*dclose };
                    for (int w = -1; w <= 1; w++) {
                        float o[3] = { tgt[0] + rtl[0] * rc_w * (float)w, tgt[1], tgt[2] + rtl[2] * rc_w * (float)w };
                        float t = cammesh_ray_pivot(tris, ntri, o, d);
                        if (w == 0) t_c = t;
                        if (t < t_min) t_min = t;
                    }

                    static float ang1 = -1.f, ang2 = 45.f, ang2k = 1.35f;
                    if (ang1 < 0.f) { const char *e = getenv("DC_CAM_NEAR_ANG1"); ang1 = e ? (float)atof(e) : 25.f;
                                      e = getenv("DC_CAM_NEAR_ANG2");   if (e) ang2  = (float)atof(e);
                                      e = getenv("DC_CAM_NEAR_ANG2_K"); if (e) ang2k = (float)atof(e); }
                    for (int k = 0; k < 4; k++) {
                        float deg = (k < 2) ? ang1 : ang2;
                        float s = (k & 1) ? 1.f : -1.f;
                        float ca = cosf(deg * 0.0174533f), sa = sinf(deg * 0.0174533f) * s;
                        float rx = u[0] * ca - u[2] * sa, rz = u[0] * sa + u[2] * ca;
                        float da[3] = { rx * dclose, u[1] * dclose, rz * dclose };
                        float t = cammesh_ray_pivot(tris, ntri, tgt, da);
                        if (k >= 2) { t *= ang2k; if (t > 1.f) t = 1.f; }
                        if (t < t_min) t_min = t;
                    }
                    soft_allowed = dclose * t_min - rc_soft;
                    hard_allowed = dclose * t_c   - rc_hard;
                }
                float depth_want = dclose;
                if (soft_allowed >= 0.f && soft_allowed < depth_want) depth_want = soft_allowed;
                if (depth_want < 40.f) depth_want = 40.f;
                if (s_depthB < 0.f || in_cut) s_depthB = depth_want;

                static float b_out = -1.f;
                if (b_out < 0.f) { const char *e = getenv("DC_CAM_NEAR_B_OUT"); b_out = e ? (float)atof(e) : 15.f; }
                if (cam_step) {
                    float dd = depth_want - s_depthB;
                    if (dd >  b_out)  dd =  b_out;
                    if (dd < -b_rate) dd = -b_rate;
                    s_depthB += dd;
                }
                if (hard_allowed >= 0.f && s_depthB > hard_allowed) s_depthB = (hard_allowed > 40.f) ? hard_allowed : 40.f;
                float depthB = s_depthB;

                float sideB = 0.f, riseB = 0.f;
                if (fabsf(s_nd_shoulder * cam_side_k()) > 1.f) {
                    float f_add = side_aim * ak + side_walk * (1.f - ak) + shift;
                    f_add /= dc_ws_frame_k();

                    sideB = nots_shoulder_eff_d(depthB) + f_add * depthB * sgn * fabsf(cam_side_k());
                }
                riseB = rise_frac * depthB * (1.f - ak);
                if (hard > 1.f && depthB < hard) {

                    float need = (hard*hard - depthB*depthB) / hard;
                    float uu = upmix * (1.f - ak);
                    float lat = sqrtf(1.f - uu*uu);
                    sideB += need * lat * sgn;
                    riseB += need * uu;
                }
                if (cap > 0.001f) {
                    float lim = cap * depthB;
                    if (fabsf(sideB) > lim) sideB = (sideB < 0.f) ? -lim : lim;
                }
                float B0[3] = { tgt[0] + u[0]*depthB, tgt[1] + u[1]*depthB, tgt[2] + u[2]*depthB };

                float t_off = 1.f, t_hard = 1.f;
                if (en && have_src && (fabsf(sideB) > 0.5f || riseB > 0.5f)) {
                    float dv[3] = { rtl[0]*sideB, -riseB, rtl[2]*sideB };
                    float len = sqrtf(dv[0]*dv[0] + dv[1]*dv[1] + dv[2]*dv[2]);
                    if (len > 1.f) {
                        float t = cammesh_ray_t(tris, ntri, B0, dv);
                        if (t < 1.f) { t_hard = t - (offpad * 0.33f) / len; t_off = t - offpad / len; }
                        if (t_off < 0.f) t_off = 0.f; if (t_hard < 0.f) t_hard = 0.f;
                    }
                }
                if (in_cut) s_cut = t_off;
                if (cam_step) {
                    float tau = (t_off < s_cut) ? cut_in : cut_out;
                    float k = (tau > 0.01f) ? (1.f - powf(0.05f, 1.f / (tau * 30.f))) : 1.f;
                    s_cut += (t_off - s_cut) * k;
                }
                if (s_cut > t_hard) s_cut = t_hard;
                if (s_cut < 0.f) s_cut = 0.f; if (s_cut > 1.f) s_cut = 1.f;
                float B[3] = { B0[0] + rtl[0]*sideB*s_cut, B0[1] - riseB*s_cut, B0[2] + rtl[2]*sideB*s_cut };

                float q = (d_start - arm_raw) / (d_start - d_end);
                if (q < 0.f) q = 0.f; if (q > 1.f) q = 1.f;
                q = q * q * (3.f - 2.f * q);
                float g = 1.f;
                if (gate < 0.999f) {
                    g = (gate - eff_t) / (gate * 0.35f + 1e-6f);
                    if (g < 0.f) g = 0.f; if (g > 1.f) g = 1.f;
                    g = g * g * (3.f - 2.f * g);
                }
                float lam_t = (en && have_src) ? q * g : 0.f;
                if (in_cut) s_lam = 0.f;
                if (cam_step && s_cam_postcut > 0 && !in_cut) {
                    s_cam_postcut--;
                    if (lam_t > s_lam) s_lam = lam_t;
                    else { float k = (lam_out > 0.01f) ? (1.f - powf(0.05f, 1.f / (lam_out * 30.f))) : 1.f;
                           s_lam += (lam_t - s_lam) * k; }
                    if (s_lam < 0.0005f) s_lam = 0.f; if (s_lam > 1.f) s_lam = 1.f;
                } else if (cam_step) {
                    float tau = (lam_t > s_lam) ? lam_in : lam_out;
                    float k = (tau > 0.01f) ? (1.f - powf(0.05f, 1.f / (tau * 30.f))) : 1.f;
                    s_lam += (lam_t - s_lam) * k;
                    if (s_lam < 0.0005f) s_lam = 0.f; if (s_lam > 1.f) s_lam = 1.f;
                }

                for (int i = 0; i < 3; i++) eyev[i] = A[i] + (B[i] - A[i]) * s_lam;

                float t_net = 1.f;
                if (s_lam > 0.001f && have_src) {
                    float d[3] = { eyev[0]-tgt[0], eyev[1]-tgt[1], eyev[2]-tgt[2] };
                    float len = sqrtf(d[0]*d[0] + d[1]*d[1] + d[2]*d[2]);
                    if (len > 1.f) {
                        float t = cammesh_ray_pivot(tris, ntri, tgt, d);
                        if (t < 1.f) {
                            float keep = t - rc_hard / len; if (keep < 0.f) keep = 0.f;
                            for (int i = 0; i < 3; i++) eyev[i] = tgt[i] + d[i] * keep;
                            t_net = keep;
                        }
                    }
                }

                float arm_eff = arm_raw + (depthB - arm_raw) * s_lam;
                float side_eff = sideB * s_cut * s_lam, rise_eff = riseB * s_cut * s_lam;
                { float a = (arm_eff > 1.f) ? arm_eff : 1.f;
                  float f_lat  = (fabsf(nots_shoulder_eff_d(arm_eff)) + fabsf(side_eff)) / a;
                  float f_vert = (rise_eff / a) * (0.5f / 0.375f);
                  float c = (f_lat > f_vert) ? f_lat : f_vert;
                  if (c > 1.f) c = 1.f;
                  s_cam_near_clear = c; }

                { static int dbg = -1; static long n = 0;
                  if (dbg < 0) { const char *e = getenv("DC_CAM_NEAR_DBG"); dbg = e ? atoi(e) : 0; }
                  if (dbg == 1 && cam_step && (++n % 10) == 0)
                      fprintf(stderr, "camnear: armraw=%.0f depthB=%.0f lam=%.2f aim=%.2f side=%.0f rise=%.0f cut=%.2f clear=%.2f%c",
                              arm_raw, depthB, s_lam, ak, side_eff, rise_eff, s_cut, s_cam_near_clear, 10);
                  if (dbg == 2 && cam_step) {
                      extern uint64_t gpu_flip_seq(void);
                      static float pe[3] = {0,0,0}, pt[3] = {0,0,0}; static int have = 0;
                      float dx = eyev[0]-pe[0], dy = eyev[1]-pe[1], dz = eyev[2]-pe[2];
                      float d = have ? sqrtf(dx*dx + dy*dy + dz*dz) : 0.f;
                      float tx = tgt[0]-pt[0], ty = tgt[1]-pt[1], tz = tgt[2]-pt[2];
                      float dt = have ? sqrtf(tx*tx + ty*ty + tz*tz) : 0.f;
                      fprintf(stderr,
                              "nearstep f=%llu d=%.0f dtgt=%.0f yaw=%.3f pit=%.3f eye=%.0f,%.0f,%.0f tgt=%.0f,%.0f,%.0f "
                              "thit=%.3f wtrk=%.3f efft=%.3f armraw=%.0f tc=%.3f tmin=%.3f dwant=%.0f depthB=%.0f "
                              "q=%.2f g=%.2f lam=%.3f aim=%.2f sideB=%.0f riseB=%.0f toff=%.2f cut=%.2f net=%.2f src=%d act=%d ryaw=%d dl=%.0f corr=%.3f%c",
                              (unsigned long long)gpu_flip_seq(), d, dt, s_nots_oyaw, s_nots_opitch,
                              eyev[0], eyev[1], eyev[2], tgt[0], tgt[1], tgt[2],
                              thit, want_trk, eff_t, arm_raw, t_c, t_min, depth_want, depthB,
                              q, g, s_lam, ak, sideB, riseB, t_off, s_cut, t_net,
                              (s_cammesh_n > 0) ? 1 : (have_src ? 2 : 0), ram[0xB92C9],
                              (int)(int16_t)(ram[0xB92B6] | (ram[0xB92B7] << 8)), dist_live,
                              s_nots_her - wt, 10);
                      pe[0]=eyev[0]; pe[1]=eyev[1]; pe[2]=eyev[2]; have = 1;
                      pt[0]=tgt[0]; pt[1]=tgt[1]; pt[2]=tgt[2];
                  } }
            }

            s_cam_eff_t = eff_t;
        }

        { extern float g_dc_cam_eye[3], g_dc_cam_tgt[3];
          g_dc_cam_eye[0]=eyev[0]; g_dc_cam_eye[1]=eyev[1]; g_dc_cam_eye[2]=eyev[2];
          g_dc_cam_tgt[0]=tgt[0];  g_dc_cam_tgt[1]=tgt[1] + cam_up_k;  g_dc_cam_tgt[2]=tgt[2]; }

        { static int jit = -1;
          if (jit < 0) { const char *e = getenv("DC_CAMJIT"); jit = (e && *e == '1') ? 1 : 0; }
          if (jit) {
              extern uint64_t gpu_flip_seq(void);
              static unsigned long long lastf = ~0ull;
              unsigned long long f = (unsigned long long)gpu_flip_seq();
              if (f != lastf) {
                  static float pe[3] = {0.f, 0.f, 0.f}; static int have = 0;
                  float dx = eyev[0]-pe[0], dy = eyev[1]-pe[1], dz = eyev[2]-pe[2];
                  float d = have ? sqrtf(dx*dx + dy*dy + dz*dz) : 0.f;

                  unsigned int ph = 2166136261u;
                  for (int q = 0; q < 9; q++) {
                      int v = (int)(s_pairset_curR[0][q] * 4096.f);
                      ph = (ph ^ (unsigned int)v) * 16777619u;
                  }
                  for (int q = 0; q < 3; q++) {
                      int v = (int)s_pairset_curT[0][q];
                      ph = (ph ^ (unsigned int)v) * 16777619u;
                  }
                  fprintf(stderr,
                          "camjit f=%llu eye=%.0f,%.0f,%.0f tgt=%.0f,%.0f,%.0f d=%.0f "
                          "arm=%.0f efft=%.3f pair=%08X cut=%d pairs=%d%c",
                          f, eyev[0], eyev[1], eyev[2], tgt[0], tgt[1], tgt[2], d,
                          s_nd_dist * s_cam_eff_t, s_cam_eff_t, ph,
                          s_nots_cut_phase, s_pairset_curn, 10);
                  pe[0]=eyev[0]; pe[1]=eyev[1]; pe[2]=eyev[2]; have = 1;
                  lastf = f;
              }
          } }

        { float look[3] = { eyev[0] + dir[0]*1000.f, eyev[1] + dir[1]*1000.f, eyev[2] + dir[2]*1000.f };
          snd_cam_publish(eyev, look); }

        float fwd[3];
        if (nots_true_shoulder()) {
            fwd[0]=dir[0]; fwd[1]=dir[1]; fwd[2]=dir[2];
        } else {
            fwd[0]=tgt[0]-eyev[0]; fwd[1]=tgt[1]-eyev[1]; fwd[2]=tgt[2]-eyev[2];
            v3norm(fwd);
        }
        float rt[3]={ fwd[2], 0.f, -fwd[0] };
        if (rt[0]*rt[0] + rt[2]*rt[2] < 1e-8f) {
            rt[0]=rtl[0]; rt[1]=0.f; rt[2]=rtl[2];
        }
        v3norm(rt);
        float upc[3]; v3cross(fwd, rt, upc);
        float Np[9]={ rt[0],rt[1],rt[2], upc[0],upc[1],upc[2], fwd[0],fwd[1],fwd[2] };

        {
            static int mtr=-1;
            if (mtr<0){ const char*e=getenv("DC_NOTS_MONTRACE"); mtr=(e&&*e=='1')?1:0; }
            if (mtr) {
                static unsigned long long mlf=~0ull;
                unsigned long long mq=(unsigned long long)s_frame_count;
                if (mq != mlf) { mlf=mq;
                    int lw=0, age=99999; dc_ots_last_yaw_write(&lw, &age);
                    fprintf(stderr, "montrace: f=%llu raw=%.0f,%.0f ry=%d flt=%.0f,%.0f fy=%d far=%d lw=%d age=%d eye=%.0f,%.0f,%.0f oyaw=%.2f\n",
                            mq, rpx, rpz, ryaw, px, pz, (int)yws, s_nots_gate_far,
                            lw, age, eyev[0], eyev[1], eyev[2], s_nots_oyaw);
                }
            }
        }

        float camyaw = s_nots_oyaw;
        { static int scr = -1;
          if (scr < 0) { const char* e = getenv("DC_OTS_SCRCOURSE"); scr = (e && *e == '0') ? 0 : 1; }

          if (scr && (fwd[0]*fwd[0] + fwd[2]*fwd[2]) > 1e-4f)
              camyaw = atan2f(fwd[0], fwd[2]); }
        s_nots_cam = camyaw; s_nots_her = wt + corr; s_nots_her_yaw = (int)yws; s_nots_ready = 1;
        s_nots_oyaw_pub = s_nots_oyaw; s_nots_oyaw_pub_have = 1;
        { extern float g_dcns_cam, g_dcns_her; extern int g_dcns_her_yaw;
          g_dcns_cam = camyaw; g_dcns_her = wt + corr; g_dcns_her_yaw = (int)yws; }
        { extern uint64_t s_frame_count; s_nots_ready_vb = s_frame_count; }

        float Ml[9];
        Ml[0]=(int16_t)(c[0]&0xFFFF); Ml[1]=(int16_t)(c[0]>>16); Ml[2]=(int16_t)(c[1]&0xFFFF);
        Ml[3]=(int16_t)(c[1]>>16);    Ml[4]=(int16_t)(c[2]&0xFFFF); Ml[5]=(int16_t)(c[2]>>16);
        Ml[6]=(int16_t)(c[3]&0xFFFF); Ml[7]=(int16_t)(c[3]>>16);   Ml[8]=(int16_t)(c[4]&0xFFFF);
        for(int i=0;i<9;i++) Ml[i]/=4096.0f;
        float TRo[3]={ (float)(int32_t)c[5], (float)(int32_t)c[6], (float)(int32_t)c[7] };

        if (dc_nots_pairsrc_site()) {
            float tgx = (float)(int16_t)(ram[mphys+0x30] | (ram[mphys+0x31]<<8));
            float tgy = (float)(int16_t)(ram[mphys+0x32] | (ram[mphys+0x33]<<8));
            float tgz = (float)(int16_t)(ram[mphys+0x34] | (ram[mphys+0x35]<<8));
            float Tp[3] = {
                -(Ml[0]*tgx + Ml[1]*tgy + Ml[2]*tgz) + TRo[0],
                -(Ml[3]*tgx + Ml[4]*tgy + Ml[5]*tgz) + TRo[1],
                -(Ml[6]*tgx + Ml[7]*tgy + Ml[8]*tgz) + TRo[2]
            };

            { static int pd = -1;
              if (pd < 0) { const char *e2 = getenv("DC_PAIR_DBG"); pd = (e2 && *e2 == '1') ? 1 : 0; }
              if (pd) { extern uint64_t s_frame_count;
                  fprintf(stderr, "pairdbg f=%llu m=%05X tg=%.0f,%.0f,%.0f TRo=%.0f,%.0f,%.0f R0=%.4f%c",
                          (unsigned long long)s_frame_count, mphys, tgx, tgy, tgz,
                          TRo[0], TRo[1], TRo[2], Ml[0], 10); } }
            dc_nots_set_pair(Ml, Tp);
        }

        {
            static unsigned long long pdlast = 0;
            unsigned long long pdf = (unsigned long long)s_frame_count_ref();
            if (s_nots_domhave && pdf - pdlast > 300) {
                pdlast = pdf;

                float dz[3] = { -s_nots_domT[0], -s_nots_domT[1], TRo[2] - s_nots_domT[2] };
                float tg[3] = { Ml[0]*dz[0] + Ml[3]*dz[1] + Ml[6]*dz[2],
                                Ml[1]*dz[0] + Ml[4]*dz[1] + Ml[7]*dz[2],
                                Ml[2]*dz[0] + Ml[5]*dz[1] + Ml[8]*dz[2] };
                uint32_t sb = mphys;
                fprintf(stderr, "pairdiag: f=%llu site R0=%.2f,%.2f,%.2f D=%.0f "
                        "| hist T=%.0f,%.0f,%.0f | tgt=%.0f,%.0f,%.0f | s16@20:",
                        pdf, Ml[0], Ml[1], Ml[2], TRo[2],
                        s_nots_domT[0], s_nots_domT[1], s_nots_domT[2],
                        tg[0], tg[1], tg[2]);
                for (uint32_t o = 0x20; o < 0x38; o += 2)
                    fprintf(stderr, " %d", (int)(int16_t)(ram[sb+o] | (ram[sb+o+1]<<8)));
                fprintf(stderr, "\n");
            }
        }

        if (!s_nots_domhave) return;
        float* Pc = s_nots_domR; float* Pt = s_nots_domT;

        static float Gc[9]={1,0,0,0,1,0,0,0,1}; static float Gt[3]={0,0,0};
        static int gch=0;
        {
            float n0=Pc[0]*Pc[0]+Pc[1]*Pc[1]+Pc[2]*Pc[2];
            float n1=Pc[3]*Pc[3]+Pc[4]*Pc[4]+Pc[5]*Pc[5];
            if (n0>0.85f && n0<1.15f && n1>0.85f && n1<1.15f) {
                for(int i=0;i<9;i++) Gc[i]=Pc[i];
                for(int i=0;i<3;i++) Gt[i]=Pt[i];
                gch=1;
            }
        }
        if (!gch) return;
        Pc = Gc; Pt = Gt;
        if (nots_cutdbg()) {
            static unsigned long long clf = ~0ull;
            if (fseq != clf) { clf = fseq;
                fprintf(stderr, "cuthold: f=%llu CONSUME T=%.0f,%.0f,%.0f (phase%d) p=%.0f,%.0f,%.0f eye=%.0f,%.0f,%.0f bb=%.0f,%.0f/%.0f,%.0f Pc0=%.2f,%.2f,%.2f yaw=%d wt=%.2f corr=%.2f\n",
                        (unsigned long long)fseq, Pt[0], Pt[1], Pt[2], s_nots_cut_phase,
                        px, py, pz, eyev[0], eyev[1], eyev[2],
                        s_nots_bbhave ? s_nots_bbmin[0] : -1.f, s_nots_bbhave ? s_nots_bbmin[2] : -1.f,
                        s_nots_bbhave ? s_nots_bbmax[0] : -1.f, s_nots_bbhave ? s_nots_bbmax[2] : -1.f,
                        Pc[0], Pc[1], Pc[2], (int)yws, wt, corr); }
        }

        {
            static int arm=-1; static unsigned long long armf=~0ull; static int lines=0;
            if (armf!=fseq){ armf=fseq;
                int a=dc_nots_dump_armed();
                if (a && !arm) lines=0;
                arm=a; }
            if (arm && lines<8000){ lines++;
                FILE* df=fopen("nots_dbg2.txt","a");
                if (df){ fprintf(df,"ld f=%llu m=%05X M=%.2f,%.2f,%.2f TR=%.0f,%.0f,%.0f pair=%.2f,%.2f,%.2f|%.0f,%.0f,%.0f\n",
                        fseq,mphys,Ml[0],Ml[1],Ml[2],TRo[0],TRo[1],TRo[2],
                        Pc[0],Pc[1],Pc[2],Pt[0],Pt[1],Pt[2]); fclose(df); } }
        }
        static int dbg=-1; if(dbg<0){ const char*e=getenv("DC_NOTS_DEBUG"); dbg=(e&&*e=='1')?1:0; }
        if (dbg) { static unsigned long long dlf=~0ull; if(fseq!=dlf && (fseq%30u)==0u){ dlf=fseq;
            float eg[3]={ -(Pc[0]*Pt[0]+Pc[3]*Pt[1]+Pc[6]*Pt[2]),
                          -(Pc[1]*Pt[0]+Pc[4]*Pt[1]+Pc[7]*Pt[2]),
                          -(Pc[2]*Pt[0]+Pc[5]*Pt[1]+Pc[8]*Pt[2]) };
            FILE* df=fopen("nots_dbg2.txt","a");
            if (df) { fprintf(df,"nots f=%llu yaw=%d oyaw=%.2f wt=%.2f corr=%.2f p=%.0f,%.0f,%.0f "
                    "eyev=%.0f,%.0f,%.0f eyeg=%.0f,%.0f,%.0f Pt=%.0f,%.0f,%.0f Pc=%.2f,%.2f,%.2f|%.2f,%.2f,%.2f|%.2f,%.2f,%.2f\n",
                    fseq,(int)yws,s_nots_oyaw,wt,corr,px,py,pz,
                    eyev[0],eyev[1],eyev[2],eg[0],eg[1],eg[2],Pt[0],Pt[1],Pt[2],
                    Pc[0],Pc[1],Pc[2],Pc[3],Pc[4],Pc[5],Pc[6],Pc[7],Pc[8]); fclose(df); } } }

        float PcI[9];
        {
            static int pinv = -1;
            if (pinv < 0) { const char *e = getenv("DC_PAIR_INV"); pinv = (e && *e == '0') ? 0 : 1; }
            double a0=Pc[0],a1=Pc[1],a2=Pc[2],a3=Pc[3],a4=Pc[4],a5=Pc[5],a6=Pc[6],a7=Pc[7],a8=Pc[8];
            double det = a0*(a4*a8-a5*a7) - a1*(a3*a8-a5*a6) + a2*(a3*a7-a4*a6);
            if (pinv && fabs(det) > 1e-3) {
                double id = 1.0/det;
                PcI[0]=(float)(( a4*a8-a5*a7)*id); PcI[1]=(float)((-a1*a8+a2*a7)*id); PcI[2]=(float)(( a1*a5-a2*a4)*id);
                PcI[3]=(float)((-a3*a8+a5*a6)*id); PcI[4]=(float)(( a0*a8-a2*a6)*id); PcI[5]=(float)((-a0*a5+a2*a3)*id);
                PcI[6]=(float)(( a3*a7-a4*a6)*id); PcI[7]=(float)((-a0*a7+a1*a6)*id); PcI[8]=(float)(( a0*a4-a1*a3)*id);
            } else {
                PcI[0]=Pc[0]; PcI[1]=Pc[3]; PcI[2]=Pc[6];
                PcI[3]=Pc[1]; PcI[4]=Pc[4]; PcI[5]=Pc[7];
                PcI[6]=Pc[2]; PcI[7]=Pc[5]; PcI[8]=Pc[8];
            }
        }
        float eyeg[3]={
            -(PcI[0]*Pt[0]+PcI[1]*Pt[1]+PcI[2]*Pt[2]),
            -(PcI[3]*Pt[0]+PcI[4]*Pt[1]+PcI[5]*Pt[2]),
            -(PcI[6]*Pt[0]+PcI[7]*Pt[1]+PcI[8]*Pt[2]) };

        {

            extern uint64_t gpu_flip_seq(void);
            static unsigned long long dpin = ~0ull;
            static int dgen = -1;
            unsigned long long fl = (unsigned long long)gpu_flip_seq();
            if (dpin == fl && dgen == s_nots_pair_gen) return;
            dpin = fl;
            dgen = s_nots_pair_gen;
        }

        {
            static unsigned long long nl = ~0ull;
            static int en2 = -1;
            if (en2 < 0) { const char *e2 = getenv("DC_D_NLATCH"); en2 = (e2 && *e2 == '0') ? 0 : 1; }
            extern uint64_t gpu_flip_seq(void);
            unsigned long long fl2 = (unsigned long long)gpu_flip_seq();
            if (!en2 || nl != fl2) {
                nl = fl2;
                for (int i = 0; i < 9; i++) s_dNp[i] = Np[i];
                for (int i = 0; i < 3; i++) s_deyev[i] = eyev[i];
                for (int i = 0; i < 3; i++) s_dtgt[i] = tgt[i];
                s_dNhave = 1;

                { static int pd = -1;
                  if (pd < 0) { const char *e4 = getenv("DC_PANDBG"); pd = (e4 && *e4 == '1') ? 1 : 0; }
                  if (pd) { extern uint64_t s_frame_count;
                      float lat_yaw = atan2f(-s_dNp[2], s_dNp[0]);
                      fprintf(stderr, "pandbg f=%llu vb=%llu lat_yaw=%.5f live_yaw=%.5f pit=%.4f eye=%.1f,%.1f,%.1f tgt=%.1f,%.1f,%.1f ryaw=%d act=%d sub=%d corr=%.4f efft=%.3f dist=%.0f%c",
                              fl2, (unsigned long long)s_frame_count, lat_yaw, s_nots_oyaw, s_nots_opitch,
                              s_deyev[0], s_deyev[1], s_deyev[2], tgt[0], tgt[1], tgt[2],
                              (int)(int16_t)(ram[0xB92B6] | (ram[0xB92B7] << 8)), ram[0xB92C9], ram[0xB92CA],
                              corr, s_cam_eff_t, s_nd_dist, 10); } }
            }

            { static int ph = -1;
              if (ph < 0) { const char *e3 = getenv("DC_PHASEDBG"); ph = (e3 && *e3 == '1') ? 1 : 0; }
              if (ph) fprintf(stderr, "phP f=%llu N0=%.4f eye=%.1f,%.1f,%.1f pair0=%.4f pairT=%.0f,%.0f,%.0f%c",
                              fl2, s_dNp[0], s_deyev[0], s_deyev[1], s_deyev[2],
                              Pc[0], Pt[0], Pt[1], Pt[2], 10); }
        }
        float D[9]; mat3_mul(s_dNp, PcI, D);
        float de[3]={ eyeg[0]-s_deyev[0], eyeg[1]-s_deyev[1], eyeg[2]-s_deyev[2] };
        float td[3]; mat3_vec(s_dNp, de, td);

        { static int pf = -1;
          if (pf < 0) { const char *e = getenv("DC_DP_PERFLIP"); pf = (e && *e == '0') ? 0 : 1; }
          extern uint64_t gpu_flip_seq(void);
          static unsigned long long dppin = ~0ull;
          unsigned long long fl2 = (unsigned long long)gpu_flip_seq();
          if (!pf || dppin != fl2) {
              dppin = fl2;
              if (s_nots_dhave && !dc_scripted_now() && !dc_scripted_edge()) {
                  for(int i=0;i<9;i++) s_nots_Dp[i]=s_nots_D[i];
                  for(int i=0;i<3;i++) s_nots_Tp[i]=s_nots_T[i];
                  s_nots_Dp_pass = s_nots_D_pass;
                  s_nots_dphave=1;
                  memcpy(s_wDp_N, s_wD_N, sizeof s_wDp_N); memcpy(s_wDp_eye, s_wD_eye, sizeof s_wDp_eye);
                  memcpy(s_wDp_tgt, s_wD_tgt, sizeof s_wDp_tgt); s_wDp_have = s_wD_have; s_wDp_flip = s_wD_flip;
                  s_wDp_cam = s_wD_cam; s_wDp_her = s_wD_her;                                                               } else { s_nots_dphave=0; s_wDp_have = 0; }
          } }
        for(int i=0;i<9;i++) s_nots_D[i]=D[i];
        for(int i=0;i<3;i++) s_nots_T[i]=td[i];
        s_nots_dhave=1;
        s_nots_D_pass = g_dc_pass_id;
        { extern uint64_t gpu_flip_seq(void);
          memcpy(s_wD_N, s_dNp, sizeof s_wD_N); memcpy(s_wD_eye, s_deyev, sizeof s_wD_eye); memcpy(s_wD_tgt, s_dtgt, sizeof s_wD_tgt);
          s_wD_have = 1; s_wD_flip = (unsigned long long)gpu_flip_seq();
          s_wD_cam = s_nots_cam; s_wD_her = s_nots_her_yaw; }
        camdbg_pair(D, td, "delta");
        { static int wdbg = -1;
          if (wdbg < 0) { const char *e = getenv("DC_CAMDBG"); wdbg = (e && *e == '1') ? 1 : 0; }
          if (wdbg) {
              float C[9];
              for (int r = 0; r < 3; r++)
                  for (int c2 = 0; c2 < 3; c2++)
                      C[r*3+c2] = D[r*3+0]*Pc[0*3+c2] + D[r*3+1]*Pc[1*3+c2] + D[r*3+2]*Pc[2*3+c2];
              float cc[3];
              for (int r = 0; r < 3; r++)
                  cc[r] = D[r*3+0]*Pt[0] + D[r*3+1]*Pt[1] + D[r*3+2]*Pt[2] + td[r];
              float ew[3] = { -(C[0]*cc[0] + C[3]*cc[1] + C[6]*cc[2]),
                              -(C[1]*cc[0] + C[4]*cc[1] + C[7]*cc[2]),
                              -(C[2]*cc[0] + C[5]*cc[1] + C[8]*cc[2]) };
              extern uint64_t s_frame_count;
              fprintf(stderr, "world f=%llu eye=%.0f,%.0f,%.0f fwd=%.3f,%.3f,%.3f%c",
                      (unsigned long long)s_frame_count, ew[0], ew[1], ew[2],
                      C[2], C[5], C[8], 10);
          } }
        (void)Ml; (void)TRo;
        return;
    }
}

static void mat3_mul(const float a[9], const float b[9], float o[9]) {
    for (int r=0;r<3;r++) for (int c=0;c<3;c++)
        o[r*3+c] = a[r*3]*b[c] + a[r*3+1]*b[3+c] + a[r*3+2]*b[6+c];
}
static void mat3_vec(const float m[9], const float v[3], float o[3]) {
    o[0]=m[0]*v[0]+m[1]*v[1]+m[2]*v[2];
    o[1]=m[3]*v[0]+m[4]*v[1]+m[5]*v[2];
    o[2]=m[6]*v[0]+m[7]*v[1]+m[8]*v[2];
}
static void v3cross(const float a[3], const float b[3], float o[3]) {
    o[0]=a[1]*b[2]-a[2]*b[1]; o[1]=a[2]*b[0]-a[0]*b[2]; o[2]=a[0]*b[1]-a[1]*b[0];
}
static void v3norm(float v[3]) {
    float l=sqrtf(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]); if(l<1e-6f)l=1.f;
    v[0]/=l; v[1]/=l; v[2]/=l;
}

static void dc_wwatch(uint32_t phys, uint32_t val, uint8_t width) {
    static int en = -1; static uint32_t wlo = 0, wlen = 0;
    if (en < 0) {
        const char *e = getenv("DC_WWATCH");
        if (e && *e) {
            wlo = (uint32_t)strtoul(e, NULL, 0);
            const char *c = strchr(e, ',');
            wlen = c ? (uint32_t)strtoul(c + 1, NULL, 0) : 4u;
            en = 1;
        } else en = 0;
    }
    if (!en || phys < wlo || phys >= wlo + wlen) return;
    uint32_t pc = g_debug_last_store_pc;

    { static int fo = -1;
      if (fo < 0) { const char *f = getenv("DC_WWATCH_FIRE"); fo = (f && *f == '1') ? 1 : 0; }
      if (fo) { extern int dc_fire_recent(void);
                extern int dc_real_shot_window(int frames);

                if (!dc_fire_recent() && !dc_real_shot_window(60)) return; } }

    { static int sn = -1; static uint32_t sk[8];
      if (sn < 0) { sn = 0; const char *s = getenv("DC_WWATCH_SKIP");
                    while (s && *s && sn < 8) { char *e2 = NULL;
                        sk[sn++] = (uint32_t)strtoul(s, &e2, 0);
                        if (e2 == s) break; s = e2;
                        while (*s == ',' || *s == ' ') s++; } }
      for (int i = 0; i < sn; i++) if (sk[i] == pc) return; }
    { static int all = -1;
      if (all < 0) { const char *a = getenv("DC_WWATCH_ALL"); all = (a && *a == '1') ? 1 : 0; }
      if (all) {
          static long n = 0;

          extern uint32_t g_debug_current_func_addr;
          extern CPUState *debug_cpu_ptr;
          uint32_t ra = debug_cpu_ptr ? debug_cpu_ptr->gpr[31] : 0;
          if (++n <= 2000)
              fprintf(stderr, "wwall phys=0x%06X val=0x%08X w=%u pc=0x%08X ra=0x%08X func=0x%08X%c",
                      phys, (unsigned)val, (unsigned)width, pc, ra,
                      (unsigned)g_debug_current_func_addr, 10);
          return;
      } }
    static uint32_t seen[256]; static int ns = 0;
    for (int i = 0; i < ns; i++) if (seen[i] == pc) return;
    if (ns < 256) {
        seen[ns++] = pc;
        fprintf(stderr, "wwatch phys=0x%06X val=0x%08X w=%u pc=0x%08X\n",
                phys, (unsigned)val, (unsigned)width, pc);
    }
}

static int s_spd_held = 0;
static unsigned long long s_spd_held_vb = 0;

static int s_spd_ramp_from = 0, s_spd_ramp_to = 0, s_spd_ramp_step = -1;
static unsigned long long s_spd_ramp_vb = 0;

static int dc_spd_ramp_n(void) {
    static int n = -1;
    if (n < 0) { const char *e = getenv("DC_SPD_RAMP_N"); n = e ? atoi(e) : 9;
                 if (n < 2) n = 2; if (n > 30) n = 30; }
    return n;
}
static unsigned long long dc_spd_now(void) {
    extern uint64_t s_frame_count;
    return (unsigned long long)s_frame_count;
}
static int dc_speed_spike(uint32_t phys, uint32_t val, uint8_t width)
{

    if (phys == 0xB92D0u && (width == 2 || width == 4)) {
        extern int dc_strafe_speed_scale(int v, int *out);
        int nv = 0;
        if (dc_strafe_speed_scale((int)(int16_t)(val & 0xFFFFu), &nv)) {
            if (nv > 32767) nv = 32767; if (nv < -32768) nv = -32768;
            ram[0xB92D0] = (uint8_t)(nv & 0xFF); ram[0xB92D1] = (uint8_t)((nv >> 8) & 0xFF);
            if (width == 4) { ram[0xB92D2] = (uint8_t)((val >> 16) & 0xFF); ram[0xB92D3] = (uint8_t)((val >> 24) & 0xFF); }
            return 1;
        }
    }
    static int en = -1;
    if (en < 0) { const char *e = getenv("DC_SPEED_SPIKE"); en = (e && *e == '0') ? 0 : 1; }
    if (!en) return 0;
    if (phys != 0xB92D0u || (width != 2 && width != 4)) return 0;

    if (ram[0xB92C8] != 1) return 0;
    { extern int dc_cut_active(void); if (dc_cut_active()) return 0; }

    { extern int dc_m5_speed_scale(int v, int ov, int *out);
      int nv5 = 0;
      if (dc_m5_speed_scale((int)(int16_t)(val & 0xFFFFu), (int)(int16_t)(ram[0xB92D0] | (ram[0xB92D1] << 8)), &nv5)) {
          if (nv5 > 32767) nv5 = 32767; if (nv5 < -32768) nv5 = -32768;
          ram[0xB92D0] = (uint8_t)(nv5 & 0xFF); ram[0xB92D1] = (uint8_t)((nv5 >> 8) & 0xFF);
          if (width == 4) { ram[0xB92D2] = (uint8_t)((val >> 16) & 0xFF); ram[0xB92D3] = (uint8_t)((val >> 24) & 0xFF); }
          return 1;
      } }

    int nv = (int)(int16_t)(val & 0xFFFFu);
    int ov = (int)(int16_t)(ram[0xB92D0] | (ram[0xB92D1] << 8));
    static int blocked_last = 0;

    if (!blocked_last && nv >= 120 && ov <= 60) {
        blocked_last = 1;
        s_spd_ramp_step = -1;
        s_spd_held = nv;
        s_spd_held_vb = dc_spd_now();
        static long n = 0;
        if (++n <= 8)
            fprintf(stderr, "spdspike: refused %d -> %d (transition frame)%c", ov, nv, 10);
        return 1;
    }

    {
        static int wr = -1;

        if (wr < 0) { const char *e = getenv("DC_WALK_RAMP"); wr = (e && *e == '1') ? 1 : 0; }
        if (wr) {

            static unsigned long long s_spd_edge_vb = 0;
            unsigned long long now_vb = dc_spd_now();
            int rapid = (s_spd_edge_vb && now_vb - s_spd_edge_vb < 20);
            s_spd_edge_vb = now_vb;
            if (rapid) {
                if (s_spd_ramp_step >= 0) s_spd_ramp_step = -1;
            } else
            if (ov == 0 && nv >= 30 && nv < 120) {
                s_spd_ramp_from = 0; s_spd_ramp_to = nv;
                s_spd_ramp_step = 0; s_spd_ramp_vb = dc_spd_now();
                blocked_last = 0; s_spd_held = 0;
                return 1;
            }
            if (ov >= 30 && ov < 120 && nv == 0) {
                s_spd_ramp_from = ov; s_spd_ramp_to = 0;
                s_spd_ramp_step = 0; s_spd_ramp_vb = dc_spd_now();
                blocked_last = 0; s_spd_held = 0;
                return 1;
            }
            if (s_spd_ramp_step >= 0) s_spd_ramp_step = -1;
        }
    }
    blocked_last = 0;
    s_spd_held = 0;
    return 0;
}

void dc_speed_spike_tick(void)
{

    if (s_spd_ramp_step >= 0) {
        if (ram[0xB92C8] != 1) { s_spd_ramp_step = -1; }
        else if (dc_spd_now() != s_spd_ramp_vb) {
            s_spd_ramp_vb = dc_spd_now();
            s_spd_ramp_step++;
            int N = dc_spd_ramp_n();
            int t = s_spd_ramp_step; if (t > N) t = N;

            float tf = (float)t / (float)N;
            float ef = tf * tf * (3.f - 2.f * tf);
            int v = s_spd_ramp_from + (int)((float)(s_spd_ramp_to - s_spd_ramp_from) * ef);
            ram[0xB92D0] = (uint8_t)(v & 0xFF);
            ram[0xB92D1] = (uint8_t)((v >> 8) & 0xFF);
            if (s_spd_ramp_step >= N) s_spd_ramp_step = -1;
        }
    }
    if (!s_spd_held) return;
    if (dc_spd_now() - s_spd_held_vb < 4) return;
    int cur = (int)(int16_t)(ram[0xB92D0] | (ram[0xB92D1] << 8));
    if (cur <= 60) {
        ram[0xB92D0] = (uint8_t)(s_spd_held & 0xFF);
        ram[0xB92D1] = (uint8_t)((s_spd_held >> 8) & 0xFF);
        static long n = 0;
        if (++n <= 8)
            fprintf(stderr, "spdspike: no follow-up write - restored %d%c", s_spd_held, 10);
    }
    s_spd_held = 0;
}

uint32_t g_ot_lo = 0xFFFFFFFFu, g_ot_hi = 0;
int      g_ot_watch = 0;
long     g_ot_late_writes = 0;


int dc_memory_policy_write(uint32_t phys, uint32_t *value, uint32_t width) {
    if (!ram || !value || (width != 1 && width != 2 && width != 4) ||
        phys >= RAM_SIZE || width > RAM_SIZE - phys) policy_fail("memory: invalid game write span");
    uint32_t val = *value;
    if (width == 4) dc_wwatch(phys, val, 4);
    if (dc_drawlist_on()) {
        extern void dc_drawlist_watch(uint32_t, uint32_t, int);
        dc_drawlist_watch(phys, val, (int)width);
    }
    if (width == 2) dc_wwatch(phys, val, 2);
    if (width != 1 && dc_speed_spike(phys, val, (uint8_t)width)) return 0;
    if (width == 2 && phys >= 0xB7ACCu) val = dc_focus_damage_half(phys, (uint16_t)val);
    if (dc_ammo_range(phys, width)) {
        extern void dc_ammo_write_note(uint32_t, int, int);
        for (uint32_t i = 0; i < width; ++i)
            dc_ammo_write_note(phys + i, ram[phys + i], (val >> (i * 8)) & 0xFF);
    }
    dc_secvis_watch(phys, val, width);
    *value = val;
    return 1;
}
