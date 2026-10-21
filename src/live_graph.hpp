#pragma once

#include <QColor>
#include <QQuickItem>
#include <QQmlEngine>

#include <deque>

// NPS (filled, accent gradient) and polyphony (dashed) over the last seconds of
// song time, sampled once per displayed frame and drawn by the scene graph.
class LiveGraph : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(double time READ time WRITE setTime NOTIFY valuesChanged)
    Q_PROPERTY(double nps READ nps WRITE setNps NOTIFY valuesChanged)
    Q_PROPERTY(double polyphony READ polyphony WRITE setPolyphony NOTIFY valuesChanged)
    Q_PROPERTY(bool sampling READ sampling WRITE setSampling NOTIFY valuesChanged)
    Q_PROPERTY(double span READ span WRITE setSpan NOTIFY valuesChanged)
    Q_PROPERTY(double npsCeiling READ npsCeiling NOTIFY ceilingChanged)
    Q_PROPERTY(double polyCeiling READ polyCeiling NOTIFY ceilingChanged)
    Q_PROPERTY(QColor accent READ accent WRITE setAccent NOTIFY styleChanged)
    Q_PROPERTY(QColor lineColor READ lineColor WRITE setLineColor NOTIFY styleChanged)
    Q_PROPERTY(QColor polyColor READ polyColor WRITE setPolyColor NOTIFY styleChanged)
    Q_PROPERTY(QColor gridColor READ gridColor WRITE setGridColor NOTIFY styleChanged)

public:
    explicit LiveGraph(QQuickItem* parent = nullptr);

    double time() const { return time_; }
    double nps() const { return nps_; }
    double polyphony() const { return polyphony_; }
    bool sampling() const { return sampling_; }
    double span() const { return span_; }
    QColor accent() const { return accent_; }
    QColor lineColor() const { return lineColor_; }
    QColor polyColor() const { return polyColor_; }
    QColor gridColor() const { return gridColor_; }

    void setTime(double value) { time_ = value; }
    void setNps(double value) { nps_ = value; }
    void setPolyphony(double value) { polyphony_ = value; }
    void setSampling(bool value);
    void setSpan(double value);
    void setAccent(const QColor& value);
    void setLineColor(const QColor& value);
    void setPolyColor(const QColor& value);
    void setGridColor(const QColor& value);

    double npsCeiling() const { return npsCeiling_; }
    double polyCeiling() const { return polyCeiling_; }

    Q_INVOKABLE void clear();
    // Samples drawn since the last call (the graph's own frame count).
    Q_INVOKABLE int takeFrames();

signals:
    void valuesChanged();
    void styleChanged();
    void ceilingChanged();

protected:
    QSGNode* updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData* data) override;
    void itemChange(ItemChange change, const ItemChangeData& data) override;

private:
    struct Sample {
        double time;
        float nps;
        float polyphony;
    };

    void sample();

    std::deque<Sample> samples_;
    float npsCeiling_ = 1.0f;
    float polyCeiling_ = 1.0f;
    int frames_ = 0;
    double time_ = 0.0;
    double nps_ = 0.0;
    double polyphony_ = 0.0;
    bool sampling_ = false;
    double span_ = 10.0;
    QColor accent_ = QColor(0x6b, 0x55, 0xd4);
    QColor lineColor_ = QColor(0xa6, 0x95, 0xe1);
    QColor polyColor_ = QColor(0xf0, 0xab, 0xfc);
    QColor gridColor_ = QColor(0x1d, 0x19, 0x40);
    QMetaObject::Connection frameConnection_;
};
