#include "CaptionValidator.h"

#include "TsDemux.h"

#include <QSet>

namespace {

constexpr int kMaxRecent = 20;
constexpr qint64 kManagementIntervalMs = 3 * 60 * 1000;
constexpr int kMaxPesPayload = 32 * 1024;

const QString kSrcArib = "[ARIB] STD-B24";
const QString kSrcAribJp = "[ARIB/Japão] TR-B14 — operação japonesa, não confirmada para o Brasil";
const QString kSrcCode = "[CÓDIGO] libaribcaption (tabelas ABNT; NBR não cotejada)";
const QString kSrcNbr15290 = "[SECUNDÁRIA] ABNT NBR 15290 (não é a NBR 15610-1)";

QString hex(int value, int width = 2) {
    return "0x" + QString("%1").arg(value, width, 16, QChar('0')).toUpper();
}

bool isCaptionTag(int tag) { return (tag >= 0x30 && tag <= 0x37) || tag == 0x87; }

}  // namespace

CaptionValidator::CaptionValidator() {
    using S = Severity;
    // PMT
    addRule("pmt.caption", S::Error, "PMT anuncia um stream de legenda", kSrcAribJp + " §4.2.8");
    addRule("pmt.descriptors", S::Error, "Stream de legenda com descritores 0x52 (stream identifier) e 0xFD (data component)",
            kSrcAribJp + " Tab. 4-8");
    addRule("pmt.component_tag", S::Error, "component_tag de legenda 0x30–0x37 (0x87 em one-seg)", kSrcAribJp + " §4.2.8.1");
    addRule("pmt.data_component_id", S::Error, "data_component_id 0x0008 (0x0012 em one-seg)",
            kSrcArib + " §9.6; [CAPTURA-REAL] Globo");
    // PES / data group
    addRule("pes.header", S::Error, "data_identifier 0x80/0x81 e private_stream_id 0xFF", kSrcArib + " Vol.3 §5");
    addRule("pes.size", S::Error, "PES de legenda ≤ 32 KB", kSrcAribJp + " Tab. 4-3");
    addRule("dg.id", S::Error, "data_group_id válido (0x00/0x20 gerenciamento; 0x01–0x08/0x21–0x28 texto)",
            kSrcArib + " Tab. 9-2");
    addRule("dg.size", S::Error, "data_group_size coerente com o tamanho do PES", kSrcArib + " cap. 9");
    addRule("dg.crc", S::Error, "CRC-16 do data group (x¹⁶+x¹²+x⁵+1, valor inicial 0)",
            kSrcArib + " Tab. 9-1; [CAPTURA-REAL] Globo valida com valor inicial 0");
    addRule("dg.units", S::Error, "Data units bem formados (unit_separator 0x1F)", kSrcArib + " Tab. 9-11");
    addRule("dg.tmd", S::Error, "TMD diferente de 11 (reservado)", kSrcArib + " Tab. 9-4");
    // Management
    addRule("mgmt.languages", S::Error, "num_languages entre 1 e 2", kSrcAribJp + " Tab. 4-4; [CÓDIGO] rejeita 0 e >2");
    addRule("mgmt.iso639", S::Error, "Idioma \"por\"", kSrcCode + " — \"por\" seleciona a tabela Latin");
    addRule("mgmt.tcs", S::Error, "TCS = 00 (código de 8 bits)", kSrcArib + " cap. 9; " + kSrcCode);
    addRule("mgmt.rollup", S::Error, "rollup_mode 00 ou 01", kSrcArib + " Tab. 9-9");
    addRule("mgmt.format", S::Warning, "Format 1000/1001/1010/1011 (960×540 ou 720×480)", kSrcAribJp + " Tab. 4-4");
    addRule("mgmt.interval", S::Warning, "Gerenciamento reenviado a cada 3 min no máximo", kSrcAribJp + " §4.2.5.1");
    // Statement body
    addRule("txt.charset", S::Error, "Caracteres dentro das tabelas Latin (alfanumérico, Latin Extension, Latin Special)",
            kSrcCode);
    addRule("txt.forbidden", S::Warning, "Sem códigos não usados (BEL, CAN, RS, CDC, WMM, MACRO, PLD, PLU, CCC, TCC, CFS, MDF, XCS)",
            kSrcAribJp + " §4.5");
    addRule("txt.ssm", S::Warning, "SSM em 16, 20, 24, 30 ou 36", kSrcAribJp + " Tab. 4-20");
    addRule("txt.swf", S::Warning, "SWF 7, 8, 9 ou 10 (960×540 ou 720×480)", kSrcAribJp + " Tab. 4-20");
    addRule("txt.scr", S::Info, "SCR (scroll) não utilizado — o monitor não simula scroll", "[CÓDIGO] libaribcaption ignora SCR");
    // Layout
    addRule("lay.area_in_plane", S::Error, "Área de exibição (SDF/SDP) dentro do plano", kSrcArib + " (plano/área de exibição)");
    addRule("lay.text_in_plane", S::Error, "Texto dentro do plano de legenda", kSrcArib + " (plano/área de exibição)");
    addRule("lay.text_in_area", S::Error, "Texto dentro da área de exibição (SDF/SDP)", kSrcArib + " (área de exibição)");
    addRule("lay.full_line_apr", S::Warning,
            "Linha cheia até a borda da área seguida de nova linha — decoders com quebra imediata exibem uma linha abaixo",
            "[CÓDIGO] libaribcaption#27; [CAPTURA-REAL] Globo usa o mesmo padrão (robustez, não é erro normativo)");
    // Accessibility (secondary source)
    addRule("acc.chars_per_line", S::Info, "Até 32 caracteres por linha", kSrcNbr15290 + " §4.1.4");
    addRule("acc.lines", S::Info, "Até 3 linhas por legenda (ao vivo)", kSrcNbr15290 + " §4.1.6.1");
}

