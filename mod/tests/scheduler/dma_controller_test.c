/* The store that kicks a GPU linked-list DMA returns only when the walk ends.
 *
 * PSX-SPX "CPU Operation during DMA": the CPU waits while DMA moves data. A
 * guest that rewrites a packet right after DrawOTag therefore cannot reach it
 * before the walk does. The retail SCPH1001 intro does exactly that: it
 * re-copies its text packets from a template, clearing the link word, right
 * after the kick. With the CPU running alongside the walk, the copy reached a
 * packet first and the walk followed link 0 into low RAM.
 *
 * Here the list is A -> B -> end. Right after the kicking store returns, the
 * "CPU" clears B's header, then time runs until the walk is done:
 *
 *   - held (the default): the GPU received A's and B's payload in order, and
 *     guest time advanced by the walk inside the store;
 *   - control: a kick made from inside the device service is not held (the
 *     walk is still running when the store returns).
 *
 * Production dma.c and dma_gpu_ll.c (test_dma_gpu_list_cpu_hold.py links
 * them); memory, GPU and clock are seams, everything else aborts. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dma.h"
#include "test_process.h"
#include "cdrom.h"
#include "dc_dma_policy.h"

void     dma_init(void);
void     dma_write(uint32_t addr, uint32_t val);
void     dma_advance(uint32_t cycles);
int      dma_gpu_linked_list_active(void);
uint32_t dma_cycles_to_internal_event(void);
uint32_t dma_snapshot_bytes(void);
void     dma_snapshot_write(uint8_t *p);
int      dma_snapshot_validate(const uint8_t *p, uint32_t len);
int      dma_snapshot_read(const uint8_t *p, uint32_t len);

/* ---- clock and device service ------------------------------------------ */
uint64_t psx_cycle_count;
uint64_t psx_next_service_cycle;
int      psx_in_device_service;
int      g_event_step_conservative;
int      g_ls_replay_active;
int      g_psx_render_pass_active;
int      g_psx_guest_time_frozen;
uint32_t g_psx_cyc_batch;
uint32_t g_psx_cyc_batch_limit;

static uint64_t s_serviced_to;
void psx_devices_service_to_now(void) {
    if (psx_in_device_service) return;
    psx_in_device_service = 1;
    const uint64_t now = psx_cycle_count;
    dma_advance((uint32_t)(now - s_serviced_to));
    s_serviced_to = now;
    const uint32_t next = dma_cycles_to_internal_event();
    psx_next_service_cycle = next == 0xFFFFFFFFu ? UINT64_MAX : now + (next ? next : 1u);
    psx_in_device_service = 0;
}
void psx_advance_cycles_slow(uint32_t cycles) {
    psx_cycle_count += cycles;
    psx_devices_service_to_now();
}

/* ---- memory ------------------------------------------------------------- */
uint32_t g_psx_ram_size = 0x00200000u;
uint32_t g_psx_ram_mask = 0x001FFFFFu;
static uint8_t ram[0x00200000u];
uint8_t *memory_get_ram_ptr(void) { return ram; }
uint32_t psx_read_word(uint32_t a) {
    uint32_t v; memcpy(&v, ram + (a & 0x1FFFFCu), 4); return v;
}
void psx_write_word(uint32_t a, uint32_t v) { memcpy(ram + (a & 0x1FFFFCu), &v, 4); }
uint32_t psx_mod_gpu_dma_resolve_address(uint32_t a) { return a; }
int psx_mod_gpu_dma_memory_contains(uint32_t a, uint32_t width) {
    (void)a; (void)width; return 0;
}

/* Product policy observations. These are the real policy's narrow host
 * collaborators, kept explicit so the event order is test-visible. */
enum ProductEventKind {
    PRODUCT_BEGIN,
    PRODUCT_SUBMISSION,
    PRODUCT_NODE,
    PRODUCT_PAYLOAD,
    PRODUCT_FINISH,
    PRODUCT_OTC
};
typedef struct ProductEvent {
    int kind;
    uint32_t value;
} ProductEvent;
static ProductEvent product_events[64];
static unsigned product_event_count;
uint32_t g_ot_lo, g_ot_hi;
int g_ot_watch, g_dc_pass_id, g_dc_dma_pass;

