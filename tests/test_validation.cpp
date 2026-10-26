// Checks AribInspector + CaptionValidator against real caption PES payloads.

#include "AribInspector.h"
#include "CaptionValidator.h"

#include <cstdio>

namespace {

int failures = 0;

void expect(bool condition, const char* what) {
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", what);
    if (!condition)
        ++failures;
}

const CaptionValidator::Rule& rule(const CaptionValidator& v, const QString& id) {
    for (const auto& r : v.rules())
        if (r.id == id)
            return r;
    std::printf("regra inexistente: %s\n", qPrintable(id));
    std::abort();
}

AribDataGroup inspectHex(const char* hex) {
    return inspectAribPes(QByteArray::fromHex(hex));
}

}  // namespace

int main() {
    // Globo (xqq/libaribcaption#27): valid CRC computed with initial value 0.
    const AribDataGroup globo = inspectHex(
        "80fff0040000009f3f00009b1f200000960c9b3720539b3136303b3939205f9b3630383b33393020569b33363b333620579b3220"
        "589b313620599b38206e881c4c408790500909090909090909094ee36f2c2067656e74652c20706f72206d696d2c2073650d0909"
        "09090909090909090909717569736572656d2066616c617220736f6272650d09090909090909090909096973736f2c206d656e"
        "6f722070726f626c656d612c2fb6");
    expect(globo.valid, "globo: data group válido");
    expect(globo.crcPresent && globo.crcValid, "globo: CRC-16 válido");
    expect(globo.charsetIssues.isEmpty(), "globo: caracteres dentro das tabelas Latin");
    expect(globo.ssmWidth == 36 && globo.sdfWidth == 608 && globo.sdpY == 99, "globo: SSM/SDF/SDP lidos");

    // SkylarBox statement: CRC sent was computed with initial value 0xFFFF.
    const AribDataGroup skylar = inspectHex(
        "80FFF00400000066000000621F2000005D0C9B3720539B3736303B343420569B31383B313820579B3220589B3420599B34303B"
        "343230205F8A9050876F7574726173207475726D61732C207475726D61203234322C206575206E656D2074656E686F0D7265"
        "6361646F206D616973A35D");
    expect(skylar.valid, "skylarbox: data group válido");
    expect(skylar.crcPresent && !skylar.crcValid, "skylarbox: CRC-16 inválido");
    expect(skylar.crcSent == 0xA35D && skylar.crcExpected == 0xF6B9 && skylar.crcWithInitFfff == 0xA35D,
           "skylarbox: CRC enviado corresponde ao valor inicial 0xFFFF");

    // SkylarBox management: por, Format 1000, TCS 00, rollup 00.
    const AribDataGroup management = inspectHex("80FFF0000000000A3F011A706F72800000006A78");
    expect(management.management && management.languages.size() == 1, "gerenciamento: 1 idioma");
    expect(management.languages.value(0).iso639 == "por" && management.languages.value(0).format == 8,
           "gerenciamento: por, Format 1000");

    // Latin Special (G3) via SS3: 0x21 (♪) is defined, 0x22 is not.
    AribDataGroup g3;
    {
        // Statement body: SS3 0x21, SS3 0x22 (the CRC is irrelevant for the charset check).
        const QByteArray body = QByteArray::fromHex("1D211D22");
        QByteArray units = QByteArray::fromHex("1F20") + QByteArray(1, 0) + QByteArray(1, 0) + QByteArray(1, body.size()) + body;
        QByteArray data = QByteArray(1, 0) + QByteArray(1, 0) + QByteArray(1, 0) + QByteArray(1, units.size()) + units;
        QByteArray pes = QByteArray::fromHex("80FFF0") + QByteArray(1, 0x04) + QByteArray(1, 0) + QByteArray(1, 0) +
                         QByteArray(1, 0) + QByteArray(1, data.size()) + data + QByteArray(2, 0);
        g3 = inspectAribPes(pes);
    }
    expect(g3.valid && g3.charsetIssues.size() == 1 && g3.charsetIssues[0].contains("0x22"),
           "Latin Special: só a posição indefinida 0x22 é acusada");

    CaptionValidator validator;
    validator.checkDataGroup(globo);
    validator.checkDataGroup(skylar);
    validator.checkDataGroup(management);
    expect(rule(validator, "dg.crc").checks == 3 && rule(validator, "dg.crc").failures == 2,
           "validador: CRC falha nos 2 data groups do SkylarBox e passa no da Globo");
    expect(rule(validator, "dg.crc").recent.value(0).detail.contains("0xFFFF"), "validador: dica do valor inicial 0xFFFF");
    expect(rule(validator, "txt.ssm").failures == 1, "validador: SSM 18 acusado como aviso [ARIB/Japão]");
    expect(rule(validator, "mgmt.iso639").failures == 0, "validador: idioma por aceito");

    // Layout: second line outside the display area, and full line followed by another line.
    validator.checkLayout({960, 540}, QRect(40, 420, 760, 44), {QRect(40, 420, 760, 22), QRect(40, 464, 220, 22)},
                          {"outras turmas, turma 242, eu nem tenho", "recado mais"});
    expect(rule(validator, "lay.text_in_area").failures == 1, "layout: linha fora da área acusada");
    expect(rule(validator, "lay.full_line_apr").failures == 1, "layout: linha cheia + nova linha acusada");
    expect(rule(validator, "acc.chars_per_line").failures == 1, "layout: linha com mais de 32 caracteres (informativo)");

    return failures == 0 ? 0 : 1;
}
