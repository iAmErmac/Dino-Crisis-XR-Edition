/* Synthetic-sector integration tests for the actual CD controller. */
#include "cdrom.c"
#include "test_process.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

enum HookEvent {
    HOOK_NOTE,
    HOOK_VIRTUAL,
    HOOK_ISO_RAW,
    HOOK_ISO_COOKED,
    HOOK_MOD_RAW,
    HOOK_MOD_COOKED,
    HOOK_MOVIE,
    HOOK_XA,
    HOOK_SPU,
    HOOK_TALK,
    HOOK_ROOM
};

static int game_started;
int fntrace_is_game_started(void) { return game_started; }
static int hook_mode;
static int hook_events[32];
static int hook_event_count;
static const int *hook_failfast_expected;
static int hook_failfast_count;
static int hook_raw_result, hook_cooked_result, hook_virtual_result;
static int hook_raw_file, hook_raw_channel, hook_raw_submode, hook_raw_coding;
static int hook_movie_first;
static int hook_xa_calls, hook_xa_lba, hook_xa_channel;
static int hook_spu_calls, hook_spu_frames, hook_spu_first;
static int hook_talk_calls, hook_talk_lba, hook_talk_file, hook_talk_channel, hook_talk_frames;
static int hook_room_calls, hook_room_lba;
static int hook_mod_init_calls, hook_mod_changed, hook_mod_reloads;

static void setup(void);

static void hook_event(int event) {
    CHECK(hook_event_count < (int)(sizeof hook_events / sizeof hook_events[0]));
    if (hook_failfast_expected) {
        CHECK(hook_event_count < hook_failfast_count);
        CHECK(hook_failfast_expected[hook_event_count] == event);
    }
    hook_events[hook_event_count++] = event;
}

static void hook_arm_expected(const int *expected, int count) {
    hook_failfast_expected = expected;
    hook_failfast_count = count;
    hook_event_count = 0;
}

static void hook_expect(const int *expected, int count) {
    CHECK(hook_event_count == count);
    for (int i = 0; i < count; i++) CHECK(hook_events[i] == expected[i]);
}

static void hook_defaults(void) {
    hook_mode = 0;
    hook_event_count = 0;
    hook_failfast_expected = NULL;
    hook_failfast_count = 0;
    hook_raw_result = 0;
    hook_cooked_result = 1;
    hook_virtual_result = 0;
    hook_raw_file = 7;
    hook_raw_channel = 3;
    hook_raw_submode = 0;
    hook_raw_coding = 0;
    hook_movie_first = -1;
    hook_xa_calls = 0;
    hook_xa_lba = -1;
    hook_xa_channel = -1;
    hook_spu_calls = 0;
    hook_spu_frames = 0;
    hook_spu_first = 0;
    hook_talk_calls = 0;
    hook_talk_lba = -1;
    hook_talk_file = -1;
    hook_talk_channel = -1;
    hook_talk_frames = 0;
    hook_room_calls = 0;
    hook_room_lba = -1;
    hook_mod_init_calls = 0;
}

static void hook_reset_sector_state(void) {
    clear_sector_buffer();
    last_sector_lba = -1;
    last_sector_size = 0;
    last_sector_frame = 0;
    last_sector_have_raw = 0;
    last_sector_raw_mode = 0;
    last_sector_xa_file = 0;
    last_sector_xa_channel = 0;
    last_sector_xa_submode = 0;
    last_sector_xa_coding = 0;
    mode_reg = 0;
    cd_muted = 0;
    xa_data_end_pending = 0;
    xa_reset_decode();
    s_frame_count = 0;
    hook_event_count = 0;
    hook_movie_first = -1;
    hook_xa_calls = 0;
    hook_spu_calls = 0;
    hook_talk_calls = 0;
    hook_room_calls = 0;
}

