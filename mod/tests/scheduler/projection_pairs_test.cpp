#include "dc_projection_pairs.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>

extern "C" {
int g_dc_pass_id;
uint32_t pgxp_pair_of(float x, float y);
uint32_t dc_pair_run_add(int pass, uint32_t key, const float matrix[9], const float translation[3]);
int dc_pair_pass_get(int pass, int *projections);
int dc_pair_run_get(int pass, int index, uint32_t *key, int *vertices, double matrix[9], double translation[3]);
void dc_pair_veto(int pass);
int dc_pair_vetoed(int pass);
}

static const int16_t identity[9] = {4096,0,0, 0,4096,0, 0,0,4096};
static const int32_t origin[3] = {0,0,0};

static uint32_t note(const int16_t vertex[3])
{
    return dc_pair_projection_note(identity, origin, vertex, 0, nullptr, nullptr);
}

static uint32_t vertex_key(const int16_t vertices[][3], int count)
{
    uint32_t key = 2166136261u;
    for (int v = 0; v < count; ++v)
        for (int i = 0; i < 3; ++i)
            key = (key ^ static_cast<uint16_t>(vertices[v][i])) * 16777619u;
    return key;
}

int main()
{
    const int16_t vertices[4][3] = {{1,-2,3}, {4,5,-6}, {-7,8,9}, {31,32,33}};
    double matrix[9], translation[3];
    uint32_t key;
    int count, projections;

    g_dc_pass_id = 101;
    g_dc_pair_track = 0;
    assert(note(vertices[0]) == 0 && g_dc_pair_last == 0);
    g_dc_pair_track = 1;
    assert(note(vertices[1]) != 0);
    assert(dc_pair_pass_get(101, &projections) == -1);

    g_dc_pass_id = 102;
    const uint32_t first = note(vertices[0]);
    for (int i = 1; i < 4; ++i) assert(note(vertices[i]) == first);
    assert(dc_pair_pass_get(102, &projections) == 1 && projections == 4);
    assert(dc_pair_run_get(102, 0, &key, &count, matrix, translation));
    assert(key == vertex_key(vertices, 3) && count == 4);
    for (int i = 0; i < 9; ++i) assert(matrix[i] == (i % 4 == 0 ? 1.0 : 0.0));
    int16_t changed[9];
    std::memcpy(changed, identity, sizeof changed);
    changed[0] = 2048;
    assert(dc_pair_projection_note(changed, origin, vertices[0], 0, nullptr, nullptr) != first);
    assert(dc_pair_pass_get(102, &projections) == 2 && projections == 5);

    g_dc_pass_id = 103;
    const int16_t raw[9] = {4096,8192,0, 0,4096,12288, 16384,0,4096};
    const int32_t offset[3] = {10,20,30};
    const float delta[9] = {0,1,0, 1,0,0, 0,0,-1};
    const float shift[3] = {3,4,5};
    int16_t raw_before[9], vertex_before[3];
    int32_t offset_before[3];
    std::memcpy(raw_before, raw, sizeof raw);
    std::memcpy(offset_before, offset, sizeof offset);
    std::memcpy(vertex_before, vertices[0], sizeof vertex_before);
    const uint32_t aimed = dc_pair_projection_note(raw, offset, vertices[0], 1, delta, shift);
    assert(aimed && dc_pair_run_get(103, 0, &key, &count, matrix, translation));
    const double expected[9] = {0,1,3, 1,2,0, -4,0,-1};
    const double expected_translation[3] = {23,14,-25};
    assert(std::memcmp(matrix, expected, sizeof expected) == 0);
    assert(std::memcmp(translation, expected_translation, sizeof expected_translation) == 0);
    assert(std::memcmp(raw_before, raw, sizeof raw) == 0);
    assert(std::memcmp(offset_before, offset, sizeof offset) == 0);
    assert(std::memcmp(vertex_before, vertices[0], sizeof vertex_before) == 0);
    assert(delta[1] == 1 && shift[0] == 3);

    const float x = 1.25f, y = -3.5f;
    dc_pair_projection_tag(aimed, x, y);
    assert(pgxp_pair_of(x, y) == aimed);
    assert(pgxp_pair_of(std::nextafter(x, 2.0f), y) == 0);
    dc_pair_projection_tag(aimed, 0.0f, 123.0f);
    assert(pgxp_pair_of(0.0f, 123.0f) == aimed);
    assert(pgxp_pair_of(-0.0f, 123.0f) == 0);
    g_dc_pass_id = 104;
    assert(pgxp_pair_of(x, y) == aimed);
    g_dc_pass_id = 105;
    assert(pgxp_pair_of(x, y) == 0);

    g_dc_pass_id = 106;
    const float room[9] = {2,1,0, 0,3,1, 1,0,4};
    const float room_offset[3] = {7,8,9};
    const uint32_t custom = dc_pair_run_add(106, 0x12345678u, room, room_offset);
    assert(custom && dc_pair_run_get(106, 0, &key, &count, matrix, translation));
    assert(key == 0x12345678u && count == 0);
    for (int i = 0; i < 9; ++i) assert(matrix[i] == room[i]);
    for (int i = 0; i < 3; ++i) assert(translation[i] == room_offset[i]);
    const uint32_t geometry = note(vertices[0]);
    assert(geometry && geometry != custom);
    assert(dc_pair_pass_get(106, &projections) == 2 && projections == 1);
    assert(dc_pair_run_add(107, 1, room, room_offset) == 0);
    assert(dc_pair_run_add(104, 1, room, room_offset) == 0);
    assert(!dc_pair_vetoed(106));
    dc_pair_veto(106);
    assert(dc_pair_vetoed(106) && !dc_pair_vetoed(110));

    g_dc_pass_id = 107;
    for (int i = 0; i < 1024; ++i) {
        changed[0] = static_cast<int16_t>(4096 + i);
        assert(dc_pair_projection_note(changed, origin, vertices[0], 0, nullptr, nullptr));
    }
    assert(dc_pair_pass_get(107, &projections) == 1024 && projections == 1024);
    changed[0] = 6000;
    assert(dc_pair_projection_note(changed, origin, vertices[0], 0, nullptr, nullptr) == 0);
    assert(g_dc_pair_last == 0 && dc_pair_pass_get(107, &projections) == -1);
    assert(!dc_pair_run_get(107, 1024, nullptr, nullptr, matrix, translation));
    dc_pair_veto(107);
    dc_pair_projection_tag(first, x, y);
    dc_pair_projection_reset();
    assert(g_dc_pair_track == 1 && g_dc_pair_last == 0);
    assert(!dc_pair_vetoed(107) && pgxp_pair_of(x, y) == 0);
    assert(dc_pair_pass_get(107, &projections) == -1);
    assert(note(vertices[0]));
    assert(dc_pair_pass_get(107, &projections) == 1 && projections == 1);
    std::puts("projection pairs passed");
}