void CaptionValidator::addRule(const QString& id, Severity severity, const QString& title, const QString& source) {
    index_[id] = rules_.size();
    rules_.append({id, severity, title, source, 0, 0, {}});
}

void CaptionValidator::reset() {
    for (auto& rule : rules_) {
        rule.checks = 0;
        rule.failures = 0;
        rule.recent.clear();
    }
    managementLate_ = false;
}

void CaptionValidator::record(const QString& id, bool ok, const QString& detail) {
    const auto it = index_.constFind(id);
    if (it == index_.constEnd())
        return;
    Rule& rule = rules_[it.value()];
    ++rule.checks;
    if (ok)
        return;
    ++rule.failures;
    rule.recent.prepend({QDateTime::currentDateTime(), detail});
    if (rule.recent.size() > kMaxRecent)
        rule.recent.removeLast();
}

int CaptionValidator::failingRules(Severity severity) const {
    int count = 0;
    for (const auto& rule : rules_)
        if (rule.severity == severity && rule.failures > 0)
            ++count;
    return count;
}

int CaptionValidator::passingRules() const {
    int count = 0;
    for (const auto& rule : rules_)
        if (rule.checks > 0 && rule.failures == 0)
            ++count;
    return count;
}

void CaptionValidator::checkPmt(const TsDemux& demux) {
    if (demux.pmts().isEmpty())
        return;

    QVector<PmtStream> captions;
    for (const auto& pmt : demux.pmts()) {
        for (const auto& stream : pmt.streams) {
            bool hasCaptionTag = false;
            bool hasCaptionComponent = false;
            for (const auto& d : stream.descriptors) {
                if (d.tag == 0x52 && !d.data.isEmpty() && isCaptionTag(static_cast<uint8_t>(d.data[0])))
                    hasCaptionTag = true;
                if (d.tag == 0xFD && d.data.size() >= 2) {
                    const int id = (static_cast<uint8_t>(d.data[0]) << 8) | static_cast<uint8_t>(d.data[1]);
                    if (id == 0x0008 || id == 0x0012)
                        hasCaptionComponent = true;
                }
            }
            if (stream.type == 0x06 && (hasCaptionTag || hasCaptionComponent))
                captions.append(stream);
        }
    }

    record("pmt.caption", !captions.isEmpty(), "Nenhum stream type 0x06 com tag 0x30–0x37 ou data_component_id 0x0008");
    for (const auto& stream : captions) {
        const QString where = "PID " + hex(stream.pid, 4);
        bool has52 = false;
        bool hasFd = false;
        int tag = -1;
        int componentId = -1;
        for (const auto& d : stream.descriptors) {
            if (d.tag == 0x52 && !d.data.isEmpty()) {
                has52 = true;
                tag = static_cast<uint8_t>(d.data[0]);
            }
            if (d.tag == 0xFD && d.data.size() >= 2) {
                hasFd = true;
                componentId = (static_cast<uint8_t>(d.data[0]) << 8) | static_cast<uint8_t>(d.data[1]);
            }
        }
        QStringList missing;
        if (!has52)
            missing << "0x52";
        if (!hasFd)
            missing << "0xFD";
        record("pmt.descriptors", missing.isEmpty(), where + ": falta descritor " + missing.join(", "));
        if (has52)
            record("pmt.component_tag", isCaptionTag(tag), where + ": component_tag " + hex(tag));
        if (hasFd) {
            const bool oneSeg = tag == 0x87;
            record("pmt.data_component_id", componentId == (oneSeg ? 0x0012 : 0x0008),
                   where + ": data_component_id " + hex(componentId, 4));
        }
    }
}

