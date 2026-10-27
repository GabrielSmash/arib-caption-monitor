#include "CaptionView.h"

#include "CaptionEngine.h"

#include <QPainter>

CaptionView::CaptionView(const CaptionEngine* engine, QWidget* parent) : QWidget(parent), engine_(engine) {
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

void CaptionView::setOverlaysVisible(bool visible) {
    overlays_ = visible;
    update();
}

void CaptionView::setDisplayArea(const QRect& planeRect) {
    if (displayArea_ == planeRect)
        return;
    displayArea_ = planeRect;
    update();
}

void CaptionView::setSignalState(const QString& text, const QColor& color) {
    if (signalText_ == text && signalColor_ == color)
        return;
    signalText_ = text;
    signalColor_ = color;
    update();
}

void CaptionView::setVideoFrame(const QImage& frame, double pixelAspect) {
    video_ = frame;
    pixelAspect_ = pixelAspect;
    update();
}

void CaptionView::captionsChanged() {
    canvasDirty_ = true;
    update();
}

void CaptionView::clearVideo() {
    video_ = QImage();
    update();
}

QRect CaptionView::screenRect() const {
    const QRect available = rect().adjusted(8, 8, -8, -8);
    QSize size(16, 9);
    size.scale(available.size(), Qt::KeepAspectRatio);
    QRect r(QPoint(0, 0), size);
    r.moveCenter(available.center());
    return r;
}

void CaptionView::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), palette().window());

    const QRect screen = screenRect();
    p.fillRect(screen, QColor(16, 16, 16));

    if (!video_.isNull()) {
        // Fit the picture with its display aspect ratio (pixel aspect, e.g. 1440x1080 anamorphic -> 16:9).
        QSizeF display(video_.width() * pixelAspect_, video_.height());
        display.scale(screen.size(), Qt::KeepAspectRatio);
        QRectF target(QPointF(0, 0), display);
        target.moveCenter(QRectF(screen).center());
        p.fillRect(screen, Qt::black);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        p.drawImage(target, video_);
    }

    const QImage& canvas = engine_->screen();
    if (canvasDirty_ || scaledCanvas_.size() != screen.size()) {
        scaledCanvas_ = canvas.scaled(screen.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        canvasDirty_ = false;
    }
    p.drawImage(screen.topLeft(), scaledCanvas_);

    // Caption plane inside the canvas, mapped to widget coordinates.
    const QRect area = engine_->captionAreaInFrame();
    const double fx = static_cast<double>(screen.width()) / canvas.width();
    const double fy = static_cast<double>(screen.height()) / canvas.height();
    const QRectF plane(screen.x() + area.x() * fx, screen.y() + area.y() * fy, area.width() * fx, area.height() * fy);
    const double px = plane.width() / engine_->planeSize().width();
    const double py = plane.height() / engine_->planeSize().height();
    auto mapPlane = [&](const QRect& r) {
        return QRectF(plane.x() + r.x() * px, plane.y() + r.y() * py, r.width() * px, r.height() * py);
    };

    QFont small = font();
    small.setPointSizeF(qMax(7.0, small.pointSizeF() * 0.85));
    p.setFont(small);

    if (overlays_) {
        p.setPen(QPen(QColor(90, 90, 90), 1, Qt::DotLine));
        p.drawRect(plane);
        p.drawText(plane.adjusted(4, 2, 0, 0), Qt::AlignLeft | Qt::AlignTop,
                   QString("plano %1×%2").arg(engine_->planeSize().width()).arg(engine_->planeSize().height()));

        if (!displayArea_.isNull()) {
            p.setPen(QPen(QColor(230, 190, 40), 1, Qt::DashLine));
            const QRectF da = mapPlane(displayArea_);
            p.drawRect(da);
            p.drawText(da.adjusted(2, -16, 0, 0), Qt::AlignLeft | Qt::AlignTop,
                       QString("área SDF/SDP %1,%2 %3×%4")
                           .arg(displayArea_.x())
                           .arg(displayArea_.y())
                           .arg(displayArea_.width())
                           .arg(displayArea_.height()));
        }

        p.setPen(QPen(QColor(40, 200, 230), 1));
        for (const auto& region : engine_->regions())
            p.drawRect(mapPlane(region.rect));
    }

    if (!signalText_.isEmpty()) {
        const double badgeWidth = 16 + p.fontMetrics().horizontalAdvance(signalText_) + 10;
        const QRectF badge(screen.right() - badgeWidth - 10, screen.y() + 10, badgeWidth, 22);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 170));
        p.drawRoundedRect(badge, 4, 4);
        p.setBrush(signalColor_);
        p.drawEllipse(QPointF(badge.x() + 11, badge.center().y()), 5, 5);
        p.setPen(Qt::white);
        p.drawText(badge.adjusted(20, 0, 0, 0), Qt::AlignVCenter | Qt::AlignLeft, signalText_);
    }
}
