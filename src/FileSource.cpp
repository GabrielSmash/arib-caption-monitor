#include "FileSource.h"

namespace {

constexpr int kTsPacket = 188;
constexpr int kTickMs = 10;
constexpr qint64 kReadChunk = 1024 * 1024;
constexpr qint64 kSampleSize = 8 * 1024 * 1024;
constexpr int kMaxPacketsPerTick = 4000;
constexpr qint64 kRebaseBehindMs = 5000;   // timestamps jumping back/forward this much re-base the clock
constexpr qint64 kRebaseAheadMs = 10000;
constexpr double kFallbackBitrate = 2'000'000;  // only for files with neither PCR nor PTS
constexpr double kPtsWrap = 8589934592.0 / 90000.0;  // 2^33 ticks of 90 kHz

}  // namespace

FileSource::FileSource(QObject* parent) : QObject(parent) {
    timer_.setTimerType(Qt::PreciseTimer);
    timer_.setInterval(kTickMs);
    connect(&timer_, &QTimer::timeout, this, &FileSource::tick);
}

bool FileSource::open(const QString& path, QString* error) {
    close();
    file_.setFileName(path);
    if (!file_.open(QIODevice::ReadOnly)) {
        *error = QString("Não foi possível abrir %1: %2").arg(path, file_.errorString());
        return false;
    }

    const QByteArray head = file_.read(kSampleSize);
    const auto* bytes = reinterpret_cast<const uint8_t*>(head.constData());
    int stride = 0;
    qsizetype syncPos = -1;
    for (int candidate : {188, 192, 204}) {
        for (qsizetype i = 0; i < qMin<qsizetype>(head.size(), 1024) && syncPos < 0; ++i) {
            bool ok = true;
            for (int k = 0; k < 10 && ok; ++k)
                ok = i + k * candidate < head.size() && bytes[i + k * candidate] == 0x47;
            if (ok) {
                syncPos = i;
                stride = candidate;
            }
        }
        if (syncPos >= 0)
            break;
    }
    if (syncPos < 0) {
        *error = QString("%1 não parece ser um MPEG-TS (sem sincronismo 0x47 em pacotes de 188/192/204 bytes)").arg(path);
        file_.close();
        return false;
    }

    packetSize_ = stride;
    syncOffset_ = stride == 192 ? 4 : 0;  // M2TS: 4-byte timestamp header before each TS packet
    dataStart_ = syncPos - syncOffset_;
    if (dataStart_ < 0)
        dataStart_ += stride;
    const qint64 dataStart = dataStart_;

    // Normalized 188-byte view of the head and tail, used to pick the clock and estimate the duration.
    auto normalize = [&](const QByteArray& raw, qsizetype start) {
        QByteArray out;
        for (qsizetype i = start; i + stride <= raw.size(); i += stride)
            if (static_cast<uint8_t>(raw[i + syncOffset_]) == 0x47)
                out.append(raw.constData() + i + syncOffset_, kTsPacket);
        return out;
    };
    const QByteArray headPackets = normalize(head, dataStart);
    timing_ = detectTiming(headPackets);

    const qint64 size = file_.size();
    qint64 tailStart = qMax<qint64>(dataStart, size - kSampleSize);
    tailStart = dataStart + (tailStart - dataStart) / stride * stride;
    file_.seek(tailStart);
    const QByteArray tailPackets = normalize(file_.read(kSampleSize), 0);

    firstTime_ = scanTime(headPackets, false);
    const double last = scanTime(tailPackets, true);
    duration_ = 0;
    if (timing_.clock == Clock::None)
        duration_ = (size - dataStart) * 8.0 / kFallbackBitrate;
    else if (firstTime_ >= 0 && last >= 0)
        duration_ = last >= firstTime_ ? last - firstTime_ : last + kPtsWrap - firstTime_;

    rewind();
    loops_ = 0;
    bytesSent_ = 0;
    state_ = State::Playing;
    wall_.start();
    timer_.start();
    return true;
}

void FileSource::close() {
    timer_.stop();
    if (file_.isOpen())
        file_.close();
    buffer_.clear();
    bufferPos_ = 0;
    state_ = State::Closed;
}

void FileSource::setPaused(bool paused) {
    if (paused && state_ == State::Playing) {
        timer_.stop();
        state_ = State::Paused;
    } else if (!paused && state_ == State::Paused) {
        baseStream_ = -1;  // re-base on the next timestamp so the pause does not cause a burst
        state_ = State::Playing;
        timer_.start();
    }
}

QString FileSource::clockSource() const {
    const QString pid = QString("PID 0x%1").arg(timing_.pid, 4, 16, QChar('0'));
    switch (timing_.clock) {
        case Clock::Pcr: return "PCR (" + pid + ")";
        case Clock::Pts: return "PTS (" + pid + ", arquivo sem PCR)";
        case Clock::None: break;
    }
    return QString("sem PCR/PTS — taxa fixa de %1 Mbps").arg(kFallbackBitrate / 1e6);
}

double FileSource::positionSeconds() const {
    if (lastTime_ < 0 || firstTime_ < 0)
        return 0;
    if (timing_.clock == Clock::None)
        return lastTime_;
    return lastTime_ >= firstTime_ ? lastTime_ - firstTime_ : lastTime_ + kPtsWrap - firstTime_;
}

