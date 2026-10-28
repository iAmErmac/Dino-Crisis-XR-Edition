#include "dc_save_session.h"
#include "dc_save_files.h"
#include "boot_state.h"
#include "interrupts.h"
#include "psx_cycles.h"
#include "psx_scheduler.h"
#include "dirty_ram_interp.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <climits>

extern "C" int psx_game_is_function_entry(uint32_t pc);
extern "C" int psx_mod_function_entry_active(void);
extern "C" void psx_fatal_halt(const char *reason);
extern "C" int g_call_unit_depth;
extern "C" int g_psx_dispatch_depth;
extern "C" { int g_savestate_loads = 0; }

namespace {
std::atomic<int> pending{0};
std::atomic<unsigned long long> completion_generation{0};
std::atomic<int> last_ok{-1};
std::atomic<int> last_load{-1};
std::atomic<int> last_slot{-1};
std::mutex request_mutex;
uint32_t checksum, entry;
int configured, request_load, request_slot;
char request_path[544];
thread_local char display_path[544];

int resume_valid(uint32_t pc) {
    if (!pc || (pc & 3u)) return 0;
    const uint32_t phys = pc & 0x1FFFFFFFu;
    if (phys == 0x80u || phys == 0x180u || phys == 0u) return 0;
    if (psx_game_is_function_entry(pc)) return 1;
    return phys < 0x200000u && dirty_ram_is_dirty(phys);
}

int write_image(const char *path, const uint8_t *data, size_t size) {
    char temporary[552];
    if (std::snprintf(temporary, sizeof temporary, "%s.tmp", path) >= (int)sizeof temporary) return 0;
    FILE *file = std::fopen(temporary, "wb");
    if (!file) return 0;
    int ok = std::fwrite(data, 1, size, file) == size;
    if (!dc_file_finish(file)) ok = 0;
    if (ok) ok = dc_replace_file(temporary, path);
    if (!ok) std::remove(temporary);
    return ok;
}
}

extern "C" const char *dc_save_session_path(void) {
    const char *base = dc_save_path();
    if (std::snprintf(display_path, sizeof display_path, "%s.tcb.pst", base) >= (int)sizeof display_path)
        return nullptr;
    return display_path;
}

extern "C" void dc_save_session_configure(uint32_t key, uint32_t pc) {
    std::lock_guard<std::mutex> lock(request_mutex);
    if (pending.load()) return;
    checksum = key; entry = pc; configured = pc != 0;
}

extern "C" int dc_save_session_pending(void) { return pending.load(); }

extern "C" int dc_save_session_request_slot(int load, int slot) {
    std::lock_guard<std::mutex> lock(request_mutex);
    if (!configured || pending.load()) return 0;
    if (slot < 0 || slot >= dc_save_slot_count()) return 0;
    dc_save_slot_set(slot);
    const char *path = dc_save_session_path();
    if (!path) return 0;
    std::memcpy(request_path, path, std::strlen(path) + 1);
    request_load = load != 0; request_slot = dc_save_slot();
    pending.store(1);
    return 1;
}

extern "C" int dc_save_session_request(int load) {
    return dc_save_session_request_slot(load, dc_save_slot());
}

extern "C" void dc_save_session_status_json(char *out, size_t cap) {
    if (!out || !cap) return;
    const int slot_count = dc_save_slot_count();
    std::snprintf(out, cap,
                  "\"session\":\"dc_tcb\",\"configured\":%d,"
                  "\"pending\":%d,\"generation\":%llu,\"last_ok\":%d,"
                  "\"last_load\":%d,\"last_slot\":%d,\"slot\":%d,"
                  "\"slot_count\":%d",
                  configured ? 1 : 0, pending.load(),
                  completion_generation.load(), last_ok.load(), last_load.load(),
                  last_slot.load(), dc_save_slot(), slot_count);
}

extern "C" void dc_save_session_poll(CPUState *cpu, uint32_t pc) {
    if (pending.load() != 1 || !cpu || psx_get_in_exception() ||
        g_call_unit_depth || g_psx_dispatch_depth > 1 ||
        psx_snapshot_host_call_depth() || psx_mod_function_entry_active() ||
        !resume_valid(pc) || !psx_irq_resume_context_snapshot_safe_at(pc)) return;
    int expected = 1;
    if (!pending.compare_exchange_strong(expected, 2)) return;
    const int load = request_load, slot = request_slot;
    uint8_t *image = nullptr; size_t bytes = 0;
    CPUState before = *cpu; before.pc = pc;
    int restored = 0;
    int ok = load ? boot_state_save_buffer_raw(&before, checksum, entry, &image, &bytes)
                  : boot_state_save_buffer(&before, checksum, entry, &image, &bytes);
    if (ok && !load) ok = write_image(request_path, image, bytes);
    if (ok && load) {
        ok = boot_state_load(request_path, checksum, entry, cpu);
        if (ok) ok = resume_valid(cpu->pc);
        if (!ok && !boot_state_load_buffer(image, bytes, checksum, entry, cpu)) {
            std::free(image); pending.store(0);
            psx_fatal_halt("save restore rollback failed");
            std::abort();
        }
        restored = 1;
    }
    std::free(image);
    pending.store(0);
    last_ok.store(ok);
    last_load.store(load);
    last_slot.store(slot);
    completion_generation.fetch_add(1);
    if (restored) {
        g_savestate_loads = g_savestate_loads == INT_MAX ? 1 : g_savestate_loads + 1;
        psx_cycles_resync_after_restore(cpu);
        interrupts_resync_after_restore();
        dc_save_session_loaded();
    }
    dc_save_session_notify(load, slot, ok);
    /* All owned buffers and locks are released before the scheduler escape. */
    if (restored) psx_scheduler_resume_at(cpu->pc);
}
