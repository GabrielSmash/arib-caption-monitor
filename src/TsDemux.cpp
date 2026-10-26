#include "TsDemux.h"

namespace {

constexpr int kPacketSize = 188;
constexpr int kNullPid = 0x1FFF;
constexpr int kPatPid = 0x0000;
constexpr int kSdtPid = 0x0011;
constexpr qint64 kNoPts = INT64_MIN;

uint32_t crc32Mpeg(const uint8_t* data, int length) {
    uint32_t crc = 0xFFFFFFFF;
    for (int i = 0; i < length; ++i) {
        crc ^= static_cast<uint32_t>(data[i]) << 24;
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc & 0x80000000) ? (crc << 1) ^ 0x04C11DB7 : (crc << 1);
    }
    return crc;
}

int u8(const QByteArray& b, int i) { return static_cast<uint8_t>(b[i]); }
int u16(const QByteArray& b, int i) { return (u8(b, i) << 8) | u8(b, i + 1); }

QVector<Descriptor> parseDescriptors(const QByteArray& s, int offset, int length) {
    QVector<Descriptor> list;
    const int end = qMin(offset + length, static_cast<int>(s.size()));
    while (offset + 2 <= end) {
        const int tag = u8(s, offset);
        const int len = u8(s, offset + 1);
        if (offset + 2 + len > end)
            break;
        list.append({static_cast<uint8_t>(tag), s.mid(offset + 2, len)});
        offset += 2 + len;
    }
    return list;
}

bool isCaptionStream(const PmtStream& stream) {
    if (stream.type != 0x06)
        return false;
    for (const auto& d : stream.descriptors) {
        if (d.tag == 0x52 && d.data.size() >= 1) {
            const int tag = static_cast<uint8_t>(d.data[0]);
            if (tag >= 0x30 && tag <= 0x37)
                return true;
        }
        if (d.tag == 0xFD && d.data.size() >= 2 && u16(d.data, 0) == 0x0008) {
            // data_component_id 0x0008 is shared by captions and superimpose; the component tag tells them apart.
            bool superimpose = false;
            for (const auto& other : stream.descriptors)
                if (other.tag == 0x52 && other.data.size() >= 1 && static_cast<uint8_t>(other.data[0]) >= 0x38)
                    superimpose = true;
            if (!superimpose)
                return true;
        }
    }
    return false;
}

}  // namespace

TsDemux::TsDemux(QObject* parent) : QObject(parent) {}

void TsDemux::reset() {
    pending_.clear();
    pat_ = {};
    pmts_.clear();
    sdt_ = {};
    pidStats_.clear();
    sections_.clear();
    pes_.clear();
    lastSections_.clear();
    totalPackets_ = 0;
    syncErrors_ = 0;
    if (captionPid_ != -1) {
        captionPid_ = -1;
        emit captionPidChanged(captionPid_);
    }
    emit tablesChanged();
}

void TsDemux::feed(const QByteArray& data) {
    pending_.append(data);
    const auto* bytes = reinterpret_cast<const uint8_t*>(pending_.constData());
    const int size = pending_.size();
    int pos = 0;

    while (pos + kPacketSize <= size) {
        if (bytes[pos] != 0x47) {
            ++syncErrors_;
            int next = pos + 1;
            while (next < size && bytes[next] != 0x47)
                ++next;
            pos = next;
            continue;
        }
        handlePacket(bytes + pos);
        pos += kPacketSize;
    }
    pending_.remove(0, pos);
}

void TsDemux::handlePacket(const uint8_t* p) {
    ++totalPackets_;
    const bool transportError = p[1] & 0x80;
    const bool unitStart = p[1] & 0x40;
    const int pid = ((p[1] & 0x1F) << 8) | p[2];
    const int scrambling = (p[3] >> 6) & 0x03;
    const int adaptation = (p[3] >> 4) & 0x03;
    const int cc = p[3] & 0x0F;

    PidStats& stats = pidStats_[pid];
    ++stats.packets;
    stats.scrambled = scrambling != 0;
    if (transportError) {
        ++stats.transportErrors;
        return;
    }
    if (pid == kNullPid)
        return;

    int offset = 4;
    bool discontinuity = false;
    if (adaptation & 0x02) {
        const int length = p[4];
        if (length > 0)
            discontinuity = p[5] & 0x80;
        offset += 1 + length;
    }
    const bool hasPayload = adaptation & 0x01;

    if (hasPayload) {
        bool ccError = false;
        if (stats.lastCc >= 0 && !discontinuity) {
            const int expected = (stats.lastCc + 1) & 0x0F;
            if (cc == stats.lastCc)
                return;  // duplicate packet, allowed once by ISO 13818-1
            ccError = cc != expected;
        }
        stats.lastCc = cc;
        if (ccError) {
            ++stats.ccErrors;
            if (pes_.contains(pid))
                pes_[pid] = {};
            if (sections_.contains(pid))
                sections_[pid] = {};
        }
    }

    if (!hasPayload || offset >= kPacketSize)
        return;

    const uint8_t* payload = p + offset;
    const int length = kPacketSize - offset;

    if (pid == kPatPid || pid == kSdtPid || pmts_.contains(pid) || pat_.programs.values().contains(pid))
        handleSectionPayload(pid, payload, length, unitStart);
    else if (pid == captionPid_)
        handlePesPayload(pid, payload, length, unitStart);
}

