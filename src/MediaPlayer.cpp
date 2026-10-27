#include "MediaPlayer.h"

#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSink>
#include <QMediaDevices>
#include <QMetaObject>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QStringList>
#include <QThread>

#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

namespace {

constexpr qsizetype kMaxInputBytes = 8 * 1024 * 1024;
constexpr size_t kMaxVideoFrames = 90;
constexpr size_t kMaxAudioChunks = 400;
constexpr int kSampleRate = 48000;
constexpr int kBytesPerSample = 4;  // S16 stereo
constexpr int kSinkBufferMs = 300;
constexpr qint64 kStartDelayMs = 150;      // absorbs network jitter before the first frame
constexpr double kStartWindowSeconds = 0.3;  // slack kept behind the newest data when the clock (re)starts
constexpr qint64 kAudioLeadMs = 150;       // audio written this far ahead keeps the sink from underrunning
constexpr qint64 kLateResetMs = 1000;      // falling this far behind re-bases the clock
constexpr qint64 kFutureResetMs = 5000;    // a timestamp jump this far ahead re-bases the clock
constexpr qsizetype kMaxPendingAudio = kSampleRate * kBytesPerSample;  // 1 s

// FFmpeg logs go to the Log tab instead of stderr. av_log has a single global callback, so the player
// that is running registers itself here; consecutive repetitions are collapsed into a count.
std::atomic<MediaPlayer*> g_logTarget{nullptr};
// While probing, the decoder sees frames from before the first keyframe: their errors are only counted.
std::atomic<bool> g_probing{false};
std::atomic<int> g_probeErrors{0};
QMutex g_logMutex;
QString g_lastLog;
int g_lastLogRepeats = 0;

void ffmpegLogCallback(void* avcl, int level, const char* format, va_list args) {
    if (level > AV_LOG_ERROR)
        return;
    if (g_probing) {
        ++g_probeErrors;
        return;
    }
    char line[1024];
    int printPrefix = 1;
    av_log_format_line2(avcl, level, format, args, line, sizeof(line), &printPrefix);
    QString text = QString::fromUtf8(line).trimmed();
    text.remove(QRegularExpression(" @ 0x[0-9a-f]+"));

    MediaPlayer* target = g_logTarget.load();
    if (!target || text.isEmpty())
        return;
    QMutexLocker lock(&g_logMutex);
    if (text == g_lastLog) {
        ++g_lastLogRepeats;
        return;
    }
    QStringList out;
    if (g_lastLogRepeats > 0)
        out << QString("FFmpeg: %1 (repetida mais %2 vez(es))").arg(g_lastLog).arg(g_lastLogRepeats);
    out << "FFmpeg: " + text;
    g_lastLog = text;
    g_lastLogRepeats = 0;
    for (const auto& message : out)
        QMetaObject::invokeMethod(target, [target, message]() { emit target->logMessage(message); }, Qt::QueuedConnection);
}

QString ffmpegError(int code) {
    char text[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(code, text, sizeof(text));
    return QString::fromUtf8(text);
}

}  // namespace

MediaPlayer::MediaPlayer(QObject* parent) : QObject(parent) {
    presentTimer_.setTimerType(Qt::PreciseTimer);
    presentTimer_.setInterval(5);
    connect(&presentTimer_, &QTimer::timeout, this, &MediaPlayer::present);

    // Follow the system default output (e.g. a Bluetooth headset connected while playing).
    devices_ = new QMediaDevices(this);
    connect(devices_, &QMediaDevices::audioOutputsChanged, this, [this]() {
        if (thread_ && QMediaDevices::defaultAudioOutput().id() != sinkDeviceId_) {
            closeAudioOutput();
            openAudioOutput();
        }
    });
}

MediaPlayer::~MediaPlayer() {
    stop();
    MediaPlayer* self = this;
    g_logTarget.compare_exchange_strong(self, nullptr);
}

void MediaPlayer::start() {
    if (thread_)
        return;

    abort_ = false;
    {
        QMutexLocker lock(&inputMutex_);
        input_.clear();
        inputBytes_ = 0;
        inputOffset_ = 0;
    }
    {
        QMutexLocker lock(&outputMutex_);
        video_.clear();
        audio_.clear();
        info_ = {};
    }
    framesShown_ = 0;
    framesDropped_ = 0;
    framesDecoded_ = 0;
    clockResets_ = 0;
    clockSet_ = false;
    pendingAudio_.clear();

    openAudioOutput();
    // Registered here, not in the constructor: Qt Multimedia's own FFmpeg plugin installs its callback when
    // the audio backend loads and would replace ours.
    g_logTarget = this;
    av_log_set_callback(&ffmpegLogCallback);
    clock_.start();
    presentTimer_.start();
    thread_ = QThread::create([this]() { decodeLoop(); });
    thread_->setObjectName("MediaDecoder");
    thread_->start();
}

void MediaPlayer::stop() {
    if (!thread_)
        return;

    abort_ = true;
    inputReady_.wakeAll();
    thread_->wait();
    delete thread_;
    thread_ = nullptr;

    presentTimer_.stop();
    closeAudioOutput();
    {
        QMutexLocker lock(&outputMutex_);
        video_.clear();
        audio_.clear();
        info_ = {};
    }
    pendingAudio_.clear();
    emit stopped();
}

void MediaPlayer::openAudioOutput() {
    QAudioFormat format;
    format.setSampleRate(kSampleRate);
    format.setChannelCount(2);
    format.setSampleFormat(QAudioFormat::Int16);
    const QAudioDevice device = QMediaDevices::defaultAudioOutput();
    sinkDeviceId_ = device.id();
    if (device.isNull() || !device.isFormatSupported(format)) {
        emit logMessage("Mídia: nenhuma saída de áudio compatível (48 kHz estéreo); tocando só o vídeo");
        return;
    }
    sink_ = new QAudioSink(device, format, this);
    sink_->setBufferSize(kSampleRate * kBytesPerSample * kSinkBufferMs / 1000);
    sink_->setVolume(muted_ ? 0.0f : volume_);
    sinkDevice_ = sink_->start();
    emit logMessage("Mídia: saída de áudio " + device.description());
}

void MediaPlayer::closeAudioOutput() {
    if (!sink_)
        return;
    sink_->stop();
    delete sink_;
    sink_ = nullptr;
    sinkDevice_ = nullptr;
}

void MediaPlayer::feed(const QByteArray& ts) {
    if (!thread_)
        return;
    QMutexLocker lock(&inputMutex_);
    input_.push_back(ts);
    inputBytes_ += ts.size();
    // The decoder fell behind (or is stuck probing): keep only the most recent data.
    while (inputBytes_ > kMaxInputBytes && input_.size() > 1) {
        inputBytes_ -= input_.front().size() - inputOffset_;
        input_.pop_front();
        inputOffset_ = 0;
    }
    inputReady_.wakeOne();
}

void MediaPlayer::setVolume(float volume) {
    volume_ = volume;
    if (sink_ && !muted_)
        sink_->setVolume(volume_);
}

void MediaPlayer::setMuted(bool muted) {
    muted_ = muted;
    if (sink_)
        sink_->setVolume(muted_ ? 0.0f : volume_);
}

MediaInfo MediaPlayer::info() const {
    QMutexLocker lock(&outputMutex_);
    return info_;
}

int MediaPlayer::audioLatencyMs() const {
    if (!sink_)
        return 0;
    const qsizetype queued = sink_->bufferSize() - sink_->bytesFree() + pendingAudio_.size();
    return static_cast<int>(queued * 1000 / (kSampleRate * kBytesPerSample));
}

int MediaPlayer::readCallback(void* opaque, uint8_t* buffer, int size) {
    return static_cast<MediaPlayer*>(opaque)->readInput(buffer, size);
}

int MediaPlayer::interruptCallback(void* opaque) {
    return static_cast<MediaPlayer*>(opaque)->abort_ ? 1 : 0;
}

int MediaPlayer::readInput(uint8_t* buffer, int size) {
    QMutexLocker lock(&inputMutex_);
    while (input_.empty()) {
        if (abort_)
            return AVERROR_EXIT;
        inputReady_.wait(&inputMutex_, 100);
    }

    int copied = 0;
    while (copied < size && !input_.empty()) {
        const QByteArray& front = input_.front();
        const int n = static_cast<int>(qMin<qsizetype>(size - copied, front.size() - inputOffset_));
        std::memcpy(buffer + copied, front.constData() + inputOffset_, n);
        copied += n;
        inputOffset_ += n;
        inputBytes_ -= n;
        if (inputOffset_ >= front.size()) {
            input_.pop_front();
            inputOffset_ = 0;
        }
    }
    return copied;
}

void MediaPlayer::decodeLoop() {
    constexpr int kIoBufferSize = 188 * 64;
    auto* ioBuffer = static_cast<uint8_t*>(av_malloc(kIoBufferSize));
    AVIOContext* io = avio_alloc_context(ioBuffer, kIoBufferSize, 0, this, &MediaPlayer::readCallback, nullptr, nullptr);
    AVFormatContext* format = avformat_alloc_context();
    format->pb = io;
    format->interrupt_callback.callback = &MediaPlayer::interruptCallback;
    format->interrupt_callback.opaque = this;
    format->probesize = 2'000'000;
    format->max_analyze_duration = 2 * AV_TIME_BASE;

    AVCodecContext* videoCodec = nullptr;
    AVCodecContext* audioCodec = nullptr;
    SwsContext* scaler = nullptr;
    SwrContext* resampler = nullptr;
    AVChannelLayout resamplerLayout{};
    int resamplerRate = 0;
    int resamplerFormat = -1;
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();

    auto cleanup = [&]() {
        av_frame_free(&frame);
        av_packet_free(&packet);
        swr_free(&resampler);
        av_channel_layout_uninit(&resamplerLayout);
        sws_freeContext(scaler);
        avcodec_free_context(&videoCodec);
        avcodec_free_context(&audioCodec);
        avformat_close_input(&format);
        if (io) {
            av_freep(&io->buffer);
            avio_context_free(&io);
        }
    };

    int ret = avformat_open_input(&format, nullptr, av_find_input_format("mpegts"), nullptr);
    if (ret < 0) {
        if (!abort_)
            emit logMessage("Mídia: falha ao abrir o MPEG-TS: " + ffmpegError(ret));
        cleanup();
        return;
    }
    g_probeErrors = 0;
    g_probing = true;
    ret = avformat_find_stream_info(format, nullptr);
    g_probing = false;
    if (g_probeErrors > 0)
        emit logMessage(QString("Mídia: %1 aviso(s) do FFmpeg ignorados durante a análise inicial "
                                "(normal ao entrar no meio do stream, antes do primeiro quadro-chave)")
                            .arg(g_probeErrors.load()));
    if (ret < 0 || abort_) {
        if (!abort_)
            emit logMessage("Mídia: falha ao analisar os streams: " + ffmpegError(ret));
        cleanup();
        return;
    }

    auto openDecoder = [&](AVMediaType type, int* index) -> AVCodecContext* {
        const AVCodec* codec = nullptr;
        *index = av_find_best_stream(format, type, -1, -1, &codec, 0);
        if (*index < 0 || !codec)
            return nullptr;
        AVCodecContext* context = avcodec_alloc_context3(codec);
        avcodec_parameters_to_context(context, format->streams[*index]->codecpar);
        context->thread_count = 0;
        if (avcodec_open2(context, codec, nullptr) < 0) {
            avcodec_free_context(&context);
            *index = -1;
        }
        return context;
    };
    int videoIndex = -1;
    int audioIndex = -1;
    videoCodec = openDecoder(AVMEDIA_TYPE_VIDEO, &videoIndex);
    audioCodec = openDecoder(AVMEDIA_TYPE_AUDIO, &audioIndex);
    for (unsigned i = 0; i < format->nb_streams; ++i)
        if (static_cast<int>(i) != videoIndex && static_cast<int>(i) != audioIndex)
            format->streams[i]->discard = AVDISCARD_ALL;

    {
        QMutexLocker lock(&outputMutex_);
        if (videoCodec) {
            const AVStream* stream = format->streams[videoIndex];
            info_.hasVideo = true;
            info_.videoCodec = QString::fromUtf8(avcodec_get_name(videoCodec->codec_id));
            info_.videoSize = QSize(videoCodec->width, videoCodec->height);
            info_.frameRate = av_q2d(stream->avg_frame_rate.num ? stream->avg_frame_rate : stream->r_frame_rate);
        }
        if (audioCodec) {
            info_.hasAudio = true;
            info_.audioCodec = QString::fromUtf8(avcodec_get_name(audioCodec->codec_id));
            info_.sampleRate = audioCodec->sample_rate;
            info_.channels = audioCodec->ch_layout.nb_channels;
        }
    }
    emit logMessage(QString("Mídia: vídeo %1, áudio %2")
                        .arg(videoCodec ? QString::fromUtf8(avcodec_get_name(videoCodec->codec_id)) : "ausente")
                        .arg(audioCodec ? QString::fromUtf8(avcodec_get_name(audioCodec->codec_id)) : "ausente"));
    if (!videoCodec && !audioCodec) {
        cleanup();
        return;
    }

    auto handleVideo = [&]() {
        const AVStream* stream = format->streams[videoIndex];
        const int width = frame->width;
        const int height = frame->height;
        scaler = sws_getCachedContext(scaler, width, height, static_cast<AVPixelFormat>(frame->format), width, height,
                                      AV_PIX_FMT_RGB32, SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!scaler)
            return;
        QImage image(width, height, QImage::Format_RGB32);
        uint8_t* destination[1] = {image.bits()};
        const int destinationStride[1] = {static_cast<int>(image.bytesPerLine())};
        sws_scale(scaler, frame->data, frame->linesize, 0, height, destination, destinationStride);

        const AVRational sar = av_guess_sample_aspect_ratio(format, const_cast<AVStream*>(stream), frame);
        const double pixelAspect = sar.num > 0 && sar.den > 0 ? av_q2d(sar) : 1.0;
        const int64_t timestamp = frame->best_effort_timestamp;
        if (timestamp == AV_NOPTS_VALUE)
            return;

        ++framesDecoded_;
        QMutexLocker lock(&outputMutex_);
        info_.videoSize = QSize(width, height);
        info_.interlaced = frame->flags & AV_FRAME_FLAG_INTERLACED;
        video_.push_back({timestamp * av_q2d(stream->time_base), std::move(image), pixelAspect});
        while (video_.size() > kMaxVideoFrames) {
            video_.pop_front();
            ++framesDropped_;
        }
    };

    auto handleAudio = [&]() {
        const AVStream* stream = format->streams[audioIndex];
        if (!resampler || frame->sample_rate != resamplerRate || frame->format != resamplerFormat ||
            av_channel_layout_compare(&frame->ch_layout, &resamplerLayout) != 0) {
            swr_free(&resampler);
            const AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
            if (swr_alloc_set_opts2(&resampler, &stereo, AV_SAMPLE_FMT_S16, kSampleRate, &frame->ch_layout,
                                    static_cast<AVSampleFormat>(frame->format), frame->sample_rate, 0, nullptr) < 0 ||
                swr_init(resampler) < 0) {
                swr_free(&resampler);
                return;
            }
            av_channel_layout_uninit(&resamplerLayout);
            av_channel_layout_copy(&resamplerLayout, &frame->ch_layout);
            resamplerRate = frame->sample_rate;
            resamplerFormat = frame->format;
        }
        const int64_t timestamp = frame->best_effort_timestamp;
        if (timestamp == AV_NOPTS_VALUE)
            return;

        const int capacity = swr_get_out_samples(resampler, frame->nb_samples);
        QByteArray pcm(capacity * kBytesPerSample, Qt::Uninitialized);
        uint8_t* output[1] = {reinterpret_cast<uint8_t*>(pcm.data())};
        const int samples = swr_convert(resampler, output, capacity,
                                        const_cast<const uint8_t**>(frame->extended_data), frame->nb_samples);
        if (samples <= 0)
            return;
        pcm.resize(samples * kBytesPerSample);

        QMutexLocker lock(&outputMutex_);
        audio_.push_back({timestamp * av_q2d(stream->time_base), std::move(pcm)});
        while (audio_.size() > kMaxAudioChunks)
            audio_.pop_front();
    };

    auto decode = [&](AVCodecContext* context, auto&& handle) {
        if (avcodec_send_packet(context, packet) < 0)
            return;
        while (avcodec_receive_frame(context, frame) == 0) {
            handle();
            av_frame_unref(frame);
        }
    };

    // Like a TV receiver joining mid-stream, ignore video until the first keyframe: earlier frames
    // reference parameter sets (SPS/PPS) the decoder has not seen and only produce errors.
    bool waitingKeyframe = videoCodec != nullptr;
    QElapsedTimer sinceStart;
    sinceStart.start();
    bool warnedNoVideo = false;

    while (!abort_) {
        if (videoCodec && !warnedNoVideo && framesDecoded_ == 0 && sinceStart.elapsed() > 10000) {
            warnedNoVideo = true;
            emit logMessage(waitingKeyframe ? "Mídia: 10 s sem quadro-chave (IDR) no vídeo"
                                            : "Mídia: 10 s sem conseguir decodificar o vídeo — SPS/PPS ausentes junto do quadro-chave?");
        }
        ret = av_read_frame(format, packet);
        if (ret == AVERROR_EXIT || ret == AVERROR_EOF || abort_)
            break;
        if (ret < 0)
            continue;
        if (packet->stream_index == videoIndex) {
            if (waitingKeyframe && !(packet->flags & AV_PKT_FLAG_KEY)) {
                av_packet_unref(packet);
                continue;
            }
            waitingKeyframe = false;
            decode(videoCodec, handleVideo);
        }
        else if (packet->stream_index == audioIndex)
            decode(audioCodec, handleAudio);
        av_packet_unref(packet);
    }

    cleanup();
}

void MediaPlayer::writeAudio() {
    if (!sinkDevice_ || pendingAudio_.isEmpty())
        return;
    if (pendingAudio_.size() > kMaxPendingAudio)
        pendingAudio_.remove(0, pendingAudio_.size() - kMaxPendingAudio);
    const qsizetype room = sink_->bytesFree() / kBytesPerSample * kBytesPerSample;
    const qsizetype length = qMin(room, pendingAudio_.size());
    if (length <= 0)
        return;
    const qint64 written = sinkDevice_->write(pendingAudio_.constData(), length);
    if (written > 0)
        pendingAudio_.remove(0, written);
}

void MediaPlayer::resetClock() {
    clockSet_ = false;
    ++clockResets_;
}

void MediaPlayer::present() {
    writeAudio();
    const qint64 now = clock_.elapsed();

    VideoItem current;
    bool haveFrame = false;
    {
        QMutexLocker lock(&outputMutex_);
        if (!clockSet_) {
            if (video_.empty() && audio_.empty())
                return;
            // With video, wait for its first frame (audio is usually muxed ahead of it), unless enough audio
            // piled up to show the video is not coming.
            if (info_.hasVideo && video_.empty() && audio_.size() < kMaxAudioChunks / 2)
                return;
            // Start from recent data: what piled up while FFmpeg analysed the stream (or while the presentation
            // fell behind) is skipped, otherwise it would become permanent latency and the video would lag the
            // captions, which are shown as soon as they arrive. The stream that is further behind (usually the
            // video, which also has decoder delay) sets the start, so it keeps some slack.
            double newest;
            if (!video_.empty() && !audio_.empty())
                newest = qMin(video_.back().pts, audio_.back().pts);
            else
                newest = video_.empty() ? audio_.back().pts : video_.back().pts;
            basePts_ = newest - kStartWindowSeconds;
            while (!audio_.empty() && audio_.front().pts < basePts_)
                audio_.pop_front();
            while (video_.size() > 1 && video_[1].pts <= basePts_)
                video_.pop_front();
            baseMs_ = now + kStartDelayMs;
            clockSet_ = true;
        }
        auto due = [&](double pts) { return baseMs_ + static_cast<qint64>((pts - basePts_) * 1000.0); };

        // Audio is written kAudioLeadMs ahead of its time so the sink never runs dry. A sample written now
        // is heard after everything already queued, so the video is shifted by (queued - lead) to match.
        while (!audio_.empty() && due(audio_.front().pts) <= now + kAudioLeadMs) {
            if (now - due(audio_.front().pts) > kLateResetMs) {
                resetClock();
                return;
            }
            pendingAudio_.append(audio_.front().pcm);
            audio_.pop_front();
        }

        const qint64 latency = sink_ ? audioLatencyMs() - kAudioLeadMs : 0;
        while (!video_.empty() && due(video_.front().pts) + latency <= now) {
            if (haveFrame)
                ++framesDropped_;
            current = std::move(video_.front());
            video_.pop_front();
            haveFrame = true;
        }
        if (haveFrame && now - (due(current.pts) + latency) > kLateResetMs)
            resetClock();
        const bool jumpedAhead = (!video_.empty() && due(video_.front().pts) - now > kFutureResetMs) ||
                                 (!audio_.empty() && due(audio_.front().pts) - now > kFutureResetMs);
        if (jumpedAhead)
            resetClock();
    }

    writeAudio();
    if (haveFrame) {
        ++framesShown_;
        emit videoFrame(current.image, current.pixelAspect);
    }
}