void FileSource::rewind() {
    file_.seek(dataStart_);
    buffer_.clear();
    bufferPos_ = 0;
    eof_ = false;
    baseStream_ = -1;
    lastTime_ = -1;
}

bool FileSource::fillBuffer() {
    if (eof_)
        return false;
    buffer_.remove(0, bufferPos_);
    bufferPos_ = 0;
    const QByteArray chunk = file_.read(kReadChunk);
    if (chunk.isEmpty())
        eof_ = true;
    buffer_.append(chunk);
    return buffer_.size() >= packetSize_;
}

FileSource::Timing FileSource::detectTiming(const QByteArray& sample) const {
    const auto* b = reinterpret_cast<const uint8_t*>(sample.constData());
    Timing ptsTiming;
    for (qsizetype i = 0; i + kTsPacket <= sample.size(); i += kTsPacket) {
        const uint8_t* p = b + i;
        const int pid = ((p[1] & 0x1F) << 8) | p[2];
        const int adaptation = (p[3] >> 4) & 0x03;
        if ((adaptation & 0x02) && p[4] >= 7 && (p[5] & 0x10))
            return {Clock::Pcr, pid};
        if (ptsTiming.pid < 0 && (p[1] & 0x40) && (adaptation & 0x01)) {
            const int offset = 4 + ((adaptation & 0x02) ? 1 + p[4] : 0);
            if (offset + 14 <= kTsPacket && p[offset] == 0 && p[offset + 1] == 0 && p[offset + 2] == 1 &&
                (p[offset + 7] & 0x80))
                ptsTiming = {Clock::Pts, pid};
        }
    }
    return ptsTiming;
}

double FileSource::packetTime(const uint8_t* p) const {
    const int pid = ((p[1] & 0x1F) << 8) | p[2];
    if (pid != timing_.pid)
        return -1;
    const int adaptation = (p[3] >> 4) & 0x03;
    if (timing_.clock == Clock::Pcr) {
        if (!(adaptation & 0x02) || p[4] < 7 || !(p[5] & 0x10))
            return -1;
        const quint64 base = (static_cast<quint64>(p[6]) << 25) | (static_cast<quint64>(p[7]) << 17) |
                             (static_cast<quint64>(p[8]) << 9) | (static_cast<quint64>(p[9]) << 1) | (p[10] >> 7);
        const int extension = ((p[10] & 0x01) << 8) | p[11];
        return (base * 300 + extension) / 27'000'000.0;
    }
    if (timing_.clock == Clock::Pts) {
        if (!(p[1] & 0x40) || !(adaptation & 0x01))
            return -1;
        const int offset = 4 + ((adaptation & 0x02) ? 1 + p[4] : 0);
        if (offset + 14 > kTsPacket || p[offset] != 0 || p[offset + 1] != 0 || p[offset + 2] != 1 ||
            !(p[offset + 7] & 0x80))
            return -1;
        const uint8_t* t = p + offset + 9;
        const quint64 pts = (static_cast<quint64>(t[0] & 0x0E) << 29) | (static_cast<quint64>(t[1]) << 22) |
                            (static_cast<quint64>(t[2] & 0xFE) << 14) | (static_cast<quint64>(t[3]) << 7) | (t[4] >> 1);
        return pts / 90000.0;
    }
    return -1;
}

double FileSource::scanTime(const QByteArray& sample, bool last) const {
    double found = -1;
    const auto* b = reinterpret_cast<const uint8_t*>(sample.constData());
    for (qsizetype i = 0; i + kTsPacket <= sample.size(); i += kTsPacket) {
        const double t = packetTime(b + i);
        if (t < 0)
            continue;
        found = t;
        if (!last)
            break;
    }
    return found;
}

void FileSource::tick() {
    if (state_ != State::Playing)
        return;

    const qint64 now = wall_.elapsed();
    QByteArray out;
    int packets = 0;
    while (packets < kMaxPacketsPerTick) {
        if (bufferPos_ + packetSize_ > buffer_.size() && !fillBuffer())
            break;

        const auto* record = reinterpret_cast<const uint8_t*>(buffer_.constData()) + bufferPos_;
        const uint8_t* packet = record + syncOffset_;
        if (packet[0] != 0x47) {
            bufferPos_ += 1;  // lost sync: slide until the next sync byte lines up
            continue;
        }

        double t = timing_.clock == Clock::None ? bytesSent_ * 8.0 / kFallbackBitrate : packetTime(packet);
        if (t >= 0) {
            if (baseStream_ < 0) {
                baseStream_ = t;
                baseWallMs_ = now;
            }
            qint64 target = baseWallMs_ + static_cast<qint64>((t - baseStream_) * 1000.0);
            if (target < now - kRebaseBehindMs || target > now + kRebaseAheadMs) {
                // Timestamp discontinuity (wrap, splice) or the machine fell far behind: restart the pace here.
                baseStream_ = t;
                baseWallMs_ = now;
                target = now;
            }
            if (target > now)
                break;
            lastTime_ = t;
        }

        out.append(reinterpret_cast<const char*>(packet), kTsPacket);
        bufferPos_ += packetSize_;
        ++packets;
    }

    if (!out.isEmpty()) {
        bytesSent_ += out.size();
        emit tsData(out);
    }

    if (eof_ && bufferPos_ + packetSize_ > buffer_.size()) {
        if (loop_) {
            ++loops_;
            rewind();
            emit restarted();
        } else {
            timer_.stop();
            state_ = State::Finished;
            emit finished();
        }
    }
}
