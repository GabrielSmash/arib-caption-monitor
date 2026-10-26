#pragma once

#include <QImage>
#include <QObject>
#include <QRect>
#include <QSize>
#include <QTimer>
#include <QVector>

#include <aribcaption/aribcaption.hpp>

// Decodes ARIB caption PES with libaribcaption (ABNT NBR 15606-1 Latin) and keeps a persistent
// caption screen. libaribcaption's renderer only shows the latest statement, but on a real receiver
// a statement without CS writes over what is already on screen, so the screen is composited here
// and only cleared by CS (or by a TIME wait expiring).
class CaptionEngine : public QObject {
    Q_OBJECT

public:
    struct Region {
        QRect rect;  // caption plane coordinates
        QString text;
    };

    explicit CaptionEngine(QObject* parent = nullptr);

    bool initialize(QString* error);
    void reset();
    // Returns true when the PES produced a caption (statement), false otherwise.
    bool processPes(const QByteArray& payload, qint64 pts);

    const QImage& screen() const { return screen_; }
    QSize planeSize() const { return planeSize_; }
    QRect captionAreaInFrame() const;
    const QVector<Region>& regions() const { return regions_; }
    // Regions written by the most recent caption only (regions() also keeps earlier ones still on screen).
    const QVector<Region>& lastCaptionRegions() const { return lastCaptionRegions_; }
    QString screenText() const;
    quint64 decodeErrors() const { return decodeErrors_; }
    quint64 captionsDecoded() const { return captionsDecoded_; }

signals:
    void screenChanged();
    void captionDecoded(const QString& text, qint64 pts, bool clearScreen);
    void logMessage(const QString& message);

private:
    void applyCaption(aribcaption::Caption& caption);
    void clearScreen();

    static constexpr int kFrameWidth = 1920;
    static constexpr int kFrameHeight = 1080;

    aribcaption::Context context_;
    aribcaption::Decoder decoder_;
    aribcaption::Renderer renderer_;
    QImage screen_;
    QSize planeSize_{960, 540};
    QVector<Region> regions_;
    QVector<Region> lastCaptionRegions_;
    QTimer waitTimer_;
    quint64 decodeErrors_ = 0;
    quint64 captionsDecoded_ = 0;
};
