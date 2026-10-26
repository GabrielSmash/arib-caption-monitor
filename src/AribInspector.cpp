#include "AribInspector.h"

#include <QStringList>

namespace {

int u8(const QByteArray& b, int i) { return static_cast<uint8_t>(b[i]); }

const char* kColorNames[] = {"preto", "vermelho", "verde", "amarelo", "azul", "magenta", "ciano", "branco"};

struct CsiResult {
    int length = 0;  // bytes consumed after the CSI byte
    int finalByte = 0;
    QVector<int> params;
    QString raw;
};

// CSI P1 ; P2 ... I(0x20) F
CsiResult parseCsi(const QByteArray& b, int pos) {
    CsiResult r;
    int value = -1;
    int i = pos;
    while (i < b.size()) {
        const int c = u8(b, i);
        if (c >= 0x30 && c <= 0x39) {
            value = (value < 0 ? 0 : value * 10) + (c - 0x30);
        } else if (c == 0x3B) {
            r.params.append(value < 0 ? 0 : value);
            value = -1;
        } else if (c == 0x20) {
            if (value >= 0)
                r.params.append(value);
            if (i + 1 >= b.size())
                return {};
            r.finalByte = u8(b, i + 1);
            r.length = i + 2 - pos;
            QStringList parts;
            for (int p : r.params)
                parts << QString::number(p);
            r.raw = parts.join(';');
            return r;
        }
        ++i;
    }
    return {};
}

QString csiName(int f) {
    switch (f) {
        case 0x42: return "GSM";
        case 0x53: return "SWF";
        case 0x54: return "CCC";
        case 0x56: return "SDF";
        case 0x57: return "SSM";
        case 0x58: return "SHS";
        case 0x59: return "SVS";
        case 0x5B: return "PLD";
        case 0x5C: return "PLU";
        case 0x5D: return "GAA";
        case 0x5E: return "SRC";
        case 0x5F: return "SDP";
        case 0x61: return "ACPS";
        case 0x62: return "TCC";
        case 0x63: return "ORN";
        case 0x64: return "MDF";
        case 0x65: return "CFS";
        case 0x66: return "XCS";
        case 0x67: return "SCR";
        case 0x68: return "PRA";
        case 0x69: return "ACS";
        case 0x6A: return "UED";
        case 0x6E: return "RCS";
        case 0x6F: return "SCS";
        default: return QString("CSI 0x%1").arg(f, 2, 16, QChar('0'));
    }
}

quint16 crc16Arib(const uint8_t* data, int length, quint16 initial = 0) {
    quint16 crc = initial;
    for (int i = 0; i < length; ++i) {
        crc ^= static_cast<quint16>(data[i]) << 8;
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc & 0x8000) ? static_cast<quint16>((crc << 1) ^ 0x1021) : static_cast<quint16>(crc << 1);
    }
    return crc;
}

// Graphic sets of the ABNT NBR 15606-1 Latin profile (final bytes from libaribcaption b24_codesets.cpp).
enum class CharSet { Alphanumeric, LatinExtension, LatinSpecial, NonLatin };

CharSet charSetForFinalByte(int f) {
    switch (f) {
        case 0x4A: return CharSet::Alphanumeric;
        case 0x4B: return CharSet::LatinExtension;
        case 0x4C: return CharSet::LatinSpecial;
        default: return CharSet::NonLatin;
    }
}

bool latinSpecialDefined(int position) {
    return position == 0x21 || (position >= 0x30 && position <= 0x37) || (position >= 0x40 && position <= 0x4B);
}

