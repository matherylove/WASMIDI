#pragma once

#include <QPointer>
#include <QQuickFramebufferObject>
#include <QQmlEngine>
#include <QTimer>

#include <atomic>

class PianoRoll : public QQuickFramebufferObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(PianoRollSurface)
    Q_PROPERTY(QObject* controller READ controller WRITE setController NOTIFY controllerChanged)
    Q_PROPERTY(int renderFps READ renderFps)
    Q_PROPERTY(bool portrait READ portrait WRITE setPortrait NOTIFY portraitChanged)

public:
    explicit PianoRoll(QQuickItem* parent = nullptr);

    QObject* controller() const { return controller_.data(); }
    void setController(QObject* controller);

    bool portrait() const { return portrait_; }
    void setPortrait(bool portrait);

    int renderFps() const {
        return renderFps_.load(std::memory_order_relaxed);
    }

    Renderer* createRenderer() const override;

protected:
    void geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) override;

signals:
    void controllerChanged();
    void portraitChanged();

private:
    QPointer<QObject> controller_;
    bool portrait_ = false;
    mutable std::atomic<int> renderFps_{0};
    QTimer resizeSettleTimer_;
};