void TsDemux::handleSectionPayload(int pid, const uint8_t* payload, int length, bool unitStart) {
    SectionBuffer& buffer = sections_[pid];
    int pos = 0;

    if (unitStart) {
        const int pointer = payload[0];
        pos = 1;
        if (buffer.active && pos + pointer <= length) {
            buffer.data.append(reinterpret_cast<const char*>(payload + pos), pointer);
            flushSections(pid, buffer);
        }
        buffer.data.clear();
        buffer.active = true;
        pos += pointer;
    } else if (!buffer.active) {
        return;
    }

    if (pos < length)
        buffer.data.append(reinterpret_cast<const char*>(payload + pos), length - pos);
    flushSections(pid, buffer);
}

void TsDemux::flushSections(int pid, SectionBuffer& buffer) {
    while (buffer.active && buffer.data.size() >= 3) {
        if (u8(buffer.data, 0) == 0xFF) {  // stuffing until the next unit start
            buffer.data.clear();
            buffer.active = false;
            return;
        }
        const int sectionLength = ((u8(buffer.data, 1) & 0x0F) << 8 | u8(buffer.data, 2)) + 3;
        if (buffer.data.size() < sectionLength)
            return;
        handleSection(pid, buffer.data.left(sectionLength));
        buffer.data.remove(0, sectionLength);
    }
}

void TsDemux::handleSection(int pid, const QByteArray& section) {
    if (section.size() < 12)
        return;
    const bool syntax = u8(section, 1) & 0x80;
    if (!syntax)
        return;
    if (crc32Mpeg(reinterpret_cast<const uint8_t*>(section.constData()), section.size()) != 0)
        return;

    const int tableId = u8(section, 0);
    const int extension = u16(section, 3);
    const quint64 key = (static_cast<quint64>(pid) << 32) | (static_cast<quint64>(tableId) << 16) | extension;
    if (lastSections_.value(key) == section)
        return;
    lastSections_[key] = section;

    if (pid == kPatPid && tableId == 0x00)
        parsePat(section);
    else if (pid == kSdtPid && tableId == 0x42)
        parseSdt(section);
    else if (tableId == 0x02)
        parsePmt(pid, section);
    else
        return;

    emit tablesChanged();
}

void TsDemux::parsePat(const QByteArray& s) {
    PatInfo pat;
    pat.valid = true;
    pat.tsId = u16(s, 3);
    pat.version = (u8(s, 5) >> 1) & 0x1F;
    for (int i = 8; i + 4 <= s.size() - 4; i += 4) {
        const int program = u16(s, i);
        const int pid = u16(s, i + 2) & 0x1FFF;
        if (program == 0)
            pat.nitPid = pid;
        else
            pat.programs[program] = pid;
    }

    // Drop PMTs that are no longer announced.
    for (auto it = pmts_.begin(); it != pmts_.end();) {
        if (!pat.programs.values().contains(it.key()))
            it = pmts_.erase(it);
        else
            ++it;
    }
    pat_ = pat;
    selectCaptionPid();
}

void TsDemux::parsePmt(int pid, const QByteArray& s) {
    PmtInfo pmt;
    pmt.pid = pid;
    pmt.programNumber = u16(s, 3);
    pmt.version = (u8(s, 5) >> 1) & 0x1F;
    pmt.pcrPid = u16(s, 8) & 0x1FFF;
    const int programInfoLength = u16(s, 10) & 0x0FFF;
    pmt.descriptors = parseDescriptors(s, 12, programInfoLength);

    int pos = 12 + programInfoLength;
    const int end = s.size() - 4;
    while (pos + 5 <= end) {
        PmtStream stream;
        stream.type = u8(s, pos);
        stream.pid = u16(s, pos + 1) & 0x1FFF;
        const int esInfoLength = u16(s, pos + 3) & 0x0FFF;
        stream.descriptors = parseDescriptors(s, pos + 5, esInfoLength);
        pmt.streams.append(stream);
        pos += 5 + esInfoLength;
    }

    pmts_[pid] = pmt;
    selectCaptionPid();
}

