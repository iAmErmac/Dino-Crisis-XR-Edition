#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "dma_gpu_ll.h"

enum {
    ARENA_NODE = 0x00402000u,
    APERTURE_NODE = 0x00802000u,
    TERMINATOR = 0x00FFFFFFu,
    TRACE_CAP = 32u
};

extern uint32_t psx_mod_gpu_dma_resolve_address(uint32_t address);
extern uint32_t psx_mod_gpu_dma_memory_alloc(uint32_t size,
                                             uint32_t alignment);
extern int psx_mod_gpu_dma_memory_contains(uint32_t address,
                                           uint32_t bytes);
extern uint32_t psx_read_word(uint32_t address);
extern void psx_write_word(uint32_t address, uint32_t value);

typedef struct {
    uint32_t headers[TRACE_CAP];
    uint32_t header_words[TRACE_CAP];
    uint32_t begins[TRACE_CAP];
    uint32_t begin_counts[TRACE_CAP];
    uint32_t emit_addresses[TRACE_CAP];
    uint32_t emit_words[TRACE_CAP];
    uint32_t header_count;
    uint32_t begin_count;
    uint32_t emit_count;
    uint32_t complete_count;
    int complete_hit_limit;
    uint32_t mutate_address;
    uint32_t mutate_word;
    int mutate_after_first_emit;
} Trace;

static uint32_t resolve_address(void *opaque, uint32_t address)
{
    uint32_t resolved;
    (void)opaque;
    resolved = psx_mod_gpu_dma_resolve_address(address);
    assert(resolved == address);
    return resolved;
}

static uint32_t read_word(void *opaque, uint32_t address)
{
    (void)opaque;
    return psx_read_word(address);
}

static void observe_header(void *opaque, uint32_t address, uint32_t header)
{
    Trace *trace = (Trace *)opaque;
    assert(trace->header_count < TRACE_CAP);
    trace->headers[trace->header_count] = address;
    trace->header_words[trace->header_count] = header;
    ++trace->header_count;
}

static int begin_node(void *opaque, uint32_t address, uint32_t word_count)
{
    Trace *trace = (Trace *)opaque;
    assert(trace->begin_count < TRACE_CAP);
    trace->begins[trace->begin_count] = address;
    trace->begin_counts[trace->begin_count] = word_count;
    ++trace->begin_count;
    return 1;
}

static void emit_word(void *opaque, uint32_t address, uint32_t word)
{
    Trace *trace = (Trace *)opaque;
    assert(trace->emit_count < TRACE_CAP);
    trace->emit_addresses[trace->emit_count] = address;
    trace->emit_words[trace->emit_count] = word;
    ++trace->emit_count;
    if (trace->mutate_after_first_emit && trace->emit_count == 1u) {
        psx_write_word(trace->mutate_address, trace->mutate_word);
        trace->mutate_after_first_emit = 0;
    }
}

static void complete(void *opaque, int hit_limit)
{
    Trace *trace = (Trace *)opaque;
    ++trace->complete_count;
    trace->complete_hit_limit = hit_limit;
}

static const DMAGPULinkedListOps OPS = {
    resolve_address,
    read_word,
    observe_header,
    begin_node,
    emit_word,
    complete
};

static void seed_word(uint32_t address, uint32_t value)
{
    psx_write_word(address, value);
    assert(psx_read_word(address) == value);
}

static void seed_node(uint32_t address, uint32_t next,
                      uint32_t first, uint32_t second)
{
    seed_word(address, (2u << 24) | next);
    seed_word(address + 4u, first);
    seed_word(address + 8u, second);
}

static void seed_single_node(uint32_t address, uint32_t next,
                             uint32_t first, uint32_t second,
                             uint32_t third)
{
    seed_word(address, (3u << 24) | next);
    seed_word(address + 4u, first);
    seed_word(address + 8u, second);
    seed_word(address + 12u, third);
}

static void seed_loop_node(uint32_t address, uint32_t value)
{
    seed_word(address, (1u << 24) | address);
    seed_word(address + 4u, value);
}

static void run_cyclewise(DMAGPULinkedList *state, Trace *trace)
{
    while (state->active) {
        assert(dma_gpu_ll_cycles_to_event(state) == 1u);
        dma_gpu_ll_advance(state, 1u, &OPS, trace);
    }
    assert(dma_gpu_ll_cycles_to_event(state) == UINT32_MAX);
}

static void test_two_domain_order(void)
{
    DMAGPULinkedList state;
    Trace trace;

    memset(&trace, 0, sizeof trace);
    seed_node(ARENA_NODE, APERTURE_NODE, 0xA001u, 0xA002u);
    seed_node(APERTURE_NODE, TERMINATOR, 0xB001u, 0xB002u);

    dma_gpu_ll_start(&state, ARENA_NODE, 4u);
    run_cyclewise(&state, &trace);

    assert(trace.header_count == 2u);
    assert(trace.headers[0] == ARENA_NODE);
    assert(trace.headers[1] == APERTURE_NODE);
    assert(trace.header_words[0] == ((2u << 24) | APERTURE_NODE));
    assert(trace.header_words[1] == ((2u << 24) | TERMINATOR));
    assert(trace.begin_count == 2u);
    assert(trace.begins[0] == ARENA_NODE);
    assert(trace.begins[1] == APERTURE_NODE);
    assert(trace.begin_counts[0] == 2u);
    assert(trace.begin_counts[1] == 2u);
    assert(trace.emit_count == 4u);
    assert(trace.emit_addresses[0] == ARENA_NODE + 4u);
    assert(trace.emit_addresses[1] == ARENA_NODE + 8u);
    assert(trace.emit_addresses[2] == APERTURE_NODE + 4u);
    assert(trace.emit_addresses[3] == APERTURE_NODE + 8u);
    assert(trace.emit_words[0] == 0xA001u);
    assert(trace.emit_words[1] == 0xA002u);
    assert(trace.emit_words[2] == 0xB001u);
    assert(trace.emit_words[3] == 0xB002u);
    assert(trace.complete_count == 1u);
    assert(!trace.complete_hit_limit);
    assert(state.nodes_processed == 2u);
    assert(state.total_words == 6u);
}

