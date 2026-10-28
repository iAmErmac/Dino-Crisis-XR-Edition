#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu_state.h"
#include "mod_runtime.h"
#include "parity_trace.h"
#include "psx_scheduler.h"
#include "test_process.h"
#ifdef DC_TEST_CALLS
#include "dc_call_state.h"
static unsigned animation_calls;
void pt_anim_frame(CPUState *cpu, uint32_t src, uint32_t dst, uint32_t bones)
{
    (void)cpu;
    ++animation_calls;
    assert(src == (animation_calls == 1 ? 0xBB01u : 0xAA01u));
    assert(dst == src + 1 && bones == src + 2);
}
#endif
#ifdef DC_TEST_BLOOD
#include "dc_call_state.h"
static uint32_t blood_seed = 0x1234;
static int blood_count = 3, blood_room = 1;
static unsigned seed_restores, pool_checks, forgotten;
int dc_blood_mult(void) { return blood_count; }
int dc_blood_pool_room(void) { ++pool_checks; return blood_room; }
uint32_t dc_rng_seed_get(void) { return blood_seed; }
void dc_rng_seed_set(uint32_t seed) { blood_seed = seed; ++seed_restores; }
void psx_pgxp_forget_register(uint32_t reg) { assert(reg == 4 || reg == 31); ++forgotten; }
#endif

#ifdef DC_TEST_POSE
#include "dc_call_state.h"
static DcDrawPose current_pose;
static unsigned pose_restores;
void dc_ladder_draw_enter(CPUState *cpu)
{
    memset(&current_pose, 0, sizeof current_pose);
    current_pose.gapplied = 1;
    current_pose.ladder_applied = 1;
    for (unsigned i = 0; i < 16; ++i)
        current_pose.gsaved[1][2][i] = (int16_t)(cpu->gpr[4] + i);
    current_pose.ladder_saved[2] = (int16_t)cpu->gpr[4];
}
void dc_ladder_draw_capture(DcDrawPose *pose) { *pose = current_pose; }
void dc_ladder_draw_restore(CPUState *cpu, const DcDrawPose *pose)
{
    (void)cpu;
    uint16_t expected = pose_restores ? 0xAA01 : 0xBB01;
    assert(pose->gapplied && pose->ladder_applied);
    assert((uint16_t)pose->ladder_saved[2] == expected);
    for (unsigned i = 0; i < 16; ++i)
        assert((uint16_t)pose->gsaved[1][2][i] == (uint16_t)(expected + i));
    ++pose_restores;
}
#endif

#ifdef DC_TEST_MESH
#include "dc_call_state.h"
static unsigned mesh_forgets;
void psx_pgxp_forget_register(uint32_t reg) { assert(reg == 4 || reg == 5 || reg == 31); ++mesh_forgets; }
void dc_meshrep_attach_runs(uint32_t header, DcMeshRuns *runs)
{
    memset(runs, 0, sizeof *runs);
    runs->base[0] = 0x10000; runs->base[1] = 0x20000;
    if (header == 1) {
        runs->count[0] = 8; runs->count[1] = 4;
        runs->cut[0] = (1ull << 1) | (1ull << 2) | (1ull << 5);
        runs->cut[1] = (1ull << 0) | (1ull << 3);
    } else if (header == 2) { runs->count[0] = 66; runs->cut[0] = UINT64_MAX; }
    else if (header == 3) { runs->count[0] = 64; runs->cut[0] = UINT64_MAX; }
    else assert(!"unexpected mesh header");
}
#endif

#ifdef DC_TEST_GRENADE
#include "dc_call_state.h"
static uint32_t grenade_request;
static unsigned grenade_completions;
uint32_t dc_grenade_take(void) { uint32_t object = grenade_request; grenade_request = 0; return object; }
void dc_grenade_complete(uint32_t object)
{
    assert(object == (grenade_completions ? 0x12000u : 0x13000u)); ++grenade_completions;
}
#endif

#ifdef DC_TEST_KERNEL
#include "dc_kernel.h"
#include "dc_call_state.h"
enum { PCB = 0xA0000000u + HLE_PCB_ADDR, TCB_A = 0xA0000000u + HLE_TCB_ADDR,
       TCB_B = TCB_A + 0xC0, PC_A = 0x2100, PC_B = 0x2200 };
