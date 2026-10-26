#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QImage>
#include <QMutex>
#include <QObject>
#include <QSize>
#include <QString>
#include <QTimer>
#include <QWaitCondition>

#include <atomic>
#include <deque>

class QAudioSink;
class QIODevice;
class QMediaDevices;
class QThread;

struct MediaInfo {
    bool hasVideo = false;
    bool hasAudio = false;
    QString videoCodec;
    QSize videoSize;
    double frameRate = 0;
    bool interlaced = false;
    QString audioCodec;
    int sampleRate = 0;
    int channels = 0;
};

// Plays the video and audio carried in the same MPEG-TS as the captions. FFmpeg demuxes and decodes in a
// worker thread fed with the raw UDP payload; presentation (clock, audio output, frame switching) runs in
// the GUI thread. Captions are not synchronized to this clock: they keep being shown when they arrive.
class MediaPlayer : public QObject {
    Q_OBJECT

public:
    explicit MediaPlayer(QObject* parent = nullptr);
    ~MediaPlayer() override;

    void start();
    void stop();
    bool isRunning() const { return thread_ != nullptr; }

    // Thread-safe; called with every UDP payload while running.
    void feed(const QByteArray& ts);

    void setVolume(float volume);
    void setMuted(bool muted);

    MediaInfo info() const;
    quint64 framesShown() const { return framesShown_; }
    quint64 framesDropped() const { return framesDropped_; }
    quint64 clockResets() const { return clockResets_; }
    int audioLatencyMs() const;

signals:
    void videoFrame(const QImage& frame, double pixelAspect);
    void stopped();
    void logMessage(const QString& message);

private:
    struct VideoItem {
        double pts;
        QImage image;
        double pixelAspect;
    };
    struct AudioItem {
        double pts;
        QByteArray pcm;  // S16 interleaved stereo, 48 kHz
    };

    void decodeLoop();
    int readInput(uint8_t* buffer, int size);
    void present();
    void writeAudio();
    void resetClock();
    void openAudioOutput();
    void closeAudioOutput();

    static int readCallback(void* opaque, uint8_t* buffer, int size);
    static int interruptCallback(void* opaque);

    // Input (GUI thread -> decoder thread)
    QMutex inputMutex_;
    QWaitCondition inputReady_;
    std::deque<QByteArray> input_;
    qsizetype inputBytes_ = 0;
    qsizetype inputOffset_ = 0;
    std::atomic<bool> abort_{false};
    QThread* thread_ = nullptr;

    // Decoded output (decoder thread -> GUI thread)
    mutable QMutex outputMutex_;
    std::deque<VideoItem> video_;
    std::deque<AudioItem> audio_;
    MediaInfo info_;

    // Presentation (GUI thread)
    QTimer presentTimer_;
    QElapsedTimer clock_;
    bool clockSet_ = false;
    double basePts_ = 0;
    qint64 baseMs_ = 0;
    QMediaDevices* devices_ = nullptr;
    QByteArray sinkDeviceId_;
    QAudioSink* sink_ = nullptr;
    QIODevice* sinkDevice_ = nullptr;
    QByteArray pendingAudio_;
    float volume_ = 1.0f;
    bool muted_ = false;
    std::atomic<quint64> framesShown_{0};
    std::atomic<quint64> framesDropped_{0};
    std::atomic<quint64> framesDecoded_{0};
    quint64 clockResets_ = 0;
};