static void test_live_payload_mutation(void)
{
    DMAGPULinkedList state;
    Trace trace;

    memset(&trace, 0, sizeof trace);
    seed_single_node(ARENA_NODE, TERMINATOR, 0xC001u, 0xC002u, 0xC003u);
    trace.mutate_address = ARENA_NODE + 8u;
    trace.mutate_word = 0xC0DEu;
    trace.mutate_after_first_emit = 1;

    dma_gpu_ll_start(&state, ARENA_NODE, 2u);
    run_cyclewise(&state, &trace);

    assert(trace.emit_count == 3u);
    assert(trace.emit_addresses[0] == ARENA_NODE + 4u);
    assert(trace.emit_addresses[1] == ARENA_NODE + 8u);
    assert(trace.emit_addresses[2] == ARENA_NODE + 12u);
    assert(trace.emit_words[0] == 0xC001u);
    assert(trace.emit_words[1] == 0xC0DEu);
    assert(trace.emit_words[2] == 0xC003u);
}

static void test_cancel(void)
{
    DMAGPULinkedList state;
    Trace trace;
    DMAGPULinkedList zero;

    memset(&trace, 0, sizeof trace);
    seed_node(ARENA_NODE, TERMINATOR, 0xD001u, 0xD002u);
    dma_gpu_ll_start(&state, ARENA_NODE, 2u);
    dma_gpu_ll_advance(&state, 1u, &OPS, &trace);
    dma_gpu_ll_advance(&state, 1u, &OPS, &trace);
    assert(state.active);
    assert(trace.emit_count == 1u);
    dma_gpu_ll_cancel(&state);

    memset(&zero, 0, sizeof zero);
    assert(memcmp(&state, &zero, sizeof state) == 0);
    assert(trace.complete_count == 0u);
}

static void test_max_nodes_loop_limit(void)
{
    DMAGPULinkedList state;
    Trace trace;

    memset(&trace, 0, sizeof trace);
    seed_loop_node(ARENA_NODE, 0xE001u);
    dma_gpu_ll_start(&state, ARENA_NODE, 2u);
    run_cyclewise(&state, &trace);

    assert(trace.header_count == 2u);
    assert(trace.begin_count == 2u);
    assert(trace.emit_count == 2u);
    assert(trace.emit_addresses[0] == ARENA_NODE + 4u);
    assert(trace.emit_addresses[1] == ARENA_NODE + 4u);
    assert(trace.emit_words[0] == 0xE001u);
    assert(trace.emit_words[1] == 0xE001u);
    assert(trace.complete_count == 1u);
    assert(trace.complete_hit_limit);
    assert(state.nodes_processed == 2u);
    assert(state.hit_limit);
}

static void test_capture_copy_resume(void)
{
    DMAGPULinkedList state;
    DMAGPULinkedList captured;
    Trace original;
    Trace resumed;

    memset(&original, 0, sizeof original);
    seed_single_node(ARENA_NODE, TERMINATOR, 0xF001u, 0xF002u, 0xF003u);
    dma_gpu_ll_start(&state, ARENA_NODE, 2u);
    while (state.active &&
           !(state.phase == DMA_GPU_LL_PHASE_PAYLOAD &&
             state.payload_index == 1u)) {
        assert(dma_gpu_ll_cycles_to_event(&state) == 1u);
        dma_gpu_ll_advance(&state, 1u, &OPS, &original);
    }
    assert(state.active);
    assert(state.phase == DMA_GPU_LL_PHASE_PAYLOAD);
    assert(state.payload_index == 1u);
    assert(original.emit_count == 1u);

    captured = state;
    resumed = original;
    run_cyclewise(&state, &original);
    state = captured;
    run_cyclewise(&state, &resumed);

    assert(memcmp(&original, &resumed, sizeof original) == 0);
    assert(original.emit_count == 3u);
    assert(original.emit_addresses[1] == ARENA_NODE + 8u);
    assert(original.emit_addresses[2] == ARENA_NODE + 12u);
    assert(original.emit_words[1] == 0xF002u);
    assert(original.emit_words[2] == 0xF003u);
    assert(original.complete_count == 1u);
    assert(!original.complete_hit_limit);
}

void test_memory_linked_list(void)
{
    assert(psx_mod_gpu_dma_memory_contains(APERTURE_NODE, 16u) ||
           psx_mod_gpu_dma_memory_alloc(0x3000u, 16u) != 0u);
    assert(psx_mod_gpu_dma_memory_contains(APERTURE_NODE, 16u));
    assert(psx_mod_gpu_dma_resolve_address(ARENA_NODE) == ARENA_NODE);
    assert(psx_mod_gpu_dma_resolve_address(APERTURE_NODE) == APERTURE_NODE);

    test_two_domain_order();
    test_live_payload_mutation();
    test_cancel();
    test_max_nodes_loop_limit();
    test_capture_copy_resume();
}
