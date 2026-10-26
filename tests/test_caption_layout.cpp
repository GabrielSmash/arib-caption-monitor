// Layout regression tests for the patched libaribcaption decoder (ABNT NBR 15606-1 Latin).
// Each vector is a full caption PES payload; the test checks the vertical position of every region.

#include <aribcaption/aribcaption.hpp>

#include <cstdio>
#include <string>
#include <vector>

namespace {

struct Vector {
    const char* name;
    const char* hex;
    std::vector<int> expectedY;
    std::vector<std::string> expectedText;
};

std::vector<uint8_t> fromHex(const std::string& hex) {
    std::vector<uint8_t> bytes;
    for (size_t i = 0; i + 1 < hex.size(); i += 2)
        bytes.push_back(static_cast<uint8_t>(std::stoi(hex.substr(i, 2), nullptr, 16)));
    return bytes;
}

bool run(const Vector& v) {
    aribcaption::Context context;
    aribcaption::Decoder decoder(context);
    decoder.Initialize(aribcaption::EncodingScheme::kABNT_NBR_15606_1_Latin);

    const auto data = fromHex(v.hex);
    aribcaption::DecodeResult result;
    if (decoder.Decode(data.data(), data.size(), 0, result) != aribcaption::DecodeStatus::kGotCaption) {
        std::printf("FAIL %s: no caption decoded\n", v.name);
        return false;
    }

    const auto& regions = result.caption->regions;
    bool ok = regions.size() == v.expectedY.size();
    for (size_t i = 0; ok && i < regions.size(); ++i) {
        std::string text;
        for (const auto& ch : regions[i].chars)
            text += ch.u8str;
        ok = regions[i].y == v.expectedY[i] && text == v.expectedText[i];
    }

    std::printf("%s %s\n", ok ? "PASS" : "FAIL", v.name);
    if (!ok) {
        for (const auto& region : regions) {
            std::string text;
            for (const auto& ch : region.chars)
                text += ch.u8str;
            std::printf("    y=%d [%s]\n", region.y, text.c_str());
        }
    }
    return ok;
}

}  // namespace

int main() {
    const std::vector<Vector> vectors = {
        // Globo (xqq/libaribcaption#27): three lines filled up to SDF width (608 = 32 cells), each followed by APR.
        {"globo-3-linhas-cheias",
         "80fff0040000009f3f00009b1f200000960c9b3720539b3136303b3939205f9b3630383b33393020569b33363b333620579b3220"
         "589b313620599b38206e881c4c408790500909090909090909094ee36f2c2067656e74652c20706f72206d696d2c2073650d0909"
         "09090909090909090909717569736572656d2066616c617220736f6272650d09090909090909090909096973736f2c206d656e"
         "6f722070726f626c656d612c2fb6",
         {411, 437, 463},
         {"Não, gente, por mim, se", "quiserem falar sobre", "isso, menor problema,"}},
        // SkylarBox encoder: SDF 760;44 = exactly the 38 cells of the first line, then APR.
        {"skylarbox-linha-cheia-apr",
         "80FFF00400000066000000621F2000005D0C9B3720539B3736303B343420569B31383B313820579B3220589B3420599B34303B"
         "343230205F8A9050876F7574726173207475726D61732C207475726D61203234322C206575206E656D2074656E686F0D7265"
         "6361646F206D616973A35D",
         {420, 442},
         {"outras turmas, turma 242, eu nem tenho", "recado mais"}},
    };

    int failures = 0;
    for (const auto& v : vectors)
        failures += run(v) ? 0 : 1;
    return failures == 0 ? 0 : 1;
}