void TsDemux::parseSdt(const QByteArray& s) {
    SdtInfo sdt;
    sdt.valid = true;
    sdt.tsId = u16(s, 3);
    sdt.version = (u8(s, 5) >> 1) & 0x1F;
    sdt.onId = u16(s, 8);

    int pos = 11;
    const int end = s.size() - 4;
    while (pos + 5 <= end) {
        SdtService service;
        service.serviceId = u16(s, pos);
        const int loopLength = u16(s, pos + 3) & 0x0FFF;
        for (const auto& d : parseDescriptors(s, pos + 5, loopLength)) {
            if (d.tag != 0x48 || d.data.size() < 3)
                continue;
            service.type = static_cast<uint8_t>(d.data[0]);
            const int providerLength = static_cast<uint8_t>(d.data[1]);
            service.provider = decodeDvbString(d.data.mid(2, providerLength));
            const int nameOffset = 2 + providerLength;
            if (nameOffset < d.data.size()) {
                const int nameLength = static_cast<uint8_t>(d.data[nameOffset]);
                service.name = decodeDvbString(d.data.mid(nameOffset + 1, nameLength));
            }
        }
        sdt.services.append(service);
        pos += 5 + loopLength;
    }
    sdt_ = sdt;
}

void TsDemux::handlePesPayload(int pid, const uint8_t* payload, int length, bool unitStart) {
    PesBuffer& buffer = pes_[pid];
    if (unitStart) {
        if (buffer.active)
            finishPes(pid, buffer);
        buffer.data.clear();
        buffer.active = true;
    } else if (!buffer.active) {
        return;
    }

    buffer.data.append(reinterpret_cast<const char*>(payload), length);

    // Emit as soon as the PES is complete instead of waiting for the next unit start,
    // otherwise a live caption would only appear when the following one arrives.
    if (buffer.data.size() >= 6) {
        const int pesLength = u16(buffer.data, 4);
        if (pesLength > 0 && buffer.data.size() >= 6 + pesLength) {
            buffer.data.truncate(6 + pesLength);
            finishPes(pid, buffer);
        }
    }
}

void TsDemux::finishPes(int pid, PesBuffer& buffer) {
    const QByteArray pes = buffer.data;
    buffer.data.clear();
    buffer.active = false;

    if (pes.size() < 9 || u8(pes, 0) != 0x00 || u8(pes, 1) != 0x00 || u8(pes, 2) != 0x01)
        return;

    const int headerLength = u8(pes, 8);
    const int payloadStart = 9 + headerLength;
    if (payloadStart > pes.size())
        return;

    qint64 pts = kNoPts;
    if ((u8(pes, 7) & 0x80) && pes.size() >= 14) {
        pts = (static_cast<qint64>(u8(pes, 9) & 0x0E) << 29) | (static_cast<qint64>(u8(pes, 10)) << 22) |
              (static_cast<qint64>(u8(pes, 11) & 0xFE) << 14) | (static_cast<qint64>(u8(pes, 12)) << 7) |
              (static_cast<qint64>(u8(pes, 13)) >> 1);
    }

    emit captionPes(pid, pes.mid(payloadStart), pts);
}

void TsDemux::selectCaptionPid() {
    int selected = -1;
    for (const auto& pmt : pmts_) {
        for (const auto& stream : pmt.streams) {
            if (isCaptionStream(stream)) {
                selected = stream.pid;
                break;
            }
        }
        if (selected != -1)
            break;
    }

    if (selected != captionPid_) {
        captionPid_ = selected;
        pes_.clear();
        emit captionPidChanged(captionPid_);
    }
}

bool TsDemux::hasAudioVideo() const {
    static const QVector<int> types = {0x01, 0x02, 0x10, 0x1B, 0x24, 0x03, 0x04, 0x0F, 0x11, 0x81, 0x87};
    for (const auto& pmt : pmts_)
        for (const auto& stream : pmt.streams)
            if (types.contains(stream.type))
                return true;
    return false;
}