static void hook_setup(void) {
    hook_defaults();
    setup();
    CHECK(hook_mod_init_calls == 1);
    hook_mode = 1;
    hook_reset_sector_state();
}
uint32_t i_stat, g_debug_current_func_addr, g_debug_last_store_pc;
uint64_t s_frame_count, psx_cycle_count;
static int test_dma_active;
int dma_cdrom_transfer_active(void) { return test_dma_active; }
void psx_irq_raise(uint32_t bit, uint32_t detail) { (void)detail; i_stat |= bit; }
int iso_read_raw_sector(void* h, uint32_t lba, uint8_t* out, int size) {
    (void)h;
    if (hook_mode) hook_event(HOOK_ISO_RAW);
    if (!hook_mode) {
        memset(out, 0, size); out[15]=2; out[18]=out[22]=8;
        memset(out+24, (unsigned char)lba, 2048); return 1;
    }
    if (!hook_raw_result) return 0;
    memset(out, 0, size);
    out[15] = 2;
    out[16] = (uint8_t)hook_raw_file;
    out[17] = (uint8_t)hook_raw_channel;
    out[18] = (uint8_t)hook_raw_submode;
    out[19] = (uint8_t)hook_raw_coding;
    memset(out + 24, 0x22, SECTOR_SIZE);
    return 1;
}
int iso_read_sector(void* h,uint32_t lba,uint8_t* out,int size) {
    (void)h; (void)lba;
    if (hook_mode) hook_event(HOOK_ISO_COOKED);
    if (hook_mode && !hook_cooked_result) return 0;
    memset(out, hook_mode ? 0x11 : (unsigned char)lba, size);
    return 1;
}