static void product_event(int kind, uint32_t value) {
    if (product_event_count >= sizeof(product_events) / sizeof(product_events[0])) {
        fprintf(stderr, "product event buffer overflow\n");
        abort();
    }
    product_events[product_event_count].kind = kind;
    product_events[product_event_count].value = value;
    product_event_count++;
}

void dc_meshrep_dma_begin(void) { product_event(PRODUCT_BEGIN, 0); }
void dc_meshrep_dma_at(uint32_t address) { product_event(PRODUCT_NODE, address); }
void dc_meshrep_dma(uint32_t mask) { product_event(PRODUCT_FINISH, mask); }
void dc_meshrep_otc(uint32_t address) { product_event(PRODUCT_OTC, address); }
void dcvk_note_submission(void) { product_event(PRODUCT_SUBMISSION, 0); }

static void unexpected_dependency(const char *name) {
    fprintf(stderr, "unexpected unrelated DMA dependency: %s\n", name);
    abort();
}

/* ---- GPU ---------------------------------------------------------------- */
static uint32_t s_gp0[64];
static unsigned s_gp0_n;
void gpu_write_gp0(uint32_t val) {
    if (s_gp0_n < 64u) s_gp0[s_gp0_n] = val;
    ++s_gp0_n;
    product_event(PRODUCT_PAYLOAD, val);
}
void gpu_set_gp0_source(uint32_t addr) { (void)addr; }
uint32_t gpu_read_gpuread(void) { unexpected_dependency("gpu_read_gpuread"); return 0; }
uint32_t gpu_read_gpustat(void) { unexpected_dependency("gpu_read_gpustat"); return 0; }
void gpu_write_gp1(uint32_t value) { (void)value; unexpected_dependency("gpu_write_gp1"); }

/* ---- observers and IRQ (inert) ----------------------------------------- */
uint32_t i_stat;
uint32_t g_debug_current_func_addr;
uint32_t g_debug_last_store_pc;
uint64_t s_frame_count;
uint64_t g_io_openbus_reads;
uint64_t g_io_openbus_writes;
void event_ring_record_aux(uint16_t kind, uint8_t detail, uint32_t aux) {
    (void)kind; (void)detail; (void)aux;
}
void psx_irq_raise(uint32_t bit, uint32_t detail) { (void)bit; (void)detail; }
static int s_in_exception;
int psx_get_in_exception(void) { return s_in_exception; }
void psx_fatal_halt(const char *reason) {
    fprintf(stderr, "unexpected DMA fatal: %s\n", reason ? reason : "(null)");
    abort();
}
const char *g_psx_fatal_reason;

/* The linked-list fixture never enters these other device paths. An explicit
 * abort keeps an accidental dependency visible instead of making the test
 * pass through a fake device. */
void dirty_ram_mark_executable_range(uint32_t phys, uint32_t len) {
    (void)phys; (void)len; unexpected_dependency("dirty_ram_mark_executable_range");
}
void overlay_capture_before_dma(uint32_t addr, uint32_t size) {
    (void)addr; (void)size; unexpected_dependency("overlay_capture_before_dma");
}
void overlay_capture_on_dma(uint32_t addr, uint32_t size, const uint8_t *bytes) {
    (void)addr; (void)size; (void)bytes; unexpected_dependency("overlay_capture_on_dma");
}
void audio_trace_event(uint16_t kind, uint32_t a, uint32_t b) {
    (void)kind; (void)a; (void)b; unexpected_dependency("audio_trace_event");
}

int cdrom_get_setloc_lba(void) { unexpected_dependency("cdrom_get_setloc_lba"); return -1; }
uint32_t cdrom_dma_read(void) { unexpected_dependency("cdrom_dma_read"); return 0; }
int cdrom_dma_ready(void) { unexpected_dependency("cdrom_dma_ready"); return 0; }
uint32_t cdrom_dma_sector_word_count(void) {
    unexpected_dependency("cdrom_dma_sector_word_count"); return 0;
}
void cdrom_debug_snapshot(CDROMDebugState *out) {
    (void)out; unexpected_dependency("cdrom_debug_snapshot");
}