QString TsDemux::pidUsage(int pid) const {
    if (pid == kPatPid)
        return "PAT";
    if (pid == 0x0001)
        return "CAT";
    if (pid == 0x0010 || pid == pat_.nitPid)
        return "NIT";
    if (pid == kSdtPid)
        return "SDT/BAT";
    if (pid == 0x0012 || pid == 0x0026 || pid == 0x0027)
        return "EIT";
    if (pid == 0x0014)
        return "TDT/TOT";
    if (pid == kNullPid)
        return "Stuffing (nulo)";
    for (const auto& pmt : pmts_) {
        if (pmt.pid == pid)
            return QString("PMT (programa %1)").arg(pmt.programNumber);
        for (const auto& stream : pmt.streams) {
            if (stream.pid == pid) {
                QString usage = streamTypeName(stream.type);
                if (pid == captionPid_)
                    usage += " — Legenda ARIB";
                return usage;
            }
        }
        if (pmt.pcrPid == pid)
            return "PCR";
    }
    return "Desconhecido";
}

QString streamTypeName(int type) {
    switch (type) {
        case 0x01: return "MPEG-1 Video";
        case 0x02: return "MPEG-2 Video";
        case 0x03: return "MPEG-1 Audio";
        case 0x04: return "MPEG-2 Audio";
        case 0x06: return "PES private data";
        case 0x0B: return "DSM-CC (U-N)";
        case 0x0D: return "DSM-CC sections";
        case 0x0F: return "AAC (ADTS)";
        case 0x11: return "AAC (LATM)";
        case 0x1B: return "H.264/AVC Video";
        case 0x24: return "H.265/HEVC Video";
        default: return QString("Tipo 0x%1").arg(type, 2, 16, QChar('0')).toUpper();
    }
}

QString serviceTypeName(int type) {
    switch (type) {
        case 0x01: return "TV digital";
        case 0x02: return "Rádio digital";
        case 0xA1: return "TV temporária";
        case 0xC0: return "Dados";
        default: return QString("0x%1").arg(type, 2, 16, QChar('0'));
    }
}

QString decodeDvbString(const QByteArray& bytes) {
    QString out;
    for (char c : bytes) {
        const auto b = static_cast<uint8_t>(c);
        if (b >= 0x20 && b < 0x7F)
            out += QChar(b);
        else if (b >= 0xA0)
            out += QChar(b);  // Latin-1
    }
    return out;
}

QPair<QString, QString> describeDescriptor(const Descriptor& d) {
    const QByteArray& b = d.data;
    const QString hex = b.toHex(' ').toUpper();
    switch (d.tag) {
        case 0x09: return {"Conditional access", hex};
        case 0x0A:
            return {"ISO 639 language", b.size() >= 3 ? QString::fromLatin1(b.left(3)) : hex};
        case 0x48: return {"Service", hex};
        case 0x50: return {"Component", hex};
        case 0x52: {
            if (b.isEmpty())
                return {"Stream identifier", hex};
            const int tag = static_cast<uint8_t>(b[0]);
            QString kind;
            if (tag >= 0x30 && tag <= 0x37)
                kind = " (legenda)";
            else if (tag >= 0x38 && tag <= 0x3F)
                kind = " (superimpose)";
            return {"Stream identifier", QString("component_tag 0x%1%2").arg(tag, 2, 16, QChar('0')).arg(kind)};
        }
        case 0xC1: return {"Digital copy control", hex};
        case 0xC4: return {"Audio component", hex};
        case 0xCF: return {"Logo transmission", hex};
        case 0xF6: return {"Access control", hex};
        case 0xFC: return {"Emergency information", hex};
        case 0xFD: {
            if (b.size() < 2)
                return {"Data component", hex};
            const int id = u16(b, 0);
            QString value = QString("data_component_id 0x%1").arg(id, 4, 16, QChar('0'));
            if (id == 0x0008)
                value += " (ARIB legenda/teletexto)";
            if (b.size() >= 3) {
                const int info = static_cast<uint8_t>(b[2]);
                if (id == 0x0008) {
                    static const char* timing[] = {"assíncrono", "sincronizado ao programa", "sincronizado ao relógio", "reservado"};
                    value += QString(", DMF %1, timing %2")
                                 .arg((info >> 4) & 0x0F, 4, 2, QChar('0'))
                                 .arg(timing[info & 0x03]);
                } else {
                    value += ", info " + b.mid(2).toHex(' ').toUpper();
                }
            }
            return {"Data component", value};
        }
        case 0xFE: return {"System management", hex};
        default:
            return {QString("Descritor 0x%1").arg(d.tag, 2, 16, QChar('0')).toUpper(), hex};
    }
}