static void setup(void) {
    test_dma_active = 0;
    psx_cycle_count = 1000000;
    cdrom_init("synthetic");
    mode_reg = 0x80;
    reading = 1;
    read_min = 0; read_sec = 3; read_sect = 25; /* LBA 100 */
}
static void sector(void) {
    psx_cycle_count += 225792;
    read_delay = 0;
    process_read_stream(0);
}
static void ack(void) {
    cdrom_write(0x1f801800, 1);
    cdrom_write(0x1f801803, 7);
    cdrom_write(0x1f801800, 0);
}
static void request(void) {
    cdrom_write(0x1f801800, 0);
    cdrom_write(0x1f801803, 0);
    cdrom_write(0x1f801803, 0x80);
}
static void drain(unsigned lba, int words) {
    for (int n = 0; n < words; n++) {
        CHECK(cdrom_dma_ready());
        CHECK(cdrom_dma_read() == lba * 0x01010101u);
    }
}
static void delayed_notification(void) {
    setup(); sector(); sector(); /* 100 announced, 101 pending */
    CHECK(pending_dataready && last_sector_lba == 101);
    ack(); request();
    psx_cycle_count += CDROM_PEND_PRESENT_DELAY;
    cdrom_advance(0); /* 101 becomes visible between BFRD and DMA */
    CHECK(irq_flag == CDIRQ_DATA_READY && !pending_dataready);
    drain(100, 512);
    CHECK(!cdrom_dma_ready());
    ack(); request();
    drain(101, 512);
    CHECK(s_int1_lost == 0);
}
static void dma_continuation(void) {
    setup(); sector(); request(); drain(100, 512);
    test_dma_active = 1;
    sector(); drain(101, 512);
    sector(); drain(102, 512);
    CHECK(!pending_dataready);
    CHECK(s_int1_lost == 0);
    test_dma_active = 0;
}
static void dma_arrival_mid_drain(void) {
    setup(); sector(); request();
    test_dma_active = 1;
    drain(100, 17);
    sector(); /* Decoder advances while the old FIFO still has unread bytes. */
    drain(100, 495);
    drain(101, 512);
    CHECK(!cdrom_dma_ready());
    CHECK(!pending_dataready && s_int1_lost == 0);
    test_dma_active = 0;
}
static void request_rearms_retained_sector(void) {
    setup(); sector(); request(); drain(100, 512); ack();
    sector(); /* BFRD remains asserted and preloads 101. */
    request(); /* BIOS clears BFRD before requesting that same sector. */
    drain(101, 512);
    CHECK(!cdrom_dma_ready());
}
static void snapshot_pending_deadline(void) {
    setup(); sector(); request(); sector(); ack();
    psx_cycle_count += 123;
    const uint32_t size = cdrom_snapshot_bytes();
    uint8_t *wire = malloc(size);
    CHECK(wire);
    cdrom_snapshot_write(wire);
    setup();
    psx_cycle_count = 0; /* Restore at a different cycle origin. */
    CHECK(cdrom_snapshot_read(wire, size));
    CHECK(pending_present_due == CDROM_PEND_PRESENT_DELAY - 123);
    psx_cycle_count = pending_present_due - 1;
    cdrom_advance(0);
    CHECK(irq_flag == 0 && pending_dataready);
    psx_cycle_count++;
    cdrom_advance(0);
    CHECK(irq_flag == CDIRQ_DATA_READY && !pending_dataready);
    drain(100, 512);
    request(); drain(101, 512);
    free(wire);
}
static void snapshot_between_request_and_dma(void) {
    setup(); sector(); request(); drain(100, 11); sector();
    const uint32_t size = cdrom_snapshot_bytes();
    uint8_t *wire = malloc(size), *after = malloc(size);
    CHECK(wire && after);
    cdrom_snapshot_write(wire);
    CHECK(cdrom_snapshot_validate(wire, size));
    setup();
    CHECK(cdrom_snapshot_read(wire, size));
    ack();
    psx_cycle_count += CDROM_PEND_PRESENT_DELAY;
    cdrom_advance(0);
    drain(100, 501);
    ack(); request(); drain(101, 512);

    /* Malformed ring indices must be rejected before controller mutation. */
    cdrom_snapshot_write(after);
    const uint32_t legacy = cdrom_snapshot_legacy_bytes();
    wire[legacy + 4] = 0xff;
    CHECK(!cdrom_snapshot_validate(wire, size));
    CHECK(!cdrom_snapshot_read(wire, size));
    cdrom_snapshot_write(wire);
    CHECK(!memcmp(wire, after, size));

    CHECK(!cdrom_snapshot_validate(NULL, size));
    CHECK(!cdrom_snapshot_validate(after, size - 1));
    memcpy(wire, after, size);
    memset(wire + legacy + 16, 0x7f, 4); /* Impossible pending delay. */
    CHECK(!cdrom_snapshot_read(wire, size));
    cdrom_snapshot_write(wire);
    CHECK(!memcmp(wire, after, size));

    /* Old states retain their original wire prefix and readable payload. */
    setup(); sector(); request(); drain(100, 7);
    PstW writer;
    pst_w_init(&writer, wire, size);
    CHECK(cdrom_snap_emit(&writer, 0));
    CHECK(writer.written == legacy);
    setup();
    CHECK(cdrom_snapshot_validate(wire, legacy));
    CHECK(cdrom_snapshot_read(wire, legacy));
    drain(100, 505);
    free(wire); free(after);
}
static void invalid_setloc_preserves_target(void) {
    const uint8_t invalid[][3] = {
        {0xC0,0,0}, {0x0A,0,0}, {0,0x60,0}, {0,0x0A,0},
        {0,0,0x75}, {0,0,0x0A}, {0xFF,0xFF,0xFF}
    };
    for (unsigned n=0;n<sizeof invalid/sizeof invalid[0];++n) {
        setup();
        param_count=3;
        param_fifo[0]=0; param_fifo[1]=3; param_fifo[2]=0x25;
        exec_command(0x02);
        CHECK(irq_flag==CDIRQ_ACK && s_setloc_lba==100 && setloc_pending);
        ack();
        const int pending=setloc_pending, seek_far=setloc_seek_far;
        memcpy(param_fifo,invalid[n],3); param_count=3;
        exec_command(0x02);
        CHECK(irq_flag==CDIRQ_ERROR && response_count==2 && response_fifo[1]==0x10);
        CHECK((response_fifo[0]&1u)!=0);
        CHECK(s_setloc_lba==100 && seek_min==0 && seek_sec==3 && seek_sect==25);
        CHECK(setloc_pending==pending && setloc_seek_far==seek_far);
    }
}
static void pause_read_resumes_position(void) {
    const uint8_t reads[] = {0x06, 0x1B};
    for (unsigned n = 0; n < sizeof reads; ++n) {
        setup();
        seek_min = 0; seek_sec = 2; seek_sect = 0; /* stale target, LBA 0 */
        sector(); ack(); /* delivered 100; drive will read 101 next */
        exec_command(0x09); ack();
        CHECK(!reading && msf_to_lba(read_min, read_sec, read_sect) == 101);
        exec_command(reads[n]);
        CHECK(reading && read_cmd == reads[n] && !setloc_pending);
        CHECK(msf_to_lba(read_min, read_sec, read_sect) == 101);
        ack(); sector(); request(); drain(101, 512);
        ack();
        /* Another pause cannot regress to the original target either. */
        exec_command(0x09); ack(); exec_command(reads[n]);
        CHECK(msf_to_lba(read_min, read_sec, read_sect) == 102);
        ack();
        /* An explicit new Setloc still takes precedence over the drive. */
        exec_command(0x09); ack();
        param_fifo[0] = 0; param_fifo[1] = 4; param_fifo[2] = 0; param_count = 3;
        exec_command(0x02); ack(); exec_command(reads[n]);
        CHECK(msf_to_lba(read_min, read_sec, read_sect) == 150);
        CHECK(!setloc_pending);
        ack(); sector(); request(); drain(150, 512);
        /* A completed explicit seek retains its own destination too. */
        ack(); exec_command(0x09); ack();
        param_fifo[0] = 0; param_fifo[1] = 5; param_fifo[2] = 0; param_count = 3;
        exec_command(0x02); ack(); exec_command(0x15); ack();
        psx_cycle_count += 100000000; process_pending(0); ack();
        exec_command(reads[n]);
        CHECK(msf_to_lba(read_min, read_sec, read_sect) == 225);
    }
}
static void snapshot_setloc_and_paused_head(void) {
    setup(); sector(); ack(); exec_command(0x09); ack();
    param_fifo[0]=0;param_fifo[1]=4;param_fifo[2]=0;param_count=3;
    exec_command(0x02);ack();
    const uint32_t size=cdrom_snapshot_bytes();
    uint8_t* wire=malloc(size);CHECK(wire);
    cdrom_snapshot_write(wire);
    setloc_pending=0;read_sec=9;
    CHECK(cdrom_snapshot_read(wire,size) && setloc_pending);
    exec_command(0x1B);ack();
    CHECK(msf_to_lba(read_min,read_sec,read_sect)==150);
    sector();ack();exec_command(0x09);ack();
    cdrom_snapshot_write(wire);
    CHECK(!setloc_pending);
    setloc_pending=1;read_sec=9;
    CHECK(cdrom_snapshot_read(wire,size) && !setloc_pending);
    exec_command(0x1B);ack();
    CHECK(msf_to_lba(read_min,read_sec,read_sect)==151);
    wire[size-1]=2;CHECK(!cdrom_snapshot_validate(wire,size));
    /* CDF2 states remain readable and retain the paused drive position. */
    exec_command(0x09);ack();
    PstW writer;pst_w_init(&writer,wire,size);
    CHECK(cdrom_snap_emit(&writer,1) && writer.written==size-5);
    read_sec=9;
    CHECK(cdrom_snapshot_read(wire,size-5));
    exec_command(0x06);
    CHECK(msf_to_lba(read_min,read_sec,read_sect)==151);
    free(wire);
}