#else
enum { PCB = 0xE000, TCB_A = 0xE100, TCB_B = 0xE1C0, PC_A = 0x2100, PC_B = 0x2200 };
#endif
#if defined(DC_TEST_SOUND) || defined(DC_TEST_SHOT) || defined(DC_TEST_CREATURE_BLOOD)
static uint32_t ram[0x200000 / 4];
#else
static uint32_t ram[0x10000 / 4];
#endif
#ifdef DC_TEST_SOUND
#include "dc_call_state.h"
static unsigned sound_notes, sound_prepares, capture_changes;
static int capture_page, sound_available = 1;
enum { SOUND_ENTRY = 0x1FEA00, SOUND_TABLE = 0x9E538 };
uint8_t *memory_get_ram_ptr(void) { return (uint8_t *)ram; }
void spu_xpage_capture(int page)
{
    assert(page == 0 || page == 3); capture_page = page; ++capture_changes;
}
int dc_fk_sound_prepare(CPUState *cpu, DcSoundSwap *swap)
{
    (void)cpu; ++sound_prepares;
    if (!sound_available) return 0;
    swap->entry_address = SOUND_ENTRY; swap->table_address = SOUND_TABLE;
    swap->entry_word = ram[SOUND_ENTRY / 4]; swap->table_word = ram[SOUND_TABLE / 4];
    swap->page = 3;
    ram[SOUND_ENTRY / 4] = 0xAABBCCDD; ram[SOUND_TABLE / 4] = 0xEEFF0011;
    dc_sound_capture_page(swap->page);
    return 1;
}
void dc_shot_effect_note(uint32_t actor)
{
    assert(actor == (sound_notes == 1 ? 0xBB01 : 0xAA01)); ++sound_notes;
}
#endif
#ifdef DC_TEST_SHOT
#include "dc_call_state.h"
uint8_t *memory_get_ram_ptr(void) { return (uint8_t *)ram; }
uint64_t g_aim_hit_vb;
unsigned long long g_shot_log_vb, g_shot_victim_frame, g_shot_turn_vb;
int g_presdump_req, g_shot_victim = -1, g_shot_victims[3], g_shot_turn_nv;
float g_dc_cam_eye[3], g_aim_hit_pt[3];
uint32_t g_aim_hit_actor;
static uint32_t shot_seed = 0x1234;
static unsigned shot_seed_restores;
static int shot_room = 1, shot_multiplier = 2;
int dc_nots_active(void) { return 1; }
int dc_cut_camera(void) { return 0; }
int dc_aim_turning(float *error) { *error = 0; return 0; }
uint32_t dc_aim_geom_first(float *depth) { *depth = 1000; return 0xB7ACC; }
void dc_nots_cam_get(float *yaw, float *pitch, float *distance) { *yaw = *pitch = *distance = 0; }
uint32_t dc_rng_seed_get(void) { return shot_seed; }
void dc_rng_seed_set(uint32_t seed) { shot_seed = seed; ++shot_seed_restores; }
int dc_blood_mult(void) { return shot_multiplier; }
int dc_blood_pool_room(void) { return shot_room; }
#endif
static unsigned dispatches, cleanup_count, yields;
static unsigned host_call_depth, callback_restores, discarded_threads;
static CPUState *active_cpu;
uint64_t s_frame_count;
int g_psx_dispatch_depth, psx_in_device_service, g_call_unit_depth;
int g_psx_cyc_bb_defer;
uint32_t g_psx_cyc_batch;
uint32_t *g_psx_cyc_local_acc;

static uint32_t read_word(uint32_t address)
{
    address &= 0x1FFFFFFF;
    assert(address < sizeof ram && (address & 3) == 0);
    return ram[address / 4];
}

static void write_word(uint32_t address, uint32_t value)
{
    address &= 0x1FFFFFFF;
    assert(address < sizeof ram && (address & 3) == 0);
    ram[address / 4] = value;
}

#ifdef DC_TEST_KERNEL
void psx_dispatch_call(CPUState *cpu, uint32_t pc, uint32_t stop)
{ (void)cpu; (void)pc; (void)stop; abort(); }
uint32_t psx_read_word(uint32_t address) { return read_word(address); }
void psx_write_word(uint32_t address, uint32_t value) { write_word(address, value); }
static void kernel_call(CPUState *cpu, uint32_t call, uint32_t argument)
{
    cpu->gpr[9] = call;
    cpu->gpr[4] = argument;
    assert(hle_bios_dispatch(cpu, HLE_VEC_B0, HLE_VEC_B0));
}
#endif

int psx_get_in_exception(void) { return 0; }
void psx_irq_discard_thread(uint32_t tcb)
{
    assert(tcb == TCB_A || tcb == TCB_B);
    ++discarded_threads;
}
int overlay_loader_shadow_native_thread_switch_bail(void) { return 0; }
void overlay_loader_shadow_scheduler_escape_fixup(void) { ++cleanup_count; }
void psx_snapshot_host_call_reset(void) { host_call_depth = 0; }
void mod_runtime_function_entry_context_restore(const ModFunctionEntryContext *context)
{
    assert(context != NULL);
    ++callback_restores;
}
void psx_irq_refresh_cause_ip2(void) { active_cpu->cop0[13] &= ~0x400u; }
void psx_irq_arm_compiled_resume_pc(uint32_t pc) { assert(pc != 0); }
int psx_netplay_rb_recover_null_pc(CPUState *cpu, uint32_t *pc)
{
    (void)cpu; (void)pc; abort();
}
void debug_server_log_thread_event(uint32_t kind, CPUState *cpu,
                                   uint32_t current, uint32_t target, uint32_t pc)
{
    (void)cpu; (void)current; (void)target; (void)pc;
    if (kind == 8) ++yields;
}
int parity_trace_is_armed(void) { return 0; }
void parity_trace_record(parity_kind_t kind, uint32_t pc, uint32_t ra,
                         uint32_t sp, uint32_t target,
                         uint32_t (*read)(void *, uint32_t), void *context)
{
    (void)kind; (void)pc; (void)ra; (void)sp; (void)target; (void)read; (void)context;
    abort();
}

