#include "dc_save_files.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

extern "C" void psx_fatal_halt(const char* reason);

namespace {

namespace fs = std::filesystem;

constexpr std::size_t kPathCapacity = 512;
constexpr int kReleaseSlotCount = 50;
constexpr int kMigrationSlotCount = 100;
constexpr char kSaveDirectory[] = "quicksaves";

int s_slot = -1;
int s_slot_count = 0;
char s_path[kPathCapacity];

int configured_slot_count() {
#ifdef DCME_RELEASE
    return kReleaseSlotCount;
#else
    if (s_slot_count > 0) return s_slot_count;

    const char* value = std::getenv("DC_SS_SLOTS");
    long parsed = 50;
    if (value && value[0]) {
        char* end = nullptr;
        errno = 0;
        parsed = std::strtol(value, &end, 10);
        if (errno != 0 || end == value) parsed = 50;
    }
    if (parsed < 1) parsed = 50;
    if (parsed > 100) parsed = 100;
    s_slot_count = static_cast<int>(parsed);
    return s_slot_count;
#endif
}

[[noreturn]] void fatal_path_error(const char* message) {
    std::fputs(message, stderr);
    std::fputc('\n', stderr);
    psx_fatal_halt(message);
    std::abort();
}

const char* publish_path(const std::string& value) {
    if (value.size() >= kPathCapacity) {
        fatal_path_error("save path exceeds 511 bytes");
    }
    std::memcpy(s_path, value.c_str(), value.size() + 1);
    return s_path;
}

#ifdef DCME_RELEASE
bool path_entry_exists(const fs::path& path, std::error_code& error) {
    const fs::file_status status = fs::symlink_status(path, error);
    if (error == std::errc::no_such_file_or_directory) {
        error.clear();
        return false;
    }
    if (error) return false;
    return status.type() != fs::file_type::not_found;
}

void migrate_legacy_file(const fs::path& directory, const std::string& name) {
    const fs::path source(name);
    const fs::path destination = directory / name;
    std::error_code ec;

    if (!path_entry_exists(source, ec) || ec) return;
    ec.clear();
    if (path_entry_exists(destination, ec) || ec) return;

#ifdef _WIN32
    std::wstring source_wide;
    std::wstring destination_wide;
    try {
        source_wide = fs::u8path(source.generic_string()).wstring();
        destination_wide = fs::u8path(destination.generic_string()).wstring();
    } catch (...) {
        return;
    }
    MoveFileExW(source_wide.c_str(), destination_wide.c_str(),
                MOVEFILE_WRITE_THROUGH);
#else
    ec.clear();
    if (!fs::is_regular_file(source, ec) || ec) return;

    ec.clear();
    fs::create_hard_link(source, destination, ec);
    if (ec) return;

    ec.clear();
    fs::remove(source, ec);
#endif
}

void migrate_legacy_saves(const fs::path& directory) {
    for (int slot = 0; slot < kMigrationSlotCount; ++slot) {
        std::string name;
        if (slot == 0) {
            name = "quicksave.dcss";
        } else {
            name = "quicksave" + std::to_string(slot) + ".dcss";
        }
        migrate_legacy_file(directory, name);
        migrate_legacy_file(directory, name + ".story");
    }
}

void ensure_release_directory() {
    static bool done = false;
    if (done) return;

    const fs::path directory(kSaveDirectory);
    std::error_code ec;
    const bool existed = fs::exists(directory, ec);
    if (ec) fatal_path_error("cannot access save directory");

    if (!existed) {
        fs::create_directory(directory, ec);
        if (ec) fatal_path_error("cannot create save directory");
    }

    ec.clear();
    if (!fs::is_directory(directory, ec) || ec) {
        fatal_path_error("save path is not a directory");
    }

    migrate_legacy_saves(directory);
    done = true;
}
#endif

}  // namespace

extern "C" int dc_save_slot_count(void) {
    return configured_slot_count();
}

extern "C" int dc_save_slot(void) {
    if (s_slot >= 0) return s_slot;

    s_slot = 0;
    const char* value = std::getenv("DC_SS_SLOT");
    if (value && value[0]) {
        char* end = nullptr;
        errno = 0;
        const long parsed = std::strtol(value, &end, 10);
        const int count = configured_slot_count();
        if (errno == 0 && end != value && parsed >= 0 && parsed < count) {
            s_slot = static_cast<int>(parsed);
        }
    }
    return s_slot;
}

extern "C" void dc_save_slot_set(int slot) {
    const int count = configured_slot_count();
    (void)dc_save_slot();
    slot %= count;
    if (slot < 0) slot += count;
    s_slot = slot;
}

extern "C" const char* dc_save_path(void) {
    const char* override_path = std::getenv("DC_SS_FILE");
    if (override_path && override_path[0]) {
        return publish_path(override_path);
    }

#ifdef DCME_RELEASE
    ensure_release_directory();
#endif

    const int slot = dc_save_slot();
    fs::path path;
#ifdef DCME_RELEASE
    path = fs::path(kSaveDirectory) /
           (slot == 0 ? "quicksave.dcss"
                      : "quicksave" + std::to_string(slot) + ".dcss");
#else
    path = slot == 0 ? fs::path("quicksave.dcss")
                     : fs::path("quicksave" + std::to_string(slot) + ".dcss");
#endif
    return publish_path(path.string());
}

extern "C" int dc_file_finish(FILE* file) {
    if (!file) return 0;

    int ok = std::ferror(file) == 0;
    if (ok && std::fflush(file) != 0) ok = 0;

#ifdef _WIN32
    if (ok && _commit(_fileno(file)) != 0) ok = 0;
#else
    if (ok) {
        const int descriptor = fileno(file);
        if (descriptor < 0 || fsync(descriptor) != 0) ok = 0;
    }
#endif

    if (std::fclose(file) != 0) ok = 0;
    return ok;
}

extern "C" int dc_replace_file(const char* temporary_path,
                                const char* destination_path) {
    if (!temporary_path || !destination_path) return 0;

#ifdef _WIN32
    std::wstring temporary;
    std::wstring destination;
    try {
        temporary = fs::u8path(temporary_path).wstring();
        destination = fs::u8path(destination_path).wstring();
    } catch (...) {
        return 0;
    }

    for (int attempt = 0; attempt < 25; ++attempt) {
        if (MoveFileExW(temporary.c_str(), destination.c_str(),
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            return 1;
        }

        const DWORD error = GetLastError();
        if (error != ERROR_SHARING_VIOLATION && error != ERROR_ACCESS_DENIED) {
            return 0;
        }
        if (attempt + 1 < 25) Sleep(20);
    }
    return 0;
#else
    return std::rename(temporary_path, destination_path) == 0;
#endif
}