void mdec_dma_write_word(uint32_t value) {
    (void)value; unexpected_dependency("mdec_dma_write_word");
}
uint32_t mdec_dma_read_word(void) { unexpected_dependency("mdec_dma_read_word"); return 0; }
uint32_t mdec_dma_write_words(const uint32_t *src, uint32_t max_words) {
    (void)src; (void)max_words; unexpected_dependency("mdec_dma_write_words"); return 0;
}
uint32_t mdec_dma_read_words(uint32_t *dst, uint32_t max_words) {
    (void)dst; (void)max_words; unexpected_dependency("mdec_dma_read_words"); return 0;
}
int mdec_dma_write_ready(void) { unexpected_dependency("mdec_dma_write_ready"); return 0; }
int mdec_dma_read_ready(void) { unexpected_dependency("mdec_dma_read_ready"); return 0; }
void mdec_debug_dma_in_start(uint32_t addr, uint32_t words) {
    (void)addr; (void)words; unexpected_dependency("mdec_debug_dma_in_start");
}
void mdec_debug_dma_in_end(uint32_t addr, uint32_t words) {
    (void)addr; (void)words; unexpected_dependency("mdec_debug_dma_in_end");
}
void mdec_debug_dma_out_start(uint32_t addr, uint32_t words) {
    (void)addr; (void)words; unexpected_dependency("mdec_debug_dma_out_start");
}
void mdec_debug_dma_out_end(uint32_t addr, uint32_t words) {
    (void)addr; (void)words; unexpected_dependency("mdec_debug_dma_out_end");
}

void spu_dma_write(uint32_t word) {
    (void)word; unexpected_dependency("spu_dma_write");
}
uint32_t spu_dma_read(void) { unexpected_dependency("spu_dma_read"); return 0; }

/* ---- scenario ------------------------------------------------------------ */
#define NODE_A 0x00001000u
#define NODE_B 0x00002000u
#define NODE_C 0x00003000u
#define END    0x00FFFFFFu

static int failures;
#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);              \
            fprintf(stderr, __VA_ARGS__);                                     \
            fputc('\n', stderr);                                              \
            exit(1);                                                       \
        }                                                                     \
    } while (0)

static void build_list(void) {
    memset(ram, 0, sizeof ram);
    psx_write_word(NODE_A,      (2u << 24) | NODE_B);
    psx_write_word(NODE_A + 4u, 0xE1000001u);
    psx_write_word(NODE_A + 8u, 0xE2000002u);
    psx_write_word(NODE_B,      (1u << 24) | NODE_C);
    psx_write_word(NODE_B + 4u, 0x01000003u);
    psx_write_word(NODE_C,      END);
}

/* Kicks the list; `in_service` makes the kick look like one from inside the
 * device service. Returns 1 when the walk was still active after the store. */
static int kick(int in_service) {
    dma_init();
    s_gp0_n = 0;
    psx_cycle_count = 1000u;
    s_serviced_to = psx_cycle_count;
    psx_next_service_cycle = UINT64_MAX;
    build_list();
    dma_write(0x1F8010F0u, 0x00000800u);           /* DPCR: channel 2 enabled */
    dma_write(0x1F8010A0u, NODE_A);                /* MADR */
    dma_write(0x1F8010A4u, 0u);                    /* BCR */
    psx_in_device_service = in_service;
    dma_write(0x1F8010A8u, 0x01000401u);           /* CHCR: from RAM, list, start */
    psx_in_device_service = 0;
    return dma_gpu_linked_list_active();
}

static void finish_walk(void) {
    for (int i = 0; i < 1000000 && dma_gpu_linked_list_active(); i++) {
        psx_cycle_count += 1u;
        psx_devices_service_to_now();
    }
}

#define PRODUCT_NODE_A 0x000B5850u
#define PRODUCT_NODE_B 0x000B6850u
#define PRODUCT_OTC_ADDR 0x00009000u

static void product_reset_observations(void) {
    product_event_count = 0;
    s_gp0_n = 0;
    memset(s_gp0, 0, sizeof(s_gp0));
    g_ot_lo = 0;
    g_ot_hi = 0;
    g_ot_watch = 0;
    g_dc_pass_id = 17;
    g_dc_dma_pass = 0;
}

