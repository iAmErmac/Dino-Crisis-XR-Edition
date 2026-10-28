#include "dc_kernel_snapshot.h"
#include "pst_wire.h"

/* Guest RAM owns the tables; this stores only the kernel's host-side state. */
typedef struct DcKernelHostState {
    uint32_t irq[IRQ_PRIO], hook, exception_tcb;
    int32_t card_delay, card_armed;
    uint32_t card_spec, pad_buf[2], pad_len[2];
    int32_t pad_started, find_active, find_card, find_index;
    char pattern[24];
    uint32_t find_entry;
    struct {
        int32_t used, card, first, position, size;
        char name[24];
    } file[CARD_FD_N];
    uint32_t random_seed, heap_base, heap_size;
} DcKernelHostState;

static void dc_kernel_host_capture(DcKernelHostState *v)
{
    memset(v, 0, sizeof *v);
    memcpy(v->irq, s_irq_chain, sizeof v->irq);
    v->hook = s_entry_hook; v->exception_tcb = s_exception_tcb;
    v->card_delay = s_card_late.vbl; v->card_armed = s_card_late.armed;
    v->card_spec = s_card_late.spec;
    memcpy(v->pad_buf, s_pad_buf, sizeof v->pad_buf);
    memcpy(v->pad_len, s_pad_len, sizeof v->pad_len);
    v->pad_started = s_pad_started;
    v->find_active = s_ff.active; v->find_card = s_ff.card; v->find_index = s_ff.idx;
    memcpy(v->pattern, s_ff.pat, sizeof v->pattern); v->find_entry = s_ff.entry;
    for (unsigned i = 0; i < CARD_FD_N; ++i) {
        v->file[i].used = s_cfd[i].used; v->file[i].card = s_cfd[i].card;
        v->file[i].first = s_cfd[i].first; v->file[i].position = s_cfd[i].pos;
        v->file[i].size = s_cfd[i].size;
        memcpy(v->file[i].name, s_cfd[i].name, sizeof v->file[i].name);
    }
    v->random_seed = s_rand_seed; v->heap_base = s_heap_base; v->heap_size = s_heap_size;
}

static int dc_kernel_host_valid(const DcKernelHostState *v)
{
    if ((v->card_armed != 0 && v->card_armed != 1) ||
        (v->pad_started != 0 && v->pad_started != 1) ||
        (v->find_active != 0 && v->find_active != 1)) return 0;
    if (v->find_active && (v->find_card < 0 || v->find_card > 1 ||
        v->find_index < 0 || v->find_index >= CARD_DIR_N ||
        !memchr(v->pattern, 0, sizeof v->pattern))) return 0;
    for (unsigned i = 0; i < CARD_FD_N; ++i) {
        if (v->file[i].used != 0 && v->file[i].used != 1) return 0;
        /* Seeking beyond EOF is legal; reading decides whether a block exists. */
        if (v->file[i].used && (v->file[i].card < 0 || v->file[i].card > 1 ||
            v->file[i].first < 0 || v->file[i].first >= CARD_DIR_N ||
            v->file[i].position < 0 || v->file[i].size <= 0 ||
            !memchr(v->file[i].name, 0, sizeof v->file[i].name))) return 0;
    }
    return 1;
}

static int dc_kernel_host_wire(DcKernelHostState *v, PstW *w, PstR *r)
{
    uint32_t version = 1, priorities = IRQ_PRIO, files = CARD_FD_N;
#define WORD(x) do { if (r ? !pst_r_pod(r, &(x), sizeof(x), 4) :     !pst_w_pod(w, &(x), sizeof(x), 4)) return 0; } while (0)
#define BYTES(x) do { if (r ? !pst_r_bytes(r, (x), sizeof(x)) :     !pst_w_bytes(w, (x), sizeof(x))) return 0; } while (0)
    WORD(version); WORD(priorities); WORD(files);
    if (version != 1 || priorities != IRQ_PRIO || files != CARD_FD_N) return 0;
    WORD(v->irq); WORD(v->hook); WORD(v->exception_tcb);
    WORD(v->card_delay); WORD(v->card_armed); WORD(v->card_spec);
    WORD(v->pad_buf); WORD(v->pad_len); WORD(v->pad_started);
    WORD(v->find_active); WORD(v->find_card); WORD(v->find_index);
    BYTES(v->pattern); WORD(v->find_entry);
    for (unsigned i = 0; i < CARD_FD_N; ++i) {
        WORD(v->file[i].used); WORD(v->file[i].card); WORD(v->file[i].first);
        WORD(v->file[i].position); WORD(v->file[i].size); BYTES(v->file[i].name);
    }
    WORD(v->random_seed); WORD(v->heap_base); WORD(v->heap_size);
#undef WORD
#undef BYTES
    return 1;
}

size_t dc_kernel_snapshot_bytes(void)
{
    DcKernelHostState value = {0}; PstW w;
    pst_w_init(&w, NULL, 0);
    return dc_kernel_host_wire(&value, &w, NULL) ? w.written : 0;
}

int dc_kernel_snapshot_write(uint8_t *data, size_t size)
{
    DcKernelHostState value; PstW w;
    if (!data || size != dc_kernel_snapshot_bytes() || psx_get_in_exception()) return 0;
    dc_kernel_host_capture(&value);
    if (!dc_kernel_host_valid(&value)) return 0;
    pst_w_init(&w, data, size);
    return dc_kernel_host_wire(&value, &w, NULL) && w.written == size;
}

static int dc_kernel_host_decode(const uint8_t *data, size_t size, DcKernelHostState *value)
{
    PstR r;
    if (!data || size != dc_kernel_snapshot_bytes()) return 0;
    memset(value, 0, sizeof *value); pst_r_init(&r, data, size);
    return dc_kernel_host_wire(value, NULL, &r) && r.p == r.end && dc_kernel_host_valid(value);
}

int dc_kernel_snapshot_validate(const uint8_t *data, size_t size)
{
    DcKernelHostState value;
    return dc_kernel_host_decode(data, size, &value);
}

int dc_kernel_snapshot_read(const uint8_t *data, size_t size, CPUState *cpu)
{
    DcKernelHostState v;
    if (!cpu || psx_get_in_exception() || !dc_kernel_host_decode(data, size, &v)) return 0;
    memcpy(s_irq_chain, v.irq, sizeof s_irq_chain);
    s_entry_hook = v.hook; s_exception_tcb = v.exception_tcb;
    s_card_late.vbl = v.card_delay; s_card_late.armed = v.card_armed;
    s_card_late.spec = v.card_spec;
    memcpy(s_pad_buf, v.pad_buf, sizeof s_pad_buf); memcpy(s_pad_len, v.pad_len, sizeof s_pad_len);
    s_pad_started = v.pad_started;
    s_ff.active = v.find_active; s_ff.card = v.find_card; s_ff.idx = v.find_index;
    memcpy(s_ff.pat, v.pattern, sizeof s_ff.pat); s_ff.entry = v.find_entry;
    for (unsigned i = 0; i < CARD_FD_N; ++i) {
        s_cfd[i].used = v.file[i].used; s_cfd[i].card = v.file[i].card;
        s_cfd[i].first = v.file[i].first; s_cfd[i].pos = v.file[i].position;
        s_cfd[i].size = v.file[i].size;
        memcpy(s_cfd[i].name, v.file[i].name, sizeof s_cfd[i].name);
    }
    s_rand_seed = v.random_seed; s_heap_base = v.heap_base; s_heap_size = v.heap_size;
    s_cpu = cpu;
    return 1;
}
