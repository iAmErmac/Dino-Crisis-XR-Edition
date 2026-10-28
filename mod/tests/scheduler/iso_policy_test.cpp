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

static void write(const std::filesystem::path &path,
                  const std::vector<uint8_t> &bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    file.close();
    assert(file.good());
}

static std::vector<uint8_t> sbi(uint8_t marker) {
    return {'S','B','I',0,0,2,0,1,0x41,1,1,marker,6,5,0,3,8,1};
}

static std::vector<uint8_t> lsd(uint8_t marker, bool valid_time = true) {
    std::vector<uint8_t> data(15, 0);
    data[0] = valid_time ? 0x00 : 0x6A;
    data[1] = 0x02;
    data[2] = 0x00;
    data[3] = 0x41;
    data[4] = 0x01;
    data[5] = 0x01;
    data[6] = marker;
    data[7] = 0x06;
    data[8] = 0x05;
    data[9] = 0x00;
    data[10] = 0x03;
    data[11] = 0x08;
    data[12] = 0x01;
    data[13] = 0x00;
    data[14] = 0x00;
    return data;
}

static void replacement(PS1::ISOReader &reader, uint8_t expected) {
    std::array<uint8_t, 12> q = {};
    bool valid = true;
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
    const auto lsd_path = root / "override.lsd";
    const auto malformed_path = root / "malformed.lsd";
    write(image, std::vector<uint8_t>(2352 * 20));
    write(basename, sbi(0x23));
    write(shared, sbi(0x24));

    PS1::ISOReader reader;
    option("");
    assert(reader.Open(image.string()));
    replacement(reader, 0x24);              // Track-name fallback wins.
    reader.Close();

    fs::remove(shared);
    assert(reader.Open(image.string()));
    replacement(reader, 0x23);              // Existing SBI behavior remains.
    reader.Close();

    write(override_path, sbi(0x25));
    option("");
    PS1::ISOReader explicit_reader;
    explicit_reader.SetSubchannelPathOverride(override_path.string());
    assert(explicit_reader.Open(image.string()));
    replacement(explicit_reader, 0x25);     // Explicit API beats name lookup.
    explicit_reader.Close();

    option("0");
    PS1::ISOReader disabled_reader;
    disabled_reader.SetSubchannelPathOverride(override_path.string());
    assert(disabled_reader.Open(image.string()));
    assert(!disabled_reader.HasSubChannelReplacements());
    disabled_reader.Close();                // DC_SBI=0 disables all SBI selection.

    option("1");
    write(lsd_path, lsd(0x26));
    PS1::ISOReader lsd_reader;
    lsd_reader.SetSubchannelPathOverride(lsd_path.string());
    assert(lsd_reader.Open(image.string()));
    replacement(lsd_reader, 0x26);          // Valid LSD record is accepted.
    lsd_reader.Close();

    write(malformed_path, lsd(0x27, false));
    PS1::ISOReader malformed_reader;
    malformed_reader.SetSubchannelPathOverride(malformed_path.string());
    assert(!malformed_reader.Open(image.string()));
    assert(!malformed_reader.HasSubChannelReplacements());

    PS1::ISOReader missing_reader;
    missing_reader.SetSubchannelPathOverride((root / "missing.sbi").string());
    assert(!missing_reader.Open(image.string()));

    write(override_path, {'B','A','D'});
    PS1::ISOReader bad_sbi_reader;
    bad_sbi_reader.SetSubchannelPathOverride(override_path.string());
    assert(!bad_sbi_reader.Open(image.string()));

    option("1");
    assert(reader.Open(image.string()));
    replacement(reader, 0x23);              // Auto SBI still works after failures.
    reader.Close();

    fs::remove(basename);
    option("");
    assert(reader.Open(image.string()));
    assert(!reader.HasSubChannelReplacements());
    reader.Close();

    option("");
    fs::remove(image);
    fs::remove(override_path);
    fs::remove(lsd_path);
    fs::remove(malformed_path);
    fs::remove(root);
    return 0;
}
