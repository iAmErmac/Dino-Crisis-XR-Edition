#include "dc_save_session.h"
#include "dc_save_files.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csetjmp>
#include <filesystem>

extern "C" int g_savestate_loads;
static std::jmp_buf escape;
static int exception, host_depth, plugin, safe = 1, dirty;
static int save_ok = 1, load_ok = 1, rollback_ok = 1, valid_load = 1;
static int saves, loads, rollbacks, resets, notices, status, fatal;
static uint32_t resumed;
extern "C" {
int g_call_unit_depth, g_psx_dispatch_depth;
int psx_get_in_exception(void) { return exception; }
unsigned psx_snapshot_host_call_depth(void) { return (unsigned)host_depth; }
int psx_mod_function_entry_active(void) { return plugin; }
int psx_irq_resume_context_snapshot_safe_at(uint32_t) { return safe; }
int psx_game_is_function_entry(uint32_t pc) { return pc == 0x80010000u; }
int dirty_ram_is_dirty(uint32_t) { return dirty; }
void psx_cycles_resync_after_restore(CPUState*) { ++resets; }
void interrupts_resync_after_restore(void) { ++resets; }
void dc_save_session_loaded(void) { ++resets; }
void dc_save_session_notify(int, int, int ok) { ++notices; status = ok; }
void psx_scheduler_resume_at(uint32_t pc) { resumed = pc; std::longjmp(escape, 1); }
void psx_fatal_halt(const char*) { ++fatal; std::longjmp(escape, 2); }
int boot_state_save_buffer(const CPUState *cpu, uint32_t, uint32_t, uint8_t **out, size_t *len) {
    ++saves;
    if (!save_ok) return 0;
    *len = sizeof(uint32_t); *out = (uint8_t*)std::malloc(*len);
    assert(*out); std::memcpy(*out, &cpu->pc, *len); return 1;
}
int boot_state_save_buffer_raw(const CPUState *cpu, uint32_t key, uint32_t entry, uint8_t **out, size_t *len) {
    return boot_state_save_buffer(cpu, key, entry, out, len);
}
int boot_state_load(const char*, uint32_t, uint32_t, CPUState *cpu) {
    ++loads; cpu->gpr[2] = 0xBAD; cpu->pc = valid_load ? 0x80010000u : 0xDEADBEEFu;
    return load_ok;
}
int boot_state_load_buffer(const uint8_t *data, size_t len, uint32_t, uint32_t, CPUState *cpu) {
    ++rollbacks; assert(len == sizeof(uint32_t));
    if (!rollback_ok) return 0;
    std::memcpy(&cpu->pc, data, sizeof(uint32_t)); cpu->gpr[2] = 0; return 1;
}
}

static void poll_load(CPUState *cpu, int expected_jump) {
    int code = setjmp(escape);
    if (!code) { dc_save_session_poll(cpu, 0x80010000u); assert(false); }
    assert(code == expected_jump);
    assert(!dc_save_session_pending());
}

int main() {
    namespace fs = std::filesystem;
    const fs::path root = fs::current_path() / "save-session-files";
    fs::create_directories(root);
    const std::string original = (root / "quicksave.dcss").string();
#ifdef _WIN32
    _putenv_s("DC_SS_FILE", original.c_str());
#else
    setenv("DC_SS_FILE", original.c_str(), 1);
#endif
    FILE *old = std::fopen(original.c_str(), "wb"); assert(old);
    assert(std::fwrite("KEEP", 1, 4, old) == 4); assert(std::fclose(old) == 0);
    assert(!dc_save_session_request(0));
    dc_save_session_configure(17, 0x80010000u);
    CPUState cpu = {};
    assert(dc_save_session_request(0)); assert(!dc_save_session_request(1));
    int *guards[] = {&exception, &host_depth, &plugin, &g_call_unit_depth};
    for (int *guard : guards) {
        *guard = 1; dc_save_session_poll(&cpu, 0x80010000u); *guard = 0;
        assert(!saves && dc_save_session_pending());
    }
    g_psx_dispatch_depth = 2; dc_save_session_poll(&cpu, 0x80010000u); g_psx_dispatch_depth = 0;
    safe = 0; dc_save_session_poll(&cpu, 0x80010000u); safe = 1;
    dc_save_session_poll(&cpu, 0); dc_save_session_poll(&cpu, 0x80010001u);
    assert(!saves && dc_save_session_pending());
    dc_save_session_poll(&cpu, 0x80010000u);
    assert(saves == 1 && notices == 1 && status && !dc_save_session_pending());
    assert(fs::file_size(original + ".tcb.pst") == sizeof(uint32_t));
    char keep[4]; old = std::fopen(original.c_str(), "rb"); assert(old);
    assert(std::fread(keep, 1, 4, old) == 4); std::fclose(old); assert(!std::memcmp(keep, "KEEP", 4));
    save_ok = 0; assert(dc_save_session_request(0)); dc_save_session_poll(&cpu, 0x80010000u);
    assert(!status && !dc_save_session_pending()); save_ok = 1;
    assert(dc_save_session_request(1)); poll_load(&cpu, 1);
    assert(g_savestate_loads == 1);
    assert(loads == 1 && !rollbacks && resets == 3 && resumed == 0x80010000u && status);
    load_ok = 0; assert(dc_save_session_request(1)); poll_load(&cpu, 1);
    assert(g_savestate_loads == 2);
    assert(rollbacks == 1 && !status && cpu.gpr[2] == 0 && resets == 6);
    load_ok = 1; valid_load = 0; assert(dc_save_session_request(1)); poll_load(&cpu, 1);
    assert(rollbacks == 2 && !status && resumed == 0x80010000u);
    rollback_ok = 0; assert(dc_save_session_request(1)); poll_load(&cpu, 2); assert(fatal == 1);
    valid_load = 1; rollback_ok = 1;
    assert(dc_save_session_request(0)); dirty = 1; dc_save_session_poll(&cpu, 0x80020000u);
    assert(!dc_save_session_pending() && status);
    std::remove((original + ".tcb.pst").c_str()); std::remove(original.c_str());
    return 0;
}
