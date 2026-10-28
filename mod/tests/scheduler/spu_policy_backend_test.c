#define main dc_spu_pages_backend_main
#include "spu_pages_backend_test.c"
#undef main
#include "dc_spu_policy.h"

static void test_overwritten_end(void)
{
    cycle = 0; spu_init(); seed(spu_ram, 0x11); start(0, 0);
    spu_ram[0x111] = 2;
    decode_block(&voices[0]); decode_block(&voices[0]);
    assert(voices[0].flags == 1 && (endx_latch & 1u));
    voices[0].sample_idx = SPU_BLOCK_SAMPLES;
    voices[0].env_level = 0x7FFF;
    assert(voice_next_sample(0) == 0 && !voices[0].env_level);

    spu_init(); seed(spu_ram, 0x11); seed(spu_xpage(2), 0x22); start(0, 2);
    spu_xpage(2)[0x111] = 2;
    decode_block(&voices[0]); decode_block(&voices[0]);
    assert(voices[0].flags == 2 && !(endx_latch & 1u));

    uint8_t bank[0x120] = {0}; seed(bank, 0x33);
    assert(spu_register_sample_bank(1, bank, sizeof bank));
    assert(spu_bind_next_voice_bank(0, 1)); start(0, 0);
    sample_banks[1].data[0x111] = 2;
    decode_block(&voices[0]); decode_block(&voices[0]);
    assert(voices[0].flags == 2 && !(endx_latch & 1u));
}

static void test_guard_snapshots(void)
{
    spu_init(); seed(spu_ram, 0x11); start(0, 0);
    uint32_t core = spu_snapshot_core_bytes(), pages = dc_spu_pages_snapshot_bytes();
    uint32_t size = spu_snapshot_bytes();
    uint8_t *saved = malloc(size), *bad = malloc(size), *after = malloc(size);
    assert(saved && bad && after); spu_snapshot_write(saved);
    memcpy(bad, saved, size); snapshot_put_u32(bad + core + pages + 4, 1);
    assert(!spu_snapshot_validate(bad, size));
    assert(!spu_snapshot_read(bad, size));
    spu_snapshot_write(after); assert(!memcmp(saved, after, size));
    dc_spu_policy_reset(); assert(spu_snapshot_validate(saved, size));
    assert(spu_snapshot_read(saved, size));
    spu_ram[0x111] = 0; decode_block(&voices[0]); decode_block(&voices[0]);
    assert(voices[0].flags == 1);

    uint32_t old_size = size - DC_SPU_POLICY_STATE_BYTES;
    memcpy(bad, saved, core + pages);
    snapshot_put_u32(bad + core + pages, SPU_PAGES_TAG);
    snapshot_put_u32(bad + core + pages + 4, core);
    snapshot_put_u32(bad + core + pages + 8, pages);
    assert(spu_snapshot_len_ok(old_size) && spu_snapshot_read(bad, old_size));
    decode_block(&voices[0]); decode_block(&voices[0]);
    assert(voices[0].flags == 0);
    free(saved); free(bad); free(after);
}

static void cd_setup(void)
{
    cycle = 0; spu_init();
    spu_write(0x1F801D80u, 0x3FFF); spu_write(0x1F801D82u, 0x3FFF);
    spu_write(0x1F801DB0u, 0x7FFF); spu_write(0x1F801DB2u, 0x7FFF);
    spu_write(0x1F801DAAu, 0x8001);
}

static void next_cd_frame(int expected_left, int expected_right)
{
    int16_t out[2]; cycle += 768; spu_catch_up();
    assert(spu_output_pop(out, 1) == 1);
    assert(out[0] == expected_left && out[1] == expected_right);
}

static void test_cd_arrival_time(void)
{
    const int16_t pcm[] = {12000, 6000}; int16_t out[10];
    cd_setup();
    cycle = 768u * 5u + 100u;
    spu_cd_audio_push(pcm, 1);
    assert(spu_output_pop(out, 5) == 5);
    for (unsigned i = 0; i < 10; ++i) assert(!out[i]);
    assert(cd_frame_count == 1);
    cycle += 668; spu_catch_up(); assert(spu_output_pop(out, 1) == 1 && !out[0] && !out[1]);
    next_cd_frame(0, 0); next_cd_frame(0, 0);
    assert(cd_frame_count == 1 && !cd_underflow_frames);
    next_cd_frame(11998, 5998); assert(!cd_frame_count);
}