void CaptionValidator::checkDataGroup(const AribDataGroup& g) {
    const QString kind = g.management ? "gerenciamento" : QString("statement idioma %1").arg(g.languageId);

    record("pes.header", (g.dataIdentifier == 0x80 || g.dataIdentifier == 0x81) && g.privateStreamId == 0xFF,
           QString("data_identifier %1, private_stream_id %2").arg(hex(g.dataIdentifier), hex(g.privateStreamId)));
    record("pes.size", g.pesPayloadSize <= kMaxPesPayload, QString("%1 bytes").arg(g.pesPayloadSize));

    // crcPresent means header + data_group_size bytes + CRC_16 all fit inside the PES.
    record("dg.size", g.crcPresent,
           g.error.isEmpty() ? QString("data_group_size %1 sem espaço para o CRC").arg(g.size) : g.error);
    if (!g.crcPresent)
        return;

    const int id = g.dataGroupId;
    const bool idValid = id == 0x00 || id == 0x20 || (id >= 0x01 && id <= 0x08) || (id >= 0x21 && id <= 0x28);
    record("dg.id", idValid, "data_group_id " + hex(id));

    QString crcDetail = QString("%1: enviado %2, esperado %3").arg(kind, hex(g.crcSent, 4), hex(g.crcExpected, 4));
    if (g.crcSent == g.crcWithInitFfff)
        crcDetail += " — o encoder calcula o CRC com valor inicial 0xFFFF (CRC-16/CCITT-FALSE)";
    record("dg.crc", g.crcValid, crcDetail);
    record("dg.units", g.valid, g.error);
    record("dg.tmd", g.tmd != 0x03, QString("TMD %1").arg(g.tmd));

    if (g.management) {
        managementLate_ = false;
        record("mgmt.interval", true);
        record("mgmt.languages", g.languages.size() >= 1 && g.languages.size() <= 2,
               QString("num_languages %1").arg(g.languages.size()));
        for (const auto& lang : g.languages) {
            const QString who = QString("idioma tag %1").arg(lang.languageTag);
            record("mgmt.iso639", lang.iso639 == "por", who + ": \"" + lang.iso639 + "\"");
            record("mgmt.tcs", lang.tcs == 0, who + ": TCS " + describeTcs(lang.tcs));
            record("mgmt.rollup", lang.rollupMode <= 1, who + QString(": rollup_mode %1").arg(lang.rollupMode));
            record("mgmt.format", lang.format >= 0x8 && lang.format <= 0xB,
                   who + QString(": Format %1").arg(lang.format, 4, 2, QChar('0')));
        }
        return;
    }

    record("txt.charset", g.charsetIssues.isEmpty(), g.charsetIssues.mid(0, 5).join("; "));

    static const QSet<QString> forbidden = {"BEL", "CAN", "RS", "CDC", "WMM", "MACRO", "PLD", "PLU",
                                            "CCC", "TCC", "CFS", "MDF", "XCS"};
    QStringList used;
    bool usesScr = false;
    for (const auto& cmd : g.commands) {
        if (forbidden.contains(cmd.code) && !used.contains(cmd.code))
            used << cmd.code;
        if (cmd.code == "SCR")
            usesScr = true;
    }
    record("txt.forbidden", used.isEmpty(), "usa " + used.join(", "));
    record("txt.scr", !usesScr, QString("SCR modo %1, velocidade %2").arg(g.scrMode).arg(g.scrSpeed));

    if (g.ssmWidth >= 0) {
        static const QSet<int> allowed = {16, 20, 24, 30, 36};
        record("txt.ssm", allowed.contains(g.ssmWidth) && allowed.contains(g.ssmHeight),
               QString("SSM %1;%2").arg(g.ssmWidth).arg(g.ssmHeight));
    }
    if (g.swf >= 0)
        record("txt.swf", g.swf >= 7 && g.swf <= 10, QString("SWF %1 (%2)").arg(g.swf).arg(describeSwf(g.swf)));
}

