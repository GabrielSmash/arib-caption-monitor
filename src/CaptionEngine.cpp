#include "CaptionEngine.h"

#include <QPainter>
#include <QStringList>

#include <algorithm>
#include <cmath>

using namespace aribcaption;

CaptionEngine::CaptionEngine(QObject* parent)
    : QObject(parent), decoder_(context_), renderer_(context_),
      screen_(kFrameWidth, kFrameHeight, QImage::Format_RGBA8888_Premultiplied) {
    screen_.fill(Qt::transparent);
    waitTimer_.setSingleShot(true);
    connect(&waitTimer_, &QTimer::timeout, this, [this]() {
        clearScreen();
        emit screenChanged();
    });
}

bool CaptionEngine::initialize(QString* error) {
    context_.SetLogcatCallback([this](LogLevel level, const char* message) {
        if (level != LogLevel::kVerbose)
            emit logMessage(QString("libaribcaption: %1").arg(QString::fromUtf8(message)));
    });

    // Brazilian captions (SBTVD / ISDB-Tb) must use the ABNT NBR 15606-1 Latin table;
    // kAuto or the JIS scheme would decode the bytes as Japanese characters.
    if (!decoder_.Initialize(EncodingScheme::kABNT_NBR_15606_1_Latin, CaptionType::kCaption, Profile::kProfileA,
                             LanguageId::kFirst)) {
        *error = "Falha ao inicializar o decoder libaribcaption.";
        return false;
    }
    if (!renderer_.Initialize(CaptionType::kCaption, FontProviderType::kFontconfig, TextRendererType::kFreetype)) {
        *error = "Falha ao inicializar o renderer libaribcaption.";
        return false;
    }
    // ARIB places every character in a fixed-size cell (SSM + SHS), so a proportional font looks letter-spaced.
    renderer_.SetDefaultFontFamily({"DejaVu Sans Mono", "Noto Sans Mono", "Liberation Mono", "monospace"}, true);
    renderer_.SetFrameSize(kFrameWidth, kFrameHeight);
    renderer_.SetMargins(0, 0, 0, 0);
    renderer_.SetStoragePolicy(CaptionStoragePolicy::kMinimum);
    return true;
}

void CaptionEngine::reset() {
    decoder_.Flush();
    renderer_.Flush();
    clearScreen();
    emit screenChanged();
}

bool CaptionEngine::processPes(const QByteArray& payload, qint64 pts) {
    DecodeResult result;
    const auto status = decoder_.Decode(reinterpret_cast<const uint8_t*>(payload.constData()),
                                        static_cast<size_t>(payload.size()), pts, result);
    if (status == DecodeStatus::kError) {
        ++decodeErrors_;
        return false;
    }
    if (status != DecodeStatus::kGotCaption || !result.caption)
        return false;

    ++captionsDecoded_;
    applyCaption(*result.caption);
    return true;
}

void CaptionEngine::applyCaption(Caption& caption) {
    const bool clear = caption.flags & kCaptionFlagsClearScreen;
    if (clear)
        clearScreen();
    lastCaptionRegions_.clear();
    if (caption.plane_width > 0 && caption.plane_height > 0)
        planeSize_ = QSize(caption.plane_width, caption.plane_height);

    // Captions are shown when they arrive (live monitor), so the PTS only matters to the renderer lookup.
    if (caption.pts == PTS_NOPTS)
        caption.pts = 0;

    if (!caption.regions.empty()) {
        const QRect area = captionAreaInFrame();
        const double scale = static_cast<double>(area.width()) / planeSize_.width();
        auto toFrame = [&](const CaptionRegion& r) {
            return QRect(area.x() + static_cast<int>(std::floor(r.x * scale)),
                         area.y() + static_cast<int>(std::floor(r.y * scale)),
                         static_cast<int>(std::ceil(r.width * scale)), static_cast<int>(std::ceil(r.height * scale)));
        };

        QPainter painter(&screen_);
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        for (const auto& region : caption.regions) {
            // A new character cell replaces whatever was drawn at the same position.
            painter.fillRect(toFrame(region), Qt::transparent);

            const QRect planeRect(region.x, region.y, region.width, region.height);
            QString text;
            for (const auto& ch : region.chars)
                text += QString::fromUtf8(ch.u8str);
            regions_.erase(std::remove_if(regions_.begin(), regions_.end(),
                                          [&](const Region& r) { return r.rect.intersects(planeRect); }),
                           regions_.end());
            regions_.append({planeRect, text});
            lastCaptionRegions_.append({planeRect, text});
        }

        renderer_.Flush();
        renderer_.AppendCaption(caption);
        RenderResult rendered;
        const auto status = renderer_.Render(caption.pts, rendered);
        if (status == RenderStatus::kGotImage || status == RenderStatus::kGotImageUnchanged) {
            painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
            for (const auto& image : rendered.images) {
                const QImage view(image.bitmap.data(), image.width, image.height, image.stride,
                                  QImage::Format_RGBA8888);
                painter.drawImage(image.dst_x, image.dst_y, view);
            }
        } else if (status == RenderStatus::kError) {
            emit logMessage("Falha ao renderizar a legenda.");
        }
    }

    if ((caption.flags & kCaptionFlagsWaitDuration) && caption.wait_duration != DURATION_INDEFINITE)
        waitTimer_.start(static_cast<int>(caption.wait_duration));
    else
        waitTimer_.stop();

    std::sort(regions_.begin(), regions_.end(), [](const Region& a, const Region& b) {
        return a.rect.y() != b.rect.y() ? a.rect.y() < b.rect.y() : a.rect.x() < b.rect.x();
    });

    emit captionDecoded(QString::fromStdString(caption.text), caption.pts, clear);
    emit screenChanged();
}

void CaptionEngine::clearScreen() {
    screen_.fill(Qt::transparent);
    regions_.clear();
    waitTimer_.stop();
}

QRect CaptionEngine::captionAreaInFrame() const {
    // Same fit-and-center rule libaribcaption uses to place the caption plane inside the frame.
    const double magnification = std::min(static_cast<double>(kFrameWidth) / planeSize_.width(),
                                          static_cast<double>(kFrameHeight) / planeSize_.height());
    const int width = static_cast<int>(std::floor(planeSize_.width() * magnification));
    const int height = static_cast<int>(std::floor(planeSize_.height() * magnification));
    return QRect((kFrameWidth - width) / 2, (kFrameHeight - height) / 2, width, height);
}

QString CaptionEngine::screenText() const {
    QStringList lines;
    for (const auto& region : regions_)
        lines << region.text;
    return lines.join('\n');
}