void parseStatementBody(const QByteArray& b, AribDataGroup& g) {
    // Initial code set state of the Latin profile: G0/G1 alphanumeric, G2 Latin Extension,
    // G3 Latin Special, GL = G0, GR = G2.
    CharSet sets[4] = {CharSet::Alphanumeric, CharSet::Alphanumeric, CharSet::LatinExtension, CharSet::LatinSpecial};
    int gl = 0;
    int gr = 2;
    int singleShift = -1;

    auto checkCharacter = [&](int byte, int offset) {
        const bool isGr = byte >= 0xA0;
        const int setIndex = singleShift >= 0 ? singleShift : (isGr ? gr : gl);
        singleShift = -1;
        const int position = byte & 0x7F;
        const CharSet set = sets[setIndex];
        if (set == CharSet::NonLatin) {
            g.charsetIssues << QString("byte 0x%1 (offset %2) em conjunto não latino (G%3)")
                                   .arg(byte, 2, 16, QChar('0'))
                                   .arg(offset)
                                   .arg(setIndex);
        } else if (set == CharSet::LatinSpecial && !latinSpecialDefined(position)) {
            g.charsetIssues << QString("byte 0x%1 (offset %2): posição 0x%3 indefinida no Latin Special")
                                   .arg(byte, 2, 16, QChar('0'))
                                   .arg(offset)
                                   .arg(position, 2, 16, QChar('0'));
        }
    };

    auto handleEscape = [&](const QByteArray& seq) {
        if (seq.isEmpty())
            return;
        const int first = static_cast<uint8_t>(seq[0]);
        const int last = static_cast<uint8_t>(seq[seq.size() - 1]);
        switch (first) {
            case 0x6E: gl = 2; return;  // LS2
            case 0x6F: gl = 3; return;  // LS3
            case 0x7E: gr = 1; return;  // LS1R
            case 0x7D: gr = 2; return;  // LS2R
            case 0x7C: gr = 3; return;  // LS3R
            default: break;
        }
        int target = -1;
        CharSet set = CharSet::NonLatin;
        if (first >= 0x28 && first <= 0x2B) {  // 1-byte set (or DRCS when followed by 0x20)
            target = first - 0x28;
            if (!(seq.size() >= 3 && static_cast<uint8_t>(seq[1]) == 0x20))
                set = charSetForFinalByte(last);
        } else if (first == 0x24) {  // 2-byte set: never Latin
            target = 0;
            if (seq.size() >= 3 && static_cast<uint8_t>(seq[1]) >= 0x29 && static_cast<uint8_t>(seq[1]) <= 0x2B)
                target = static_cast<uint8_t>(seq[1]) - 0x28;
        }
        if (target < 0)
            return;
        sets[target] = set;
        if (set == CharSet::NonLatin)
            g.charsetIssues << QString("designação de conjunto não latino em G%1 (ESC %2)")
                                   .arg(target)
                                   .arg(QString(seq.toHex(' ').toUpper()));
    };

    int textRun = 0;
    auto flushText = [&]() {
        if (textRun > 0) {
            g.commands.append({"Texto", QString::number(textRun), QString("%1 byte(s) de caracteres").arg(textRun)});
            textRun = 0;
        }
    };
    auto add = [&](const QString& code, const QString& params, const QString& description) {
        flushText();
        g.commands.append({code, params, description});
    };
    auto param = [&](int i) { return i < b.size() ? u8(b, i) : 0; };

    int i = 0;
    while (i < b.size()) {
        const int c = u8(b, i);

        if ((c >= 0x21 && c <= 0x7E) || (c >= 0xA1 && c <= 0xFE) || c == 0x20) {
            if (c != 0x20)
                checkCharacter(c, i);
            ++textRun;
            ++i;
            continue;
        }

        switch (c) {
            case 0x00: add("NUL", {}, "Nulo"); i += 1; break;
            case 0x07: add("BEL", {}, "Alerta sonoro"); i += 1; break;
            case 0x08: add("APB", {}, "Cursor para trás"); i += 1; break;
            case 0x09: add("APF", {}, "Cursor para frente"); i += 1; break;
            case 0x0A: add("APD", {}, "Cursor desce uma linha"); i += 1; break;
            case 0x0B: add("APU", {}, "Cursor sobe uma linha"); i += 1; break;
            case 0x0C:
                g.clearScreen = true;
                add("CS", {}, "Limpar tela");
                i += 1;
                break;
            case 0x0D: add("APR", {}, "Quebra de linha (retorno + desce)"); i += 1; break;
            case 0x0E: gl = 1; add("LS1", {}, "Locking shift G1→GL"); i += 1; break;
            case 0x0F: gl = 0; add("LS0", {}, "Locking shift G0→GL"); i += 1; break;
            case 0x16:
                add("PAPF", QString::number(param(i + 1) & 0x3F), QString("Avança cursor %1 posição(ões)").arg(param(i + 1) & 0x3F));
                i += 2;
                break;
            case 0x18: add("CAN", {}, "Cancelar"); i += 1; break;
            case 0x19: add("SS2", {}, "Single shift G2 (caractere estendido)"); singleShift = 2; i += 1; break;
            case 0x1D: add("SS3", {}, "Single shift G3 (caractere especial)"); singleShift = 3; i += 1; break;
            case 0x1B: {
                int j = i + 1;
                while (j < b.size() && u8(b, j) >= 0x20 && u8(b, j) <= 0x2F)
                    ++j;
                const QByteArray seq = b.mid(i + 1, j - i);
                handleEscape(seq);
                add("ESC", seq.toHex(' ').toUpper(), "Designação/invocação de conjunto de caracteres");
                i = j + 1;
                break;
            }
            case 0x1C: {
                const int y = param(i + 1) & 0x3F;
                const int x = param(i + 2) & 0x3F;
                add("APS", QString("%1;%2").arg(y).arg(x), QString("Posiciona cursor linha %1, coluna %2").arg(y).arg(x));
                i += 3;
                break;
            }
            case 0x1E: add("RS", {}, "Record separator"); i += 1; break;
            case 0x1F: add("US", {}, "Unit separator"); i += 1; break;
            case 0x7F: add("DEL", {}, "Apagar"); i += 1; break;
            case 0x80: case 0x81: case 0x82: case 0x83: case 0x84: case 0x85: case 0x86: case 0x87: {
                static const char* codes[] = {"BKF", "RDF", "GRF", "YLF", "BLF", "MGF", "CNF", "WHF"};
                add(codes[c - 0x80], {}, QString("Cor do texto: %1").arg(kColorNames[c - 0x80]));
                i += 1;
                break;
            }
            case 0x88: add("SSZ", {}, "Tamanho pequeno"); i += 1; break;
            case 0x89: add("MSZ", {}, "Tamanho médio"); i += 1; break;
            case 0x8A: add("NSZ", {}, "Tamanho normal"); i += 1; break;
            case 0x8B: add("SZX", QString::number(param(i + 1), 16), "Tamanho especial"); i += 2; break;
            case 0x90: {
                const int p1 = param(i + 1);
                if (p1 == 0x20) {
                    add("COL", QString("palette %1").arg(param(i + 2) & 0x0F), "Seleciona paleta");
                    i += 3;
                } else {
                    const int index = p1 & 0x0F;
                    const QString target = (p1 & 0xF0) == 0x50 ? "fundo" : (p1 & 0xF0) == 0x40 ? "texto" : "meio-tom";
                    const QString color = index < 8 ? kColorNames[index] : QString("índice %1").arg(index);
                    add("COL", QString("0x%1").arg(p1, 2, 16, QChar('0')), QString("Cor do %1: %2").arg(target, color));
                    i += 2;
                }
                break;
            }
            case 0x91: add("FLC", QString::number(param(i + 1), 16), "Controle de piscar"); i += 2; break;
            case 0x92:
                add("CDC", {}, "Controle de ocultação");
                i += param(i + 1) == 0x20 ? 3 : 2;
                break;
            case 0x93: add("POL", QString::number(param(i + 1), 16), "Polaridade"); i += 2; break;
            case 0x94: add("WMM", QString::number(param(i + 1), 16), "Modo de escrita"); i += 2; break;
            case 0x95: add("MACRO", {}, "Macro (não suportada)"); i = b.size(); break;
            case 0x97: add("HLC", QString::number(param(i + 1) & 0x0F), "Contorno (enclosure)"); i += 2; break;
            case 0x98: add("RPC", QString::number(param(i + 1) & 0x3F), "Repetir caractere"); i += 2; break;
            case 0x99: add("SPL", {}, "Fim de sublinhado"); i += 1; break;
            case 0x9A: add("STL", {}, "Início de sublinhado"); i += 1; break;
            case 0x9D: {
                if (param(i + 1) == 0x20) {
                    const int tenths = param(i + 2) & 0x3F;
                    add("TIME", QString::number(tenths), QString("Espera %1 ms").arg(tenths * 100));
                } else {
                    add("TIME", QString("0x%1").arg(param(i + 1), 2, 16, QChar('0')), "Controle de tempo");
                }
                i += 3;
                break;
            }
            case 0x9B: {
                const CsiResult csi = parseCsi(b, i + 1);
                if (csi.length == 0) {
                    add("CSI", {}, "Sequência CSI truncada");
                    i = b.size();
                    break;
                }
                const QString name = csiName(csi.finalByte);
                const int p1 = csi.params.value(0, -1);
                const int p2 = csi.params.value(1, -1);
                QString description;
                switch (csi.finalByte) {
                    case 0x53:
                        g.swf = p1;
                        description = "Formato de escrita: " + describeSwf(p1);
                        break;
                    case 0x56:
                        g.sdfWidth = p1;
                        g.sdfHeight = p2;
                        description = QString("Tamanho da área de exibição %1×%2").arg(p1).arg(p2);
                        break;
                    case 0x5F:
                        g.sdpX = p1;
                        g.sdpY = p2;
                        description = QString("Posição da área de exibição x=%1 y=%2").arg(p1).arg(p2);
                        break;
                    case 0x57:
                        g.ssmWidth = p1;
                        g.ssmHeight = p2;
                        description = QString("Tamanho do caractere %1×%2 dots").arg(p1).arg(p2);
                        break;
                    case 0x58:
                        g.shs = p1;
                        description = QString("Espaçamento horizontal %1 dots").arg(p1);
                        break;
                    case 0x59:
                        g.svs = p1;
                        description = QString("Espaçamento vertical %1 dots").arg(p1);
                        break;
                    case 0x61:
                        description = QString("Posiciona cursor em x=%1 y=%2 (dots)").arg(p1).arg(p2);
                        break;
                    case 0x67: {
                        g.scrMode = p1;
                        g.scrSpeed = p2;
                        static const char* modes[] = {"fixo (sem scroll)", "scroll sem roll-out", "scroll com roll-out"};
                        description = QString("Scroll: %1, velocidade %2")
                                          .arg(p1 >= 0 && p1 <= 2 ? modes[p1] : "desconhecido")
                                          .arg(p2);
                        break;
                    }
                    case 0x5B: description = "Desce meia linha"; break;
                    case 0x5C: description = "Sobe meia linha"; break;
                    case 0x68: description = QString("Som interno %1").arg(p1); break;
                    case 0x63: description = "Ornamento (borda)"; break;
                    case 0x64: description = "Estilo de fonte"; break;
                    default: description = "Comando CSI"; break;
                }
                add(name, csi.raw, description);
                i += 1 + csi.length;
                break;
            }
            default:
                add(QString("0x%1").arg(c, 2, 16, QChar('0')).toUpper(), {}, "Código de controle não reconhecido");
                i += 1;
                break;
        }
    }
    flushText();
}

