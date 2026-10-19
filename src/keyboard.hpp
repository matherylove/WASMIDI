#pragma once
#include <QPointer>
#include <QQuickFramebufferObject>
#include <QQmlEngine>
#include <QTimer>

class Keyboard : public QQuickFramebufferObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(KeyboardSurface)
    Q_PROPERTY(QObject* controller READ controller WRITE setController NOTIFY controllerChanged)
    Q_PROPERTY(bool portrait READ portrait WRITE setPortrait NOTIFY portraitChanged)
public:
    explicit Keyboard(QQuickItem* parent = nullptr);
    QObject* controller() const { return controller_.data(); }
    void setController(QObject* controller);
    bool portrait() const { return portrait_; }
    void setPortrait(bool portrait);
    Renderer* createRenderer() const override;
protected:
    void geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) override;
signals:
    void controllerChanged();
    void portraitChanged();
private:
    QPointer<QObject> controller_;
    bool portrait_ = false;
    QTimer resizeSettleTimer_;
};