static void build_product_list(uint32_t first_words, uint32_t second_words) {
    memset(ram, 0, sizeof ram);
    psx_write_word(PRODUCT_NODE_A, (first_words << 24) | PRODUCT_NODE_B);
    for (uint32_t i = 0; i < first_words; i++)
        psx_write_word(PRODUCT_NODE_A + 4u + i * 4u, 0xA1000000u + i);
    psx_write_word(PRODUCT_NODE_B, (second_words << 24) | END);
    for (uint32_t i = 0; i < second_words; i++)
        psx_write_word(PRODUCT_NODE_B + 4u + i * 4u, 0xB2000000u + i);
}

static int product_kick(uint32_t first_words, uint32_t second_words) {
    dma_init();
    product_reset_observations();
    psx_cycle_count = 1000u;
    s_serviced_to = psx_cycle_count;
    psx_next_service_cycle = UINT64_MAX;
    g_psx_render_pass_active = 0;
    build_product_list(first_words, second_words);
    dma_write(0x1F8010F0u, 0x00000800u);
    dma_write(0x1F8010A0u, PRODUCT_NODE_A);
    dma_write(0x1F8010A4u, 0u);
    psx_in_device_service = 1;
    dma_write(0x1F8010A8u, 0x01000401u);
    psx_in_device_service = 0;
    return dma_gpu_linked_list_active();
}

static void product_advance_one(void) {
    psx_cycle_count++;
    dma_advance(1u);
}

static void product_finish(void) {
    for (int i = 0; i < 1000000 && dma_gpu_linked_list_active(); i++)
        product_advance_one();
    CHECK(!dma_gpu_linked_list_active(), "product linked list did not finish");
}

static void check_product_event(unsigned index, int kind, uint32_t value) {
    CHECK(index < product_event_count, "missing product event %u (count %u)",
          index, product_event_count);
    if (index < product_event_count) {
        CHECK(product_events[index].kind == kind && product_events[index].value == value,
              "product event %u was kind %d value %08X, expected kind %d value %08X",
              index, product_events[index].kind, product_events[index].value,
              kind, value);
    }
}

static void test_product_ordering_table(void) {
    DMAGpuOtStats before, after;
    dma_debug_get_gpu_ot_stats(&before);
    CHECK(product_kick(1u, 1u), "product list did not remain active during register kick");
    product_finish();
    dma_debug_get_gpu_ot_stats(&after);

    CHECK(product_event_count == 7u, "product completion event count %u", product_event_count);
    check_product_event(0u, PRODUCT_BEGIN, 0u);
    check_product_event(1u, PRODUCT_SUBMISSION, 0u);
    check_product_event(2u, PRODUCT_NODE, PRODUCT_NODE_A);
    check_product_event(3u, PRODUCT_PAYLOAD, 0xA1000000u);
    check_product_event(4u, PRODUCT_NODE, PRODUCT_NODE_B);
    check_product_event(5u, PRODUCT_PAYLOAD, 0xB2000000u);
    check_product_event(6u, PRODUCT_FINISH, 3u);
    CHECK(s_gp0_n == 2u && s_gp0[0] == 0xA1000000u && s_gp0[1] == 0xB2000000u,
          "product payloads were %u (%08X %08X)", s_gp0_n, s_gp0[0], s_gp0[1]);
    CHECK(after.starts == before.starts + 1u && after.completes == before.completes + 1u,
          "product stats starts %llu/%llu completes %llu/%llu",
          (unsigned long long)before.starts, (unsigned long long)after.starts,
          (unsigned long long)before.completes, (unsigned long long)after.completes);
    CHECK(after.cancels == before.cancels && after.nodes_last == 2u && after.words_last == 4u,
          "product stats cancels %llu/%llu nodes %u words %u",
          (unsigned long long)before.cancels, (unsigned long long)after.cancels,
          after.nodes_last, after.words_last);
    CHECK(g_ot_lo == PRODUCT_NODE_A && g_ot_hi == PRODUCT_NODE_B &&
          g_ot_watch == 1 && g_dc_dma_pass == g_dc_pass_id,
          "ordering-table policy range/watch was %08X..%08X watch %d pass %d/%d",
          g_ot_lo, g_ot_hi, g_ot_watch, g_dc_dma_pass, g_dc_pass_id);
}

