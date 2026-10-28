#include "iso_reader.h"
#include <array>
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <vector>

static void option(const char *value) {
#ifdef _WIN32
    assert(_putenv_s("DC_SBI", value) == 0);
#else
    if (value[0]) assert(setenv("DC_SBI", value, 1) == 0);
    else assert(unsetenv("DC_SBI") == 0);
#endif
}
static void write(const std::filesystem::path &path, const std::vector<uint8_t> &bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    file.close(); assert(file.good());
}
static void replacement(PS1::ISOReader &reader, uint8_t expected) {
    std::array<uint8_t, 12> q = {}; bool valid = true;
    assert(reader.HasSubChannelReplacements());
    assert(reader.ReadSubChannelQ(0, q.data(), &valid));
    assert(!valid && q[3] == expected);
}
int main() {
    namespace fs = std::filesystem;
    const auto root = fs::current_path() / "dc_iso_policy_fixture";
    fs::create_directories(root);
    const auto image = root / "protected (Track 1).bin";
    const auto basename = root / "protected (Track 1).sbi";
    const auto shared = root / "protected.sbi";
    const auto override_path = root / "override.sbi";
    write(image, std::vector<uint8_t>(2352 * 20));
    std::vector<uint8_t> data = {'S','B','I',0,0,2,0,1,0x41,1,1,0x23,6,5,0,3,8,1};
    write(basename, data); data[11] = 0x24; write(shared, data);
    PS1::ISOReader reader;
    option(""); assert(reader.Open(image.string())); replacement(reader, 0x24); reader.Close();
    fs::remove(shared);
    assert(reader.Open(image.string())); replacement(reader, 0x23); reader.Close();
    data[11] = 0x25; write(override_path, data);
    option(override_path.string().c_str());
    assert(reader.Open(image.string())); replacement(reader, 0x25); reader.Close();
    option("0"); assert(reader.Open(image.string())); assert(!reader.HasSubChannelReplacements()); reader.Close();
    option((root / "missing.sbi").string().c_str()); assert(!reader.Open(image.string()));
    write(override_path, {'B','A','D'}); option(override_path.string().c_str()); assert(!reader.Open(image.string()));
    option("1"); assert(reader.Open(image.string())); replacement(reader, 0x23); reader.Close();
    fs::remove(basename); option(""); assert(reader.Open(image.string()));
    assert(!reader.HasSubChannelReplacements()); reader.Close();
    fs::remove(image); fs::remove(override_path); fs::remove(root);
    return 0;
}