void CaptionValidator::checkLayout(const QSize& plane, const QRect& displayArea, const QVector<QRect>& regions,
                                   const QStringList& texts) {
    const QRect planeRect(QPoint(0, 0), plane);
    auto describe = [](const QRect& r) {
        return QString("x=%1 y=%2 %3×%4").arg(r.x()).arg(r.y()).arg(r.width()).arg(r.height());
    };
    auto inside = [](const QRect& outer, const QRect& inner) {
        return inner.x() >= outer.x() && inner.y() >= outer.y() && inner.x() + inner.width() <= outer.x() + outer.width() &&
               inner.y() + inner.height() <= outer.y() + outer.height();
    };

    if (!displayArea.isNull())
        record("lay.area_in_plane", inside(planeRect, displayArea),
               QString("área %1 fora do plano %2×%3").arg(describe(displayArea)).arg(plane.width()).arg(plane.height()));

    QSet<int> lines;
    for (int i = 0; i < regions.size(); ++i) {
        const QRect& r = regions[i];
        const QString text = texts.value(i);
        const QString where = QString("\"%1\" em %2").arg(text, describe(r));
        lines.insert(r.y());

        record("lay.text_in_plane", inside(planeRect, r), where);
        if (!displayArea.isNull()) {
            record("lay.text_in_area", inside(displayArea, r), where + " — área " + describe(displayArea));

            const bool fullLine = r.x() + r.width() == displayArea.x() + displayArea.width();
            const bool followedByLine = i + 1 < regions.size() && regions[i + 1].y() > r.y();
            record("lay.full_line_apr", !(fullLine && followedByLine), where + " termina exatamente na borda da área");
        }
        record("acc.chars_per_line", text.size() <= 32, QString("%1 caracteres: %2").arg(text.size()).arg(where));
    }
    if (!regions.isEmpty())
        record("acc.lines", lines.size() <= 3, QString("%1 linhas").arg(lines.size()));
}

void CaptionValidator::checkManagementInterval(qint64 msSinceManagement, qint64 msSinceFirstStatement) {
    if (managementLate_)
        return;
    if (msSinceManagement < 0) {
        if (msSinceFirstStatement > kManagementIntervalMs) {
            managementLate_ = true;
            record("mgmt.interval", false, "legendas há mais de 3 min sem nenhum dado de gerenciamento");
        }
        return;
    }
    if (msSinceManagement > kManagementIntervalMs) {
        managementLate_ = true;
        record("mgmt.interval", false, QString("%1 s sem gerenciamento").arg(msSinceManagement / 1000));
    }
}