#ifdef DC_TEST_CREATURE_BLOOD
#include "dc_creature_blood.h"
#include "dc_call_state.h"
#include "dc_continuation_wire.h"
static uint32_t creature_seed = 0x1234, creature_writes;
volatile int g_dc_standdown;
void dc_kernel_event_result(int hit) { (void)hit; abort(); }
void dc_kernel_card_arm(uint32_t delay, uint32_t spec) { (void)delay; (void)spec; abort(); }
uint32_t g_debug_last_store_pc;
uint8_t *memory_get_ram_ptr(void) { return (uint8_t *)ram; }
int dc_actor_model_bones(const uint8_t *data, uint32_t actor) {
    assert(data == (uint8_t *)ram && actor >= 0xB7ACCu); return 22;
}
int dc_recent_shot_victim(int slot) { assert(slot >= 0 && slot < 2); return 1; }
int dc_blood_mult(void) { return 2; }
int dc_blood_pool_room(void) { return 1; }
uint32_t dc_rng_seed_get(void) { return creature_seed; }
void dc_rng_seed_set(uint32_t seed) { creature_seed = seed; }
static void creature_write(uint32_t address, uint16_t value) {
    uint32_t phys = address & 0x1FFFFFFFu;
    assert(phys < sizeof ram && !(phys & 1u));
    memcpy((uint8_t *)ram + phys, &value, 2); ++creature_writes;
}
static void creature_begin(CPUState *cpu, uint32_t pc, uint32_t actor) {
    uint16_t hp = 100; memcpy((uint8_t *)ram + actor + 280, &hp, 2);
    cpu->write_half = creature_write;
    assert(dc_creature_blood_defer(cpu, pc, 0xA4800118u, 0x80000000u | (actor + 280), 70));
    assert(!dc_creature_blood_defer(cpu, pc + 4, 0xA4800118u, actor + 280, 60));
    assert(dc_creature_blood_flush(cpu, 0x80018000u));
    assert(cpu->pc == 0x8002E7CCu && cpu->gpr[31] == pc && cpu->gpr[4] == (0x80000000u | actor));
    assert(!dc_creature_blood_flush(cpu, 0x80019000u));
    memcpy(&hp, (uint8_t *)ram + actor + 280, 2); assert(hp == 100);
}
static void creature_finish(CPUState *cpu, uint32_t pc, uint32_t actor) {
    assert(dc_creature_blood_pending(cpu, pc));
    uint32_t before = creature_writes;
    creature_seed = 0xABCD;
    assert(dc_creature_blood_resume(cpu, pc));
    assert(cpu->pc == 0x8002E7CCu && creature_writes == before);
    cpu->gpr[16] = 0xBAD; cpu->hi = 0xBAD; cpu->lo = 0xBAD;
    assert(dc_creature_blood_resume(cpu, pc));
    uint16_t hp; memcpy(&hp, (uint8_t *)ram + actor + 280, 2);
    assert(hp == 70 && creature_writes == before + 1 && cpu->pc == 0x80018000u);
    assert(!dc_creature_blood_resume(cpu, pc) && creature_writes == before + 1);
}
#endif