static void product_cooked_delivery(void) {
    hook_setup();
    hook_raw_result = 0;
    hook_cooked_result = 1;
    hook_virtual_result = 0;
    uint8_t direct[SECTOR_SIZE];
    const int direct_expected[] = { HOOK_ISO_COOKED, HOOK_MOD_COOKED };
    hook_arm_expected(direct_expected, (int)(sizeof direct_expected / sizeof direct_expected[0]));
    CHECK(cdrom_image_read_sector(42, direct));
    CHECK(direct[0] == 0xC1);
    hook_expect(direct_expected, (int)(sizeof direct_expected / sizeof direct_expected[0]));

    hook_reset_sector_state();
    const int expected[] = {
        HOOK_NOTE, HOOK_VIRTUAL, HOOK_ISO_RAW, HOOK_ISO_COOKED,
        HOOK_MOD_COOKED, HOOK_MOVIE, HOOK_ROOM
    };
    hook_arm_expected(expected, (int)(sizeof expected / sizeof expected[0]));
    CHECK(read_sector_at(0, 2, 16));
    CHECK(last_sector_lba == 16 && last_sector_size == SECTOR_SIZE);
    CHECK(last_sector_buffer[0] == 0xC1);
    CHECK(hook_movie_first == 0xC1);
    CHECK(hook_room_lba == 1016);
    hook_expect(expected, (int)(sizeof expected / sizeof expected[0]));
}

static void product_raw_delivery(void) {
    hook_setup();
    hook_raw_result = 1;
    hook_cooked_result = 1;
    hook_virtual_result = 0;
    hook_raw_submode = 0;
    hook_reset_sector_state();
    const int expected[] = {
        HOOK_NOTE, HOOK_VIRTUAL, HOOK_ISO_RAW, HOOK_MOD_RAW,
        HOOK_MOVIE, HOOK_ROOM
    };
    hook_arm_expected(expected, (int)(sizeof expected / sizeof expected[0]));
    CHECK(read_sector_at(0, 2, 16));
    CHECK(last_sector_buffer[0] == 0xA2);
    CHECK(hook_movie_first == 0xA2);
    CHECK(hook_room_lba == 1016);
    hook_expect(expected, (int)(sizeof expected / sizeof expected[0]));
}