bool parseDataUnits(const QByteArray& d, int pos, AribDataGroup& g) {
    if (pos + 3 > d.size()) {
        g.error = "data_unit_loop_length ausente";
        return false;
    }
    const int loopLength = (u8(d, pos) << 16) | (u8(d, pos + 1) << 8) | u8(d, pos + 2);
    pos += 3;
    const int end = qMin(pos + loopLength, static_cast<int>(d.size()));
    while (pos + 5 <= end) {
        if (u8(d, pos) != 0x1F) {
            g.error = "unit_separator inválido";
            return false;
        }
        AribDataUnit unit;
        unit.parameter = u8(d, pos + 1);
        unit.size = (u8(d, pos + 2) << 16) | (u8(d, pos + 3) << 8) | u8(d, pos + 4);
        g.units.append(unit);
        if (unit.parameter == 0x20)
            parseStatementBody(d.mid(pos + 5, unit.size), g);
        pos += 5 + unit.size;
    }
    return true;
}

}  // namespace

AribDataGroup inspectAribPes(const QByteArray& p) {
    AribDataGroup g;
    g.pesPayloadSize = p.size();
    if (p.size() < 3) {
        g.error = "PES muito curto";
        return g;
    }
    g.dataIdentifier = u8(p, 0);
    g.privateStreamId = u8(p, 1);
    const int headerLength = u8(p, 2) & 0x0F;
    const int begin = 3 + headerLength;
    if (begin + 5 > p.size()) {
        g.error = "data_group incompleto";
        return g;
    }

    g.dataGroupId = (u8(p, begin) >> 2) & 0x3F;
    g.groupSet = (g.dataGroupId & 0x20) ? 'B' : 'A';
    g.languageId = g.dataGroupId & 0x0F;
    g.management = g.languageId == 0;
    g.size = (u8(p, begin + 3) << 8) | u8(p, begin + 4);
    const int groupLength = 5 + g.size + 2;  // header + data + CRC_16
    if (begin + groupLength <= p.size()) {
        const auto* group = reinterpret_cast<const uint8_t*>(p.constData()) + begin;
        g.crcPresent = true;
        g.crcSent = static_cast<quint16>((group[groupLength - 2] << 8) | group[groupLength - 1]);
        g.crcExpected = crc16Arib(group, groupLength - 2);
        g.crcWithInitFfff = crc16Arib(group, groupLength - 2, 0xFFFF);
        g.crcValid = g.crcSent == g.crcExpected;
    }
    const QByteArray d = p.mid(begin + 5, g.size);
    if (d.size() < g.size || d.isEmpty()) {
        g.error = "data_group_size maior que o PES";
        return g;
    }

    g.tmd = (u8(d, 0) >> 6) & 0x03;
    int pos = 1;

    if (g.management) {
        if (g.tmd == 0x02)
            pos += 5;  // OTM
        if (pos >= d.size()) {
            g.error = "num_languages ausente";
            return g;
        }
        const int count = u8(d, pos++);
        for (int n = 0; n < count && pos + 5 <= d.size(); ++n) {
            AribLanguageInfo lang;
            lang.languageTag = (u8(d, pos) >> 5) & 0x07;
            lang.dmf = u8(d, pos) & 0x0F;
            pos += 1;
            if (lang.dmf == 0x0C || lang.dmf == 0x0D || lang.dmf == 0x0E)
                pos += 1;  // DC
            lang.iso639 = QString::fromLatin1(d.mid(pos, 3));
            pos += 3;
            lang.format = (u8(d, pos) >> 4) & 0x0F;
            lang.tcs = (u8(d, pos) >> 2) & 0x03;
            lang.rollupMode = u8(d, pos) & 0x03;
            pos += 1;
            g.languages.append(lang);
        }
    } else if (g.tmd == 0x01 || g.tmd == 0x02) {
        pos += 5;  // STM
    }

    g.valid = parseDataUnits(d, pos, g);
    return g;
}