void psx_dispatch(CPUState *cpu, uint32_t pc)
{
    ++dispatches;
    if (dispatches == 1) {
        assert(pc == PC_A && cpu->gpr[16] == 0xAAAA);
#ifdef DC_TEST_MESH
        assert(dc_mesh_call_begin(cpu, 1, 0x3000, 0x4000, 0x5000, 0x6000) == 0x3000);
        assert(cpu->gpr[4] == 0x10000 && cpu->gpr[5] == 1 && cpu->gpr[31] == 0x4000);
        assert(dc_mesh_call_pending(cpu, 0x6000)); cpu->gpr[25] = 0x1234;
#endif

#ifdef DC_TEST_SOUND
        ram[SOUND_ENTRY / 4] = 0x11223344; ram[SOUND_TABLE / 4] = 0x55667788;
        cpu->gpr[31] = PC_A + 4;
        dc_sound_call_begin(cpu);
        assert(capture_page == 3 && dc_call_sound_active() && sound_notes == 1);
#endif

#ifdef DC_TEST_BLOOD
        cpu->gpr[31] = PC_A + 4;
        assert(dc_blood_call_begin(cpu, PC_A + 4));
        blood_seed = 0x2222;
        cpu->gpr[4] = 0xCAFE;
#endif
#ifdef DC_TEST_POSE
        dc_pose_call_begin(cpu, PC_A + 4);
#endif
#ifdef DC_TEST_CALLS
        dc_anim_call_begin(cpu, PC_A + 4);
        cpu->gpr[4] = cpu->gpr[5] = cpu->gpr[6] = 0xCAFE;
#endif
#ifdef DC_TEST_KERNEL
        write_word(TCB_B, 0x1000);
        cpu->gpr[5] = 0x12340;
        cpu->gpr[6] = 0x56780;
        kernel_call(cpu, 0x0E, PC_B);
        assert(cpu->gpr[2] == 0xFF000001);
        assert(read_word(TCB_B + 136) == PC_B);
        write_word(TCB_B + 8 + 16 * 4, 0xBBBB);
#endif
        assert(psx_sched_switch_thread(cpu, TCB_A) == 1);
        assert(psx_sched_switch_thread(cpu, TCB_A + 4) == 0);
        assert(psx_sched_switch_thread(cpu, TCB_B + 0xC0) == 0);
        write_word(TCB_B, 0x1000);
        assert(psx_sched_switch_thread(cpu, TCB_B) == 0);
        write_word(TCB_B, 0x4000);
        assert(read_word(PCB) == TCB_A);
        cpu->gpr[2] = 1;
        cpu->gpr[16] = 0xA123;
        cpu->gpr[31] = PC_A + 4;
        g_psx_dispatch_depth = 3;
        g_call_unit_depth = 2;
        psx_in_device_service = 1;
        host_call_depth = 2;
        cpu->hi = 0x1234;
        cpu->lo = 0x5678;
#ifdef DC_TEST_CREATURE_BLOOD
        creature_begin(cpu, PC_A + 4, 0xB7ACC);
#endif
#ifdef DC_TEST_SHOT
        uint8_t *data = memory_get_ram_ptr();
        data[0xB92C8] = 1; data[0xB7ACC + 48] = 1;
        data[0xB94CC] = 0x20;
        *(int16_t *)(data + 0xB7ACC + 280) = 100;
        *(int16_t *)(data + 0xB7ACC + 36) = 1000;
        *(uint32_t *)(data + 0xB7ACC + 156) = 0x80016000;
        *(uint16_t *)(data + 0x16000 + 20) = 20;
        s_frame_count = g_shot_log_vb = 100;
        dc_shot_call_begin(cpu, 0x800B928C, 0, PC_A + 4, 120, 0x3000, 0x4000);
        cpu->gpr[29] -= 120; cpu->pc = 0;
        assert(!dc_shot_call_resuming(cpu, PC_A + 4, 120));
        assert(dc_shot_call_step(cpu, PC_A + 4, 120) == 0x3000);
        assert(cpu->gpr[4] == 0x800B92AC && cpu->gpr[5] == 0x800B7AEC && cpu->gpr[6] == 5);
#endif
#ifdef DC_TEST_GRENADE
        grenade_request = 0x12000; cpu->pc = 0;
        assert(dc_grenade_entry(cpu, PC_A + 4, 0x3000) == 0x3000);
        assert(cpu->gpr[4] == 0x80012000 && cpu->gpr[5] == 1 && cpu->gpr[6] == 1);
        cpu->hi = 0xBAD; cpu->lo = 0xDAD; cpu->gpr[16] = 0x123;
#endif
#ifdef DC_TEST_KERNEL
        kernel_call(cpu, 0x10, 0xFF000001);
#else
        psx_sched_switch_thread(cpu, TCB_B);
#endif
        abort();
    }
    if (dispatches == 2) {
        assert(pc == PC_B && cpu->gpr[16] == 0xBBBB);
#ifdef DC_TEST_CREATURE_BLOOD
        assert(!dc_creature_blood_pending(cpu, PC_A + 4));
        creature_seed = 0x4321; creature_begin(cpu, PC_B + 4, 0xB7ACC + 608);
        creature_finish(cpu, PC_B + 4, 0xB7ACC + 608);
        assert(creature_seed == 0x4321 && cpu->gpr[16] == 0xBBBB);
#endif
#ifdef DC_TEST_SHOT
        dc_shot_call_begin(cpu, 0x80010000, 0, PC_A + 4, 120, 0x3000, 0x4000);
        cpu->gpr[29] -= 120;
        assert(!dc_shot_call_step(cpu, PC_A + 4, 120));
        assert(g_shot_log_vb == 100);
        cpu->gpr[29] += 120;
#endif

#ifdef DC_TEST_GRENADE
        grenade_request = 0x13000; cpu->pc = 0;
        assert(dc_grenade_entry(cpu, PC_A + 4, 0x3000) == 0x3000);
        cpu->gpr[4] = 0xDEAD; cpu->hi = 0xABC;
        assert(!dc_grenade_entry(cpu, PC_A + 4, 0x3000));
        assert(cpu->gpr[4] == 0xBB01 && cpu->gpr[16] == 0xBBBB && cpu->hi == 0);
        assert(grenade_completions == 1);
#endif

#ifdef DC_TEST_MESH
        assert(dc_mesh_call_begin(cpu, 2, 0x3000, 0x4000, 0x5000, 0x6000) == 0x3000);
        assert(cpu->gpr[4] == 0x10000 + 64 * 48 && cpu->gpr[5] == 2);
        assert(!dc_mesh_call_next(cpu, 0x6000) && !dc_mesh_call_pending(cpu, 0x6000));
#endif

#ifdef DC_TEST_SOUND
        cpu->gpr[31] = PC_A + 4;
        dc_sound_call_begin(cpu);
        assert(sound_prepares == 1 && sound_notes == 2);
        dc_sound_call_end(cpu, PC_A + 4);
        assert(capture_page == 3 && ram[SOUND_ENTRY / 4] == 0xAABBCCDD);
#endif

        assert(g_psx_dispatch_depth == 0 && g_call_unit_depth == 0);
        assert(psx_in_device_service == 0);
        assert(host_call_depth == 0 && callback_restores == 1);
        assert(read_word(TCB_A + 136) == PC_A + 4);
        assert(read_word(TCB_A + 8 + 2 * 4) == 1);
#ifdef DC_TEST_BLOOD
        cpu->gpr[31] = PC_A + 4;
        blood_count = 2;
        assert(dc_blood_call_begin(cpu, PC_A + 4));
        cpu->gpr[4] = 0xCAFE; blood_seed = 0x3333;
        assert(dc_blood_call_return(cpu, PC_A + 4));
        assert(cpu->gpr[4] == 0xBB01 && cpu->gpr[31] == PC_A + 4);
        blood_seed = 0x4444;
        assert(!dc_blood_call_return(cpu, PC_A + 4));
        assert(blood_seed == 0x2222 && seed_restores == 1);
        blood_seed = 0x5555;
        assert(!dc_blood_call_return(cpu, PC_A + 4) && blood_seed == 0x5555);
#endif
#ifdef DC_TEST_POSE
        dc_pose_call_begin(cpu, PC_A + 4);
        dc_pose_call_end(cpu, PC_A + 4);
#endif
#ifdef DC_TEST_CALLS
        dc_anim_call_begin(cpu, PC_A + 4);
        dc_anim_call_end(cpu, PC_A + 4);
#endif
#ifdef DC_TEST_KERNEL
        dc_call_begin(cpu, PC_B + 4, DC_CALL_ANIMATION)->values[0] = 0x1234;
#endif
        cpu->gpr[16] = 0xB123;
        cpu->gpr[31] = PC_B + 4;
#ifdef DC_TEST_KERNEL
        kernel_call(cpu, 0x10, 0xFF000000);
#else
        psx_sched_switch_thread(cpu, TCB_A);
#endif
        abort();
    }
    assert(dispatches == 3 && pc == PC_A + 4);
#ifdef DC_TEST_CREATURE_BLOOD
    DcCallState *saved = malloc(sizeof *saved), *invalid = malloc(sizeof *invalid);
    DcKernelEventState *events = calloc(1, sizeof *events); assert(saved && invalid && events);
    dc_call_capture(saved); events->version = DC_KERNEL_EVENT_VERSION; assert(dc_call_validate(saved));
    uint32_t bytes = dc_continuation_wire_bytes(); uint8_t *wire = malloc(bytes); assert(wire);
    assert(dc_continuation_wire_write(wire, bytes, saved, events));
    dc_call_reset(); assert(dc_continuation_wire_read(wire, bytes, saved, events));
    assert(dc_call_restore(saved));
    *invalid = *saved; invalid->frames[0].values[7] = 0; assert(!dc_call_restore(invalid));
    *invalid = *saved; invalid->frames[0].values[5] = invalid->frames[0].values[4]; assert(!dc_call_restore(invalid));
    creature_finish(cpu, PC_A + 4, 0xB7ACC);
    assert(creature_seed == 0x1234 && creature_writes == 2);
    assert(!dc_call_has_kind(DC_CALL_CREATURE_BLOOD));
    free(wire); free(events); free(invalid); free(saved);
#endif
#ifdef DC_TEST_SHOT
    DcCallState saved, invalid;
    dc_call_capture(&saved); dc_call_reset(); assert(dc_call_restore(&saved));
    invalid = saved; invalid.frames[0].shot.count = 4; assert(!dc_call_restore(&invalid));
    assert(dc_shot_call_resuming(cpu, PC_A + 4, 120));
    cpu->gpr[2] = 0; cpu->gpr[4] = 0xDEAD;
    assert(dc_shot_call_step(cpu, PC_A + 4, 120) == 0x4000);
    assert(cpu->gpr[4] == 0x800B7ACC && memory_get_ram_ptr()[0xB94D8] == 1);
    assert(g_shot_victim == 0 && !g_shot_log_vb && g_presdump_req == 1);
    shot_seed = 0x9999; cpu->gpr[16] = 0xBAD;
    assert(dc_shot_call_step(cpu, PC_A + 4, 120) == 0x4000);
    assert(cpu->gpr[16] == 0xBAD);
    shot_seed = 0x8888;
    assert(!dc_shot_call_step(cpu, PC_A + 4, 120));
    assert(shot_seed == 0x1234 && shot_seed_restores == 1 && cpu->gpr[16] == 0xA123);
    shot_seed = 0x7777;
    assert(!dc_shot_call_step(cpu, PC_A + 4, 120) && shot_seed == 0x7777 && shot_seed_restores == 1);
    cpu->gpr[29] += 120;
    shot_room = 0; g_shot_log_vb = s_frame_count;
    dc_shot_call_begin(cpu, 0x800B928C, 0, PC_A + 4, 120, 0x3000, 0x4000);
    cpu->gpr[29] -= 120;
    assert(!dc_shot_call_step(cpu, PC_A + 4, 120));
    assert(shot_seed == 0x7777 && shot_seed_restores == 2);
    cpu->gpr[29] += 120;
    shot_room = 1; shot_multiplier = 0; g_shot_log_vb = s_frame_count;
    dc_shot_call_begin(cpu, 0x800B928C, 0, PC_A + 4, 120, 0x3000, 0x4000);
    cpu->gpr[29] -= 120;
    assert(dc_shot_call_step(cpu, PC_A + 4, 120) == 0x4000);
    shot_seed = 0x4444;
    assert(!dc_shot_call_step(cpu, PC_A + 4, 120));
    assert(shot_seed == 0x7777 && shot_seed_restores == 3);
    cpu->gpr[29] += 120;
    uint8_t *data = memory_get_ram_ptr();
    data[0xB94CC] = 0x10; data[0xB94D1] = 0;
    data[0xB94D8] = 1; data[0xB94D9] = 1;
    data[0xB7D2C + 48] = 1;
    *(int16_t *)(data + 0xB7D2C + 280) = 100;
    *(uint32_t *)(data + 0xB7D2C + 156) = 0x80017000;
    *(uint16_t *)(data + 0x17000 + 20) = 10;
    g_shot_log_vb = s_frame_count;
    dc_shot_call_begin(cpu, 0x800B928C, 0, PC_A + 4, 120, 0x3000, 0x4000);
    cpu->gpr[29] -= 120;
    assert(dc_shot_call_step(cpu, PC_A + 4, 120) == 0x3000);
    cpu->gpr[2] = 0;
    assert(dc_shot_call_step(cpu, PC_A + 4, 120) == 0x4000 && cpu->gpr[4] == 0x800B7ACC);
    shot_seed = 0x1111;
    assert(dc_shot_call_step(cpu, PC_A + 4, 120) == 0x4000 && cpu->gpr[4] == 0x800B7D2C);
    assert(shot_seed == 0x7777 && data[0xB94D8] == 2);
    shot_seed = 0x2222;
    assert(!dc_shot_call_step(cpu, PC_A + 4, 120) && shot_seed == 0x7777 && shot_seed_restores == 5);
    cpu->gpr[29] += 120;
    memory_get_ram_ptr()[0xB94D8] = 0;
    dc_shot_call_begin(cpu, 0x800B928C, 0, PC_A + 4, 120, 0x3000, 0x4000);
    cpu->gpr[29] -= 120;
    assert(dc_shot_call_step(cpu, PC_A + 4, 120) == 0x3000);
    dc_call_discard_thread(TCB_A); dc_call_discard_thread(TCB_B);
    cpu->gpr[29] += 120;
#endif

#ifdef DC_TEST_GRENADE
    DcCallState saved, invalid;
    dc_call_capture(&saved); dc_call_reset(); assert(dc_call_restore(&saved));
    invalid = saved; invalid.frames[0].guest.words[29] ^= 4; assert(!dc_call_restore(&invalid));
    invalid = saved; invalid.frames[0].values[0] = 0x200000; assert(!dc_call_restore(&invalid));
    assert(!dc_grenade_entry(cpu, PC_A + 4, 0x3000));
    assert(cpu->gpr[4] == 0xAA01 && grenade_completions == 2);
    assert(!dc_grenade_entry(cpu, PC_A + 4, 0x3000) && grenade_completions == 2);
    grenade_request = 0x14000;
    assert(dc_grenade_entry(cpu, PC_A + 4, 0x3000) == 0x3000);
    dc_call_discard_thread(TCB_A); dc_call_discard_thread(TCB_B);
    assert(grenade_completions == 2);
#endif

#ifdef DC_TEST_MESH
    DcCallState saved, invalid;
    dc_call_capture(&saved); dc_call_reset(); assert(dc_call_restore(&saved));
    invalid = saved; invalid.frames[0].values[0] = 2; assert(!dc_call_restore(&invalid));
    invalid = saved; invalid.frames[0].mesh.count[0] = 0x10000; assert(!dc_call_restore(&invalid));
    invalid = saved; invalid.frames[0].mesh.base[0] = 0xFFFFFFF0; assert(!dc_call_restore(&invalid));
    invalid = saved; invalid.frames[0].values[2] |= 1; assert(!dc_call_restore(&invalid));
    assert(dc_mesh_call_next(cpu, 0x6000) == 0x3000);
    assert(cpu->gpr[4] == 0x10000 + 3 * 48 && cpu->gpr[5] == 2);
    assert(dc_mesh_call_next(cpu, 0x6000) == 0x3000);
    assert(cpu->gpr[4] == 0x10000 + 6 * 48 && cpu->gpr[5] == 2);
    assert(dc_mesh_call_next(cpu, 0x6000) == 0x5000);
    assert(cpu->gpr[4] == 0x20000 + 64 && cpu->gpr[5] == 2 && cpu->gpr[31] == 0x6000);
    assert(cpu->gpr[25] == 0x1234);
    assert(!dc_mesh_call_next(cpu, 0x6000) && !dc_mesh_call_pending(cpu, 0x6000));
    assert(mesh_forgets == 15);
    assert(!dc_mesh_call_begin(cpu, 3, 0x3000, 0x4000, 0x5000, 0x6000));
    assert(mesh_forgets == 15 && !dc_mesh_call_pending(cpu, 0x6000));
    dc_call_capture(&saved); dc_call_reset(); assert(dc_call_restore(&saved));
    assert(!dc_mesh_call_next(cpu, 0x6000));
    assert(dc_mesh_call_begin(cpu, 1, 0x3000, 0x4000, 0x5000, 0x6000) == 0x3000);
    dc_call_discard_thread(TCB_A); dc_call_discard_thread(TCB_B);
    assert(!dc_mesh_call_pending(cpu, 0x6000));
#endif

#ifdef DC_TEST_SOUND
    DcCallState saved, invalid;
    dc_call_capture(&saved);
    invalid = saved; invalid.frames[0].sound.page = 9;
    assert(!dc_call_restore(&invalid) && capture_page == 3);
    invalid = saved; invalid.frames[1] = invalid.frames[0]; invalid.frames[1].owner += 0xC0;
    assert(!dc_call_restore(&invalid));
    dc_call_reset();
    assert(!capture_page && ram[SOUND_ENTRY / 4] == 0x11223344);
    /* RAM is restored independently before the continuation metadata. */
    ram[SOUND_ENTRY / 4] = 0xAABBCCDD; ram[SOUND_TABLE / 4] = 0xEEFF0011;
    assert(dc_call_restore(&saved));
    assert(capture_page == 3 && ram[SOUND_ENTRY / 4] == 0xAABBCCDD);
    dc_sound_call_end(cpu, PC_A + 4);
    assert(!capture_page && ram[SOUND_ENTRY / 4] == 0x11223344 && ram[SOUND_TABLE / 4] == 0x55667788);
    unsigned changes = capture_changes;
    dc_call_capture(&saved); dc_call_reset(); assert(dc_call_restore(&saved));
    dc_sound_call_end(cpu, PC_A + 4); assert(capture_changes == changes);
    cpu->gpr[4] = 0xAA01; cpu->gpr[31] = PC_A + 8;
    dc_sound_call_begin(cpu); assert(capture_page == 3);
    dc_call_discard_thread(TCB_A);
    assert(!capture_page && !dc_call_sound_active() && ram[SOUND_ENTRY / 4] == 0x11223344);
    sound_available = 0; cpu->gpr[31] = PC_A + 12;
    dc_sound_call_begin(cpu); dc_sound_call_end(cpu, PC_A + 12);
    assert(!capture_page && !dc_call_sound_active() && ram[SOUND_ENTRY / 4] == 0x11223344);
#endif

#ifdef DC_TEST_POSE
    DcCallState saved, invalid;
    dc_call_capture(&saved); dc_call_reset(); assert(dc_call_restore(&saved));
    invalid = saved; invalid.frames[0].pose.gapplied = 2;
    assert(!dc_call_restore(&invalid));
    dc_pose_call_end(cpu, PC_A + 4); assert(pose_restores == 2);
    dc_call_capture(&saved); dc_call_reset(); assert(dc_call_restore(&saved));
    dc_pose_call_end(cpu, PC_A + 4); assert(pose_restores == 2);
    dc_call_discard_thread(TCB_A); dc_call_discard_thread(TCB_B);
    dc_call_capture(&saved);
    for (unsigned i = 0; i < DC_CALL_CAPACITY; ++i) assert(!saved.frames[i].kind);
#endif

#ifdef DC_TEST_BLOOD
    DcCallState saved;
    dc_call_capture(&saved); dc_call_reset(); assert(dc_call_restore(&saved));
    assert(dc_blood_call_return(cpu, PC_A + 4));
    assert(cpu->gpr[4] == 0xAA01 && cpu->gpr[31] == PC_A + 4);
    blood_seed = 0x6666; cpu->gpr[4] = 0xCAFE;
    assert(dc_blood_call_return(cpu, PC_A + 4));
    assert(cpu->gpr[4] == 0xAA01);
    blood_seed = 0x7777;
    assert(!dc_blood_call_return(cpu, PC_A + 4));
    assert(blood_seed == 0x1234 && seed_restores == 2 && forgotten == 6);
    blood_seed = 0x9999;
    assert(!dc_blood_call_return(cpu, PC_A + 4) && blood_seed == 0x9999);
    unsigned checks = pool_checks;
    cpu->gpr[31] = PC_A + 8; blood_count = 0;
    assert(!dc_blood_call_begin(cpu, PC_A + 8));
    assert(!dc_blood_call_return(cpu, PC_A + 8));
    assert(blood_seed == 0x9999 && pool_checks == checks);
    cpu->gpr[31] = PC_A + 12; blood_count = 1; blood_room = 0;
    assert(dc_blood_call_begin(cpu, PC_A + 12)); blood_seed = 1;
    assert(!dc_blood_call_return(cpu, PC_A + 12));
    assert(blood_seed == 0x9999 && pool_checks == checks);
    cpu->gpr[31] = PC_A + 16; blood_count = 5;
    assert(dc_blood_call_begin(cpu, PC_A + 16)); blood_seed = 2;
    assert(!dc_blood_call_return(cpu, PC_A + 16));
    assert(blood_seed == 0x9999 && pool_checks == checks + 1);
#endif
#ifdef DC_TEST_CALLS
    DcCallState saved, invalid;
    dc_call_capture(&saved);
    dc_call_reset();
    assert(dc_call_restore(&saved));
    invalid = saved; invalid.version++;
    assert(!dc_call_restore(&invalid));
    invalid = saved; invalid.frames[1] = invalid.frames[0];
    assert(!dc_call_restore(&invalid));
    dc_anim_call_end(cpu, PC_A + 4);
    assert(animation_calls == 2);
    dc_call_capture(&saved); dc_call_reset(); assert(dc_call_restore(&saved));
    dc_anim_call_end(cpu, PC_A + 4);
    assert(animation_calls == 2);
    static const uint32_t call_kinds[] = {
        DC_CALL_ANIMATION, DC_CALL_BLOOD, DC_CALL_DRAW_POSE,
        DC_CALL_SOUND, DC_CALL_MESH, DC_CALL_GRENADE, DC_CALL_SHOT,
        DC_CALL_CREATURE_BLOOD, DC_CALL_BACKGROUND
    };
    DcCallFrame *kind_frames[sizeof call_kinds / sizeof call_kinds[0]];
    kind_frames[0] = dc_call_begin(cpu, 0x9000, DC_CALL_ANIMATION);
    assert(dc_call_has_kind(DC_CALL_ANIMATION));
    dc_call_capture(&saved);
    dc_call_reset();
    assert(!dc_call_has_kind(DC_CALL_ANIMATION));
    assert(dc_call_restore(&saved));
    assert(dc_call_has_kind(DC_CALL_ANIMATION));
    dc_call_finish(kind_frames[0]);
    assert(!dc_call_has_kind(DC_CALL_ANIMATION));
    for (unsigned i = 0; i < sizeof call_kinds / sizeof call_kinds[0]; ++i) {
        kind_frames[i] = dc_call_begin(cpu, 0xA000 + i * 4, call_kinds[i]);
        assert(dc_call_has_kind(call_kinds[i]));
    }
    dc_call_discard_thread(TCB_A);
    for (unsigned i = 0; i < sizeof call_kinds / sizeof call_kinds[0]; ++i)
        assert(!dc_call_has_kind(call_kinds[i]));
    dc_anim_call_begin(cpu, PC_A + 4);
    dc_call_discard_thread(TCB_A | 0xA0000000u);
    dc_call_discard_thread(TCB_B);
    dc_call_capture(&saved);
    for (unsigned i = 0; i < DC_CALL_CAPACITY; ++i) assert(saved.frames[i].kind == 0);
#endif
    assert(cpu->gpr[16] == 0xA123 && cpu->gpr[2] == 1);
    assert(cpu->hi == 0x1234 && cpu->lo == 0x5678);
    assert(cpu->cop0[12] == 0x401 && (cpu->cop0[13] & 0x400) == 0);
    assert(read_word(TCB_B + 136) == PC_B + 4);
    assert(read_word(TCB_B + 8 + 16 * 4) == 0xB123);
#ifdef DC_TEST_KERNEL
    DcCallState thread_calls;
    dc_call_capture(&thread_calls);
    assert(thread_calls.frames[0].kind == DC_CALL_ANIMATION);
    kernel_call(cpu, 0x0F, 0xFF000001);
    assert(cpu->gpr[2] == 1 && read_word(TCB_B) == 0x1000);
    dc_call_capture(&thread_calls);
    for (unsigned i = 0; i < DC_CALL_CAPACITY; ++i) assert(thread_calls.frames[i].kind == 0);
    kernel_call(cpu, 0x0E, PC_B + 8);
    assert(cpu->gpr[2] == 0xFF000001 && read_word(TCB_B + 136) == PC_B + 8);
#endif
    psx_request_return_to_lobby();
    abort();
}