static void product_virtual_short_circuit(void) {
    hook_setup();
    hook_raw_result = 1;
    hook_cooked_result = 1;
    hook_virtual_result = 1;
    hook_reset_sector_state();
    const int expected[] = { HOOK_NOTE, HOOK_VIRTUAL, HOOK_MOVIE, HOOK_ROOM };
    hook_arm_expected(expected, (int)(sizeof expected / sizeof expected[0]));
    CHECK(read_sector_at(0, 2, 16));
    CHECK(last_sector_buffer[0] == 0xB3);
    CHECK(hook_movie_first == 0xB3);
    CHECK(hook_room_lba == 1016);
    hook_expect(expected, (int)(sizeof expected / sizeof expected[0]));
}

static void product_xa_delivery(void) {
    hook_setup();
    hook_raw_result = 1;
    hook_cooked_result = 1;
    hook_virtual_result = 0;
    hook_raw_file = 2;
    hook_raw_channel = 5;
    hook_raw_submode = XA_SUBMODE_AUDIO | XA_SUBMODE_REALTIME;
    hook_raw_coding = 0;
    hook_reset_sector_state();
    mode_reg = 0x40;
    const int expected[] = {
        HOOK_NOTE, HOOK_VIRTUAL, HOOK_ISO_RAW, HOOK_MOD_RAW,
        HOOK_MOVIE, HOOK_XA, HOOK_SPU, HOOK_TALK, HOOK_ROOM
    };
    hook_arm_expected(expected, (int)(sizeof expected / sizeof expected[0]));
    CHECK(!read_sector_at(0, 2, 16));
    CHECK(hook_xa_calls == 1 && hook_xa_lba == 16 && hook_xa_channel == 5);
    CHECK(hook_spu_calls == 1 && hook_spu_frames > 0 && hook_spu_first == 0x1234);
    CHECK(hook_talk_calls == 1 && hook_talk_lba == 16 && hook_talk_file == 2);
    CHECK(hook_talk_channel == 5 && hook_talk_frames == hook_spu_frames);
    CHECK(hook_movie_first == 0xA2);
    hook_expect(expected, (int)(sizeof expected / sizeof expected[0]));
}

static void product_reload_waits_for_response(void) {
    static const int no_events[] = {0};
    hook_defaults(); setup(); reading = 0;
    hook_mode = 1; hook_arm_expected(no_events, 0);
    hook_mod_changed = 1; hook_mod_reloads = 0;
    pending.pending = 1; pending.due_cyc = UINT64_MAX;
    cdrom_advance(0); CHECK(hook_mod_reloads == 0);
    pending.pending = 0;
    cdrom_advance(0); CHECK(hook_mod_reloads == 1);
    cdrom_advance(0); CHECK(hook_mod_reloads == 1);
}

int main(void) {
    game_started = 0; reading = 1; CHECK(!cdrom_streaming());
    game_started = 1; CHECK(cdrom_streaming());
    reading = 0; CHECK(!cdrom_streaming());
    xa_stream_active = 1; CHECK(cdrom_xa_stream_active());
    xa_stream_active = 0; CHECK(!cdrom_xa_stream_active());

    test_process_init();
    pause_read_resumes_position();
    snapshot_setloc_and_paused_head();
    invalid_setloc_preserves_target();
    delayed_notification();
    dma_continuation();
    dma_arrival_mid_drain();
    request_rearms_retained_sector();
    snapshot_between_request_and_dma();
    snapshot_pending_deadline();
    product_cooked_delivery();
    product_raw_delivery();
    product_virtual_short_circuit();
    product_xa_delivery();
    product_reload_waits_for_response();
    puts("PASS: CD FIFO plus product raw/cooked/virtual/movie/XA hook delivery");
    return 0;
}