static void test_otc_and_cancel(void) {
    DMAGpuOtStats before, after;
    dma_init();
    product_reset_observations();
    psx_write_word(PRODUCT_OTC_ADDR, 0u);
    psx_write_word(PRODUCT_OTC_ADDR - 4u, 0u);
    psx_write_word(PRODUCT_OTC_ADDR - 8u, 0u);
    dma_write(0x1F8010F0u, 0x08000000u);
    dma_write(0x1F8010E0u, PRODUCT_OTC_ADDR);
    dma_write(0x1F8010E4u, 3u);
    dma_write(0x1F8010E8u, 0x01000002u);
    CHECK(product_event_count == 1u, "OTC event count %u", product_event_count);
    check_product_event(0u, PRODUCT_OTC, PRODUCT_OTC_ADDR);
    CHECK(psx_read_word(PRODUCT_OTC_ADDR) == ((PRODUCT_OTC_ADDR - 4u) & 0x00FFFFFFu) &&
          psx_read_word(PRODUCT_OTC_ADDR - 4u) == ((PRODUCT_OTC_ADDR - 8u) & 0x00FFFFFFu) &&
          psx_read_word(PRODUCT_OTC_ADDR - 8u) == END,
          "OTC did not write the expected backward chain");

    dma_debug_get_gpu_ot_stats(&before);
    CHECK(product_kick(3u, 1u), "cancel list did not remain active during register kick");
    product_advance_one();
    CHECK(product_event_count == 3u && product_events[2].kind == PRODUCT_NODE &&
          product_events[2].value == PRODUCT_NODE_A,
          "cancel did not reach first product node before CHCR clear");
    dma_write(0x1F8010A8u, 0u);
    dma_debug_get_gpu_ot_stats(&after);
    CHECK(!dma_gpu_linked_list_active(), "clearing CHCR did not cancel the list");
    CHECK(product_event_count == 3u, "cancel emitted a finish/payload event");
    CHECK(after.cancels == before.cancels + 1u && after.completes == before.completes,
          "cancel stats cancels %llu/%llu completes %llu/%llu",
          (unsigned long long)before.cancels, (unsigned long long)after.cancels,
          (unsigned long long)before.completes, (unsigned long long)after.completes);
    CHECK(g_ot_watch == 0, "cancel left ordering-table watch armed");
}

static void test_snapshot_resume_and_refusal(void) {
    uint8_t snapshot[512], bad[512];
    DMADebugState before_state, after_state;
    const uint32_t bytes = dma_snapshot_bytes();
    CHECK(bytes <= sizeof(snapshot), "DMA snapshot unexpectedly large: %u", bytes);
    CHECK(product_kick(1u, 1u), "snapshot list did not remain active during register kick");
    for (int i = 0; i < 16 && !(product_event_count == 4u && s_gp0_n == 1u); i++)
        product_advance_one();
    CHECK(product_event_count == 4u && product_events[2].kind == PRODUCT_NODE &&
          product_events[3].kind == PRODUCT_PAYLOAD,
          "snapshot did not stop after first node payload (events %u)", product_event_count);
    dma_snapshot_write(snapshot);
    /* Perturb the live product policy after capture. A valid restore must put
     * its active/mask state back before the remaining node completes. */
    dc_dma_policy_cancel();
    CHECK(g_ot_watch == 0, "policy perturbation did not clear OT watch");
    memcpy(bad, snapshot, bytes);
    bad[bytes - 4u] = 4u;
    bad[bytes - 3u] = bad[bytes - 2u] = bad[bytes - 1u] = 0u;
    dma_debug_get_state(&before_state);
    CHECK(!dma_snapshot_validate(bad, bytes), "malformed late policy snapshot validated");
    CHECK(!dma_snapshot_read(bad, bytes), "malformed late policy snapshot restored");
    dma_debug_get_state(&after_state);
    CHECK(product_event_count == 4u && s_gp0_n == 1u &&
          after_state.channels[2].active == before_state.channels[2].active &&
          after_state.channels[2].chcr == before_state.channels[2].chcr &&
          g_ot_watch == 0,
          "late policy refusal changed live DMA/callback state");

    memcpy(bad, snapshot, bytes);
    bad[146u + 32u] = 0u;
    bad[146u + 33u] = bad[146u + 34u] = bad[146u + 35u] = 0u;
    CHECK(!dma_snapshot_validate(bad, bytes), "malformed core snapshot validated");
    CHECK(!dma_snapshot_read(bad, bytes), "malformed core snapshot restored");
    dma_debug_get_state(&after_state);
    CHECK(product_event_count == 4u && s_gp0_n == 1u &&
          after_state.channels[2].active == before_state.channels[2].active &&
          after_state.channels[2].chcr == before_state.channels[2].chcr &&
          g_ot_watch == 0,
          "core refusal changed live DMA/callback state");

    CHECK(dma_snapshot_validate(snapshot, bytes), "valid product snapshot rejected");
    CHECK(dma_snapshot_read(snapshot, bytes), "valid product snapshot did not restore");
    CHECK(product_event_count == 4u && s_gp0_n == 1u && dma_gpu_linked_list_active(),
          "valid resume replayed the first node or stopped the walk");
    product_finish();
    CHECK(product_event_count == 7u && s_gp0_n == 2u,
          "snapshot resume produced event count %u payload count %u",
          product_event_count, s_gp0_n);
    check_product_event(4u, PRODUCT_NODE, PRODUCT_NODE_B);
    check_product_event(5u, PRODUCT_PAYLOAD, 0xB2000000u);
    check_product_event(6u, PRODUCT_FINISH, 3u);
}