static void test_cd_delay_restore(void)
{
    const int16_t pcm[] = {12000, 6000};
    cd_setup(); spu_cd_audio_push(pcm, 1); next_cd_frame(0, 0);
    spu_cd_audio_push(pcm, 1);
    uint32_t size = spu_snapshot_bytes(), cd_size = spu_cd_snapshot_bytes();
    uint8_t *saved = malloc(size), *cd_saved = malloc(cd_size); assert(saved && cd_saved);
    spu_snapshot_write(saved); spu_cd_snapshot_write(cd_saved);
    next_cd_frame(0, 0); next_cd_frame(0, 0); next_cd_frame(11998, 5998);
    assert(spu_snapshot_validate(saved, size));
    assert(spu_snapshot_read(saved, size));
    assert(spu_cd_snapshot_validate(cd_saved, cd_size));
    uint8_t invalid_queue[4] = {1, 0, 0, 0};
    assert(!spu_cd_snapshot_validate(invalid_queue, sizeof invalid_queue));
    assert(!spu_cd_snapshot_read(invalid_queue, sizeof invalid_queue));
    assert(!spu_cd_snapshot_validate(NULL, cd_size));
    assert(spu_cd_snapshot_read(cd_saved, cd_size));
    next_cd_frame(0, 0); next_cd_frame(0, 0); next_cd_frame(11998, 5998);
    assert(cd_frame_count == 1);
    spu_cd_audio_reset(); assert(!cd_frame_count && !dc_spu_cd_delay_tick(1));
    free(saved); free(cd_saved);
}

static void test_cd_events(void)
{
    cd_setup();
    uint64_t base = spu_cd_audio_pushed();
    const char *option = getenv("DC_TALK_RESET_OLD");
    int legacy = option && option[0] == '1';
    const int16_t pcm[] = {100, -200, -32768, 400, 2000, -3000};
    spu_cd_audio_push(pcm, 3);
    assert(spu_cd_audio_pushed() == base + 3);
    assert(spu_cd_audio_played() == base);
    assert(spu_cd_audio_peak(0, 1) == 200);
    assert(spu_cd_audio_peak(0, 2) == 32768);
    int16_t left, right;
    for (int i = 0; i < 3; ++i) assert(cd_audio_pop(&left, &right) && !left && !right);
    assert(spu_cd_audio_played() == base);
    assert(cd_audio_pop(&left, &right) && left == 100 && right == -200);
    assert(spu_cd_audio_played() == base + 1 && spu_cd_audio_peak(1, 0) == 200);
    uint32_t size = spu_cd_snapshot_bytes(); uint8_t *saved = malloc(size); assert(saved);
    spu_cd_snapshot_write(saved);
    assert(cd_audio_pop(&left, &right) && left == -32768);
    assert(spu_cd_audio_peak(1, 0) == 32768);
    assert(spu_cd_snapshot_read(saved, size));
    uint64_t end = legacy ? 2u : base + 5;
    assert(spu_cd_audio_pushed() == end && spu_cd_audio_played() == end - 2);
    assert(spu_cd_audio_peak(10, 0) == 0 && spu_cd_audio_peak(0, 1) == 32768);
    spu_cd_audio_reset();
    end = legacy ? 0u : base + 5;
    assert(spu_cd_audio_pushed() == end && spu_cd_audio_played() == end);
    assert(!spu_cd_audio_peak(UINT32_MAX, UINT32_MAX));
    free(saved);
}

int main(void)
{
    dc_spu_pages_backend_main();
    test_cd_events();
    test_overwritten_end(); test_guard_snapshots(); test_cd_arrival_time(); test_cd_delay_restore();
    puts("PASS: actual SPU overwrite guard, arrival-time CD padding and snapshot replay");
    return 0;
}