void spu_cd_audio_reset(void) {}
void spu_cd_audio_push(const int16_t* stereo,int frames) {
    if (hook_mode) {
        hook_event(HOOK_SPU);
        hook_spu_calls++;
        hook_spu_frames = frames;
        hook_spu_first = frames > 0 ? stereo[0] : 0;
    }
}
void* iso_open(const char* p) {(void)p;return (void*)1;}
void iso_close(void* h) {(void)h;}
int iso_has_subq_replacements(void* h) {(void)h;return 0;}
int iso_read_subq(void* h,uint32_t lba,uint8_t* b,int n,int* v) {(void)h;(void)lba;(void)b;(void)n;*v=0;return 0;}
int iso_read_cdda_sector(void* h,uint32_t lba,uint8_t* b,int n) {(void)h;(void)lba;memset(b,0,n);return 0;}
uint32_t iso_cdda_track_start_lba(void* h,int track) {(void)h;(void)track;return 0;}
uint32_t iso_cdda_track_pregap_lba(void* h,int track) {(void)h;(void)track;return 0;}
int iso_cdda_track_count(void* h) {(void)h;return 1;}
uint32_t iso_cdda_sector_count(void* h) {(void)h;return 1000;}
int iso_cdda_track_is_audio(void* h,int track) {(void)h;(void)track;return 0;}
uint32_t iso_sector_count(void* h) {(void)h;return 1000;}
int iso_track_count(void* h) {(void)h;return 1;}
uint32_t iso_track_start_lba(void* h,int track) {(void)h;(void)track;return 0;}
uint32_t iso_track_pregap_lba(void* h,int track) {(void)h;(void)track;return 0;}
int iso_track_is_audio(void* h,int track) {(void)h;(void)track;return 0;}
void event_ring_record_aux(uint16_t k,uint8_t d,uint32_t a) {(void)k;(void)d;(void)a;}
void event_ring_record(uint16_t k,uint8_t d) {(void)k;(void)d;}
void savestate_set_disc_scope(int n) {(void)n;}
int psx_netplay_cd_bisect_active(void) {return 0;}
uint32_t psx_netplay_sim_tick(void) {return 0;}
uint32_t interrupts_get_cycles_since_vblank(void) {return 0;}
int psx_netplay_is_resimulating(void) {return 0;}
void audio_trace_event(uint16_t k,uint32_t a,uint32_t b) {(void)k;(void)a;(void)b;}

void dc_mods_init(void *iso_handle) {
    (void)iso_handle;
    hook_mod_init_calls++;
}
void dc_mods_raw(uint32_t lba, uint8_t *raw2352) {
    (void)lba;
    if (!hook_mode) return;
    hook_event(HOOK_MOD_RAW);
    raw2352[RAW_USER_DATA_OFFSET] = 0xA2;
}
void dc_mods_cooked(uint32_t lba, uint8_t *buf2048) {
    (void)lba;
    if (!hook_mode) return;
    hook_event(HOOK_MOD_COOKED);
    buf2048[0] = 0xC1;
}
int dc_mods_virtual_raw(uint32_t lba, uint8_t *raw2352) {
    (void)lba;
    if (hook_mode) hook_event(HOOK_VIRTUAL);
    if (!hook_mode || !hook_virtual_result) return 0;
    memset(raw2352, 0, RAW_SECTOR_SIZE);
    raw2352[15] = 2;
    raw2352[16] = (uint8_t)hook_raw_file;
    raw2352[17] = (uint8_t)hook_raw_channel;
    raw2352[18] = (uint8_t)hook_raw_submode;
    raw2352[19] = (uint8_t)hook_raw_coding;
    raw2352[24] = 0xB3;
    return 1;
}
uint32_t dc_mods_virtual_home(uint32_t lba) { return lba + 1000u; }
int dc_mods_xa(int lba, int channel, int16_t *native_stereo, int frames) {
    if (!hook_mode) return 0;
    hook_event(HOOK_XA);
    CHECK(frames > 0);
    hook_xa_calls++;
    hook_xa_lba = lba;
    hook_xa_channel = channel;
    native_stereo[0] = 0x1234;
    native_stereo[1] = 0x1234;
    return 1;
}
void dc_mods_movie_sector(uint32_t lba, const uint8_t *user2048) {
    (void)lba;
    if (!hook_mode) return;
    hook_event(HOOK_MOVIE);
    hook_movie_first = user2048[0];
}
void dc_mods_note_read(uint32_t lba) {(void)lba; if (hook_mode) hook_event(HOOK_NOTE);}
int dc_mods_changed(void) { return hook_mod_changed; }
void dc_mods_reload(void) { CHECK(!pending.pending); ++hook_mod_reloads; hook_mod_changed = 0; }
void dc_talk_xa_pushed(int lba, int file, int channel, int frames) {
    if (!hook_mode) return;
    hook_event(HOOK_TALK);
    hook_talk_calls++;
    hook_talk_lba = lba;
    hook_talk_file = file;
    hook_talk_channel = channel;
    hook_talk_frames = frames;
}
void dc_room_note_lba(int lba) {
    if (!hook_mode) return;
    hook_event(HOOK_ROOM);
    hook_room_calls++;
    hook_room_lba = lba;
}