int main(void)
{
    test_process_init();
    CPUState cpu = {0};
    active_cpu = &cpu;
    cpu.read_word = read_word;
    cpu.write_word = write_word;
    write_word(0x108, PCB);
    write_word(0x110, TCB_A);
    write_word(0x114, 0x180);
    write_word(PCB, TCB_A);
    write_word(TCB_A, 0x4000);
    write_word(TCB_B, 0x4000);
    write_word(TCB_A + 136, PC_A);
    write_word(TCB_B + 136, PC_B);
    write_word(TCB_A + 148, 0x404);
    write_word(TCB_B + 148, 0x404);
    write_word(TCB_A + 8 + 16 * 4, 0xAAAA);
    write_word(TCB_B + 8 + 16 * 4, 0xBBBB);
#if defined(DC_TEST_CALLS) || defined(DC_TEST_KERNEL) || defined(DC_TEST_BLOOD) || defined(DC_TEST_POSE) || defined(DC_TEST_SOUND) || defined(DC_TEST_MESH) || defined(DC_TEST_GRENADE) || defined(DC_TEST_SHOT) || defined(DC_TEST_CREATURE_BLOOD)
    for (unsigned i = 0; i < 3; ++i) {
        write_word(TCB_A + 8 + (4 + i) * 4, 0xAA01 + i);
        write_word(TCB_B + 8 + (4 + i) * 4, 0xBB01 + i);
    }
    write_word(TCB_A + 8 + 29 * 4, 0x7000);
    write_word(TCB_B + 8 + 29 * 4, 0x7000);
#endif
    psx_scheduler_run(&cpu);
    assert(dispatches == 3 && yields == 2 && cleanup_count == 3);
    assert(read_word(PCB) == TCB_A);
    assert(psx_return_to_lobby_requested());
#ifdef DC_TEST_KERNEL
    assert(discarded_threads == 3);
#endif
    puts("PASS: TCB switch, resume registers, return value and escape cleanup");
    return 0;
}
