#include "dc_save_files.h"
#include "test_process.h"
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#ifdef _WIN32
#include <windows.h>
#endif

extern "C" void psx_fatal_halt(const char *message)
{ std::fprintf(stderr, "%s\n", message); std::abort(); }

namespace fs = std::filesystem;
static void put(const fs::path& path, const std::string& value)
{ std::ofstream out(path, std::ios::binary); out << value; out.close(); assert(out); }
static std::string get(const fs::path& path)
{ std::ifstream in(path, std::ios::binary); assert(in); return std::string(std::istreambuf_iterator<char>(in), {}); }
static void env(const char *name, const char *value)
{
#ifdef _WIN32
    assert(_putenv_s(name, value) == 0);
#else
    assert(setenv(name, value, 1) == 0);
#endif
}

int main(int argc, char **argv)
{
    test_process_init(); assert(argc == 2);
    fs::path root = fs::absolute(argv[1]); fs::create_directories(root);
    fs::path directory = root / std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    assert(fs::create_directory(directory)); fs::current_path(directory);
    env("DC_SS_SLOTS", "3"); env("DC_SS_SLOT", "2"); env("DC_SS_FILE", "");
    assert(dc_save_slot() == 2);
#ifdef DCME_RELEASE
    assert(dc_save_slot_count() == 50);
    fs::create_directory("quicksaves");
    put("quicksave.dcss", "legacy"); put("quicksave.dcss.story", "story");
    put("quicksave1.dcss", "preserved-source"); put("quicksaves/quicksave1.dcss", "preserved-target");
    put("quicksave99.dcss", "legacy-high-slot");
    assert(fs::path(dc_save_path()) == fs::path("quicksaves/quicksave2.dcss"));
    assert(get("quicksaves/quicksave.dcss") == "legacy" && !fs::exists("quicksave.dcss"));
    assert(get("quicksaves/quicksave.dcss.story") == "story" && !fs::exists("quicksave.dcss.story"));
    assert(get("quicksave1.dcss") == "preserved-source" && get("quicksaves/quicksave1.dcss") == "preserved-target");
    assert(get("quicksaves/quicksave99.dcss") == "legacy-high-slot" && !fs::exists("quicksave99.dcss"));
#else
    assert(dc_save_slot_count() == 3);
    assert(fs::path(dc_save_path()) == fs::path("quicksave2.dcss"));
    assert(!fs::exists("quicksaves"));
#endif
    dc_save_slot_set(-1); assert(dc_save_slot() == dc_save_slot_count() - 1);
    dc_save_slot_set(dc_save_slot_count()); assert(dc_save_slot() == 0);
    dc_save_slot_set(0);
    env("DC_SS_FILE", "custom/special.dcss"); assert(std::string(dc_save_path()) == "custom/special.dcss");
    env("DC_SS_FILE", "");

    put("saved.dcss", "original");
    FILE *file = std::fopen("pending.tmp", "wb"); assert(file);
    assert(std::fwrite("replacement", 1, 11, file) == 11 && dc_file_finish(file));
    assert(get("saved.dcss") == "original");
    assert(dc_replace_file("pending.tmp", "saved.dcss"));
    assert(get("saved.dcss") == "replacement" && !fs::exists("pending.tmp"));
    assert(!dc_replace_file("missing.tmp", "saved.dcss") && get("saved.dcss") == "replacement");
    file = std::fopen("saved.dcss", "rb"); assert(file);
    assert(std::fwrite("bad", 1, 3, file) == 0); assert(!dc_file_finish(file));
    assert(get("saved.dcss") == "replacement");
    fs::path unicode = fs::u8path("saved_\xE6\xB8\xB8.dcss");
    put(unicode, "unicode-original"); put("unicode.tmp", "unicode-replacement");
    assert(dc_replace_file("unicode.tmp", unicode.u8string().c_str()));
    assert(get(unicode) == "unicode-replacement");
#ifdef _WIN32
    put("locked.tmp", "later");
    HANDLE locked = CreateFileW(L"saved.dcss", GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    assert(locked != INVALID_HANDLE_VALUE);
    assert(!dc_replace_file("locked.tmp", "saved.dcss"));
    assert(CloseHandle(locked));
    assert(get("saved.dcss") == "replacement" && get("locked.tmp") == "later");
#endif
    std::puts("PASS: actual save slot paths, migration collisions and durable replacement failures");
    return 0;
}