int main(void) {
    test_process_init();
    /* Held: the walk is over when the kicking store returns. */
    const uint64_t before = 1000u;
    const int active = kick(0);
    CHECK(!active, "walk still active after the kicking store");
    CHECK(psx_cycle_count > before, "guest time did not advance during the kick");
    psx_write_word(NODE_B, 0u);                     /* the guest's packet copy */
    finish_walk();
    CHECK(s_gp0_n == 3u && s_gp0[0] == 0xE1000001u && s_gp0[1] == 0xE2000002u &&
          s_gp0[2] == 0x01000003u,
          "GPU received %u words (%08X %08X %08X), expected A's and B's payload",
          s_gp0_n, s_gp0[0], s_gp0[1], s_gp0[2]);

    /* The hold is recorded in the always-on hold ring (dma_hold_stats /
     * dma_hold_ring): A's header + 2 words, B's header + 1 word, C's header. */
    {
        DMAGpuHoldStats st; const DMAGpuHoldEntry *ring = NULL;
        dma_debug_get_gpu_hold(&st, &ring);
        CHECK(st.count == 1u && st.exc_count == 0u, "hold count %llu exc %llu",
              (unsigned long long)st.count, (unsigned long long)st.exc_count);
        CHECK(ring && ring[0].words == 6u && ring[0].held_cycles == psx_cycle_count - before
              && ring[0].cycle == before && ring[0].in_exception == 0u
              && ring[0].still_active == 0u,
              "hold entry words %u held %u cycle %llu", ring ? ring[0].words : 0u,
              ring ? ring[0].held_cycles : 0u,
              ring ? (unsigned long long)ring[0].cycle : 0ull);
        /* A kick from inside an exception handler is attributed to it. */
        s_in_exception = 1;
        kick(0);
        s_in_exception = 0;
        dma_debug_get_gpu_hold(&st, &ring);
        CHECK(st.count == 2u && st.exc_count == 1u && ring[1].in_exception == 1u &&
              st.exc_held_cycles == ring[1].held_cycles,
              "exception-kick attribution: count %llu exc %llu",
              (unsigned long long)st.count, (unsigned long long)st.exc_count);
    }

    /* Control: a kick from inside the device service is not held. */
    CHECK(kick(1), "a kick from inside the device service was held");
    finish_walk();
    CHECK(!dma_gpu_linked_list_active(), "control walk did not finish");

    test_product_ordering_table();
    test_otc_and_cancel();
    test_snapshot_resume_and_refusal();

    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    puts("PASS: the GPU linked-list kick holds the CPU until the walk ends");
    return 0;
}