QString describeDmf(int dmf) {
    static const char* modes[] = {"exibição automática", "não exibe automaticamente", "exibição selecionável",
                                  "automática sob condição específica"};
    return QString("recepção: %1; gravação: %2").arg(modes[(dmf >> 2) & 0x03], modes[dmf & 0x03]);
}

QString describeRollupMode(int mode) {
    switch (mode) {
        case 0: return "não roll-up";
        case 1: return "roll-up";
        default: return "reservado";
    }
}

QString describeTcs(int tcs) {
    switch (tcs) {
        case 0: return "código de 8 bits";
        case 1: return "UCS (UTF-8)";
        default: return "reservado";
    }
}

QString describeSwf(int swf) {
    switch (swf) {
        case 5: return "1920×1080 horizontal";
        case 6: return "1920×1080 vertical";
        case 7: return "960×540 horizontal";
        case 8: return "960×540 vertical";
        case 9: return "720×480 horizontal";
        case 10: return "720×480 vertical";
        default: return QString("formato %1").arg(swf);
    }
}

QString describeDataUnit(int parameter) {
    switch (parameter) {
        case 0x20: return "Statement body (texto/controles)";
        case 0x28: return "Geometric";
        case 0x2C: return "Synthesized sound";
        case 0x30: return "DRCS 1-byte";
        case 0x31: return "DRCS 2-byte";
        case 0x34: return "Colormap";
        case 0x35: return "Bitmap";
        default: return QString("0x%1").arg(parameter, 2, 16, QChar('0'));
    }
}
