#pragma once

#include <QImage>
#include <QRect>
#include <QVector>
#include <QWidget>

class CaptionEngine;

// 16:9 "TV screen" showing the caption canvas, with optional overlays for the caption plane,
// the display area set by SDF/SDP and the regions currently on screen.
class CaptionView : public QWidget {
    Q_OBJECT

public:
    explicit CaptionView(const CaptionEngine* engine, QWidget* parent = nullptr);

    void setOverlaysVisible(bool visible);
    void setDisplayArea(const QRect& planeRect);  // null rect = unknown
    void setSignalState(const QString& text, const QColor& color);

    QSize sizeHint() const override { return {960, 540}; }
    QSize minimumSizeHint() const override { return {320, 180}; }

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QRect screenRect() const;

    const CaptionEngine* engine_;
    bool overlays_ = true;
    QRect displayArea_;
    QString signalText_;
    QColor signalColor_;
};
