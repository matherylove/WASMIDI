#pragma once

#include <QElapsedTimer>
#include <QQuickItem>
#include <QQmlEngine>

#include <cstdint>
#include <vector>

// The dekx.cc neural field behind the whole page: drifting nodes, 2 px dots and
// lines between nodes closer than 140 px, drawn by the scene graph.
class NeuralBackground : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(bool running READ running WRITE setRunning NOTIFY runningChanged)

public:
    explicit NeuralBackground(QQuickItem* parent = nullptr);

    bool running() const { return running_; }
    void setRunning(bool running);

signals:
    void runningChanged();

protected:
    QSGNode* updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData* data) override;
    void geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) override;

private:
    struct Node {
        float x = 0.0f;
        float y = 0.0f;
        float vx = 0.0f;
        float vy = 0.0f;
    };

    void reseed();
    float random01();
    void advance();

    std::vector<Node> nodes_;
    uint32_t rng_ = 0x2545f491u;
    QElapsedTimer clock_;
    qint64 lastNs_ = -1;
    bool running_ = true;
};
