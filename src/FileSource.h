#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QFile>
#include <QObject>
#include <QString>
#include <QTimer>

// Plays an MPEG-TS file in real time, emitting 188-byte packets through the same path as the UDP receiver.
// The pace comes from the PCR, or from PES PTS when the file has no PCR (e.g. caption-only captures),
// so captions, video and audio arrive as they would on air. Accepts 188, 192 (M2TS) and 204-byte packets.
class FileSource : public QObject {
    Q_OBJECT

public:
    enum class State { Closed, Playing, Paused, Finished };

    explicit FileSource(QObject* parent = nullptr);

    bool open(const QString& path, QString* error);
    void close();
    void setPaused(bool paused);
    void setLoop(bool loop) { loop_ = loop; }

    State state() const { return state_; }
    bool isActive() const { return state_ == State::Playing || state_ == State::Paused; }
    QString path() const { return file_.fileName(); }
    int packetSize() const { return packetSize_; }
    QString clockSource() const;
    double positionSeconds() const;
    double durationSeconds() const { return duration_; }
    quint64 bytesSent() const { return bytesSent_; }
    int loops() const { return loops_; }

signals:
    void tsData(const QByteArray& data);
    void restarted();  // looped back to the beginning: downstream state must be reset
    void finished();

private:
    enum class Clock { None, Pcr, Pts };

    struct Timing {
        Clock clock = Clock::None;
        int pid = -1;
    };

    void tick();
    bool fillBuffer();
    void rewind();
    // Timestamp in seconds carried by the packet for the chosen clock, or -1.
    double packetTime(const uint8_t* packet) const;
    Timing detectTiming(const QByteArray& sample) const;
    double scanTime(const QByteArray& sample, bool last) const;

    QFile file_;
    QTimer timer_;
    QElapsedTimer wall_;
    State state_ = State::Closed;
    bool loop_ = false;
    int packetSize_ = 188;
    int syncOffset_ = 0;  // offset of the 0x47 sync byte inside each packet (4 for M2TS)
    qint64 dataStart_ = 0;
    Timing timing_;

    QByteArray buffer_;
    qsizetype bufferPos_ = 0;
    bool eof_ = false;

    double firstTime_ = -1;
    double lastTime_ = -1;
    double duration_ = 0;
    double baseStream_ = -1;
    qint64 baseWallMs_ = 0;
    qint64 pausedAtMs_ = 0;
    quint64 bytesSent_ = 0;
    int loops_ = 0;
};
