#include "live_graph.hpp"

#include <QPointF>
#include <QQuickWindow>
#include <QSGGeometryNode>
#include <QSGVertexColorMaterial>

#include <algorithm>
#include <cmath>
#include <vector>

namespace {

QSGGeometryNode* makeNode(QSGGeometry::DrawingMode mode)
{
    auto* geometry =
        new QSGGeometry(
            QSGGeometry::defaultAttributes_ColoredPoint2D(),
            0);
    geometry->setDrawingMode(mode);
    geometry->setLineWidth(1.0f);
    geometry->setVertexDataPattern(QSGGeometry::StreamPattern);

    auto* node = new QSGGeometryNode;
    node->setGeometry(geometry);
    node->setFlag(QSGNode::OwnsGeometry);
    node->setMaterial(new QSGVertexColorMaterial);
    node->setFlag(QSGNode::OwnsMaterial);
    return node;
}

void setPremultiplied(QSGGeometry::ColoredPoint2D* v, float x, float y, const QColor& c, float alpha)
{
    const float a = std::clamp(float(c.alphaF()) * alpha, 0.0f, 1.0f);
    v->set(x, y,
           uchar(std::lround(c.red() * a)),
           uchar(std::lround(c.green() * a)),
           uchar(std::lround(c.blue() * a)),
           uchar(std::lround(255.0f * a)));
}

} // namespace

LiveGraph::LiveGraph(QQuickItem* parent)
    : QQuickItem(parent)
{
    setFlag(ItemHasContents, true);
}

void LiveGraph::setSampling(bool value)
{
    if (sampling_ == value)
        return;
    sampling_ = value;
    emit valuesChanged();
}

void LiveGraph::setSpan(double value)
{
    value = std::clamp(value, 0.1, 120.0);
    if (span_ == value)
        return;
    span_ = value;
    emit valuesChanged();
    update();
}

void LiveGraph::setAccent(const QColor& value)
{
    if (accent_ == value)
        return;
    accent_ = value;
    emit styleChanged();
    update();
}

void LiveGraph::setLineColor(const QColor& value)
{
    if (lineColor_ == value)
        return;
    lineColor_ = value;
    emit styleChanged();
    update();
}

void LiveGraph::setPolyColor(const QColor& value)
{
    if (polyColor_ == value)
        return;
    polyColor_ = value;
    emit styleChanged();
    update();
}

void LiveGraph::setGridColor(const QColor& value)
{
    if (gridColor_ == value)
        return;
    gridColor_ = value;
    emit styleChanged();
    update();
}

void LiveGraph::clear()
{
    samples_.clear();
    update();
}

int LiveGraph::takeFrames()
{
    const int frames = frames_;
    frames_ = 0;
    return frames;
}

void LiveGraph::itemChange(ItemChange change, const ItemChangeData& data)
{
    QQuickItem::itemChange(change, data);
    if (change != ItemSceneChange)
        return;
    QObject::disconnect(frameConnection_);
    if (data.window)
        frameConnection_ =
            connect(data.window, &QQuickWindow::afterAnimating, this, &LiveGraph::sample);
}

void LiveGraph::sample()
{
    if (!sampling_ || !isVisible())
        return;

    if (!samples_.empty()) {
        const double last = samples_.back().time;
        // A seek starts a new history.
        if (time_ < last - 0.05 || time_ > last + 1.0)
            samples_.clear();
        else if (time_ - last < span_ / 2048.0)
            return;
    }

    samples_.push_back({time_, float(nps_), float(polyphony_)});
    ++frames_;
    while (!samples_.empty() && samples_.front().time < time_ - span_ - 0.5)
        samples_.pop_front();
    update();
}

QSGNode* LiveGraph::updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*)
{
    QSGNode* root = oldNode;
    if (!root) {
        root = new QSGNode;
        root->appendChildNode(makeNode(QSGGeometry::DrawLines));         // grid
        root->appendChildNode(makeNode(QSGGeometry::DrawTriangleStrip)); // NPS area
        root->appendChildNode(makeNode(QSGGeometry::DrawTriangleStrip)); // NPS line
        root->appendChildNode(makeNode(QSGGeometry::DrawLines));         // polyphony
    }

    auto* gridNode = static_cast<QSGGeometryNode*>(root->childAtIndex(0));
    auto* areaNode = static_cast<QSGGeometryNode*>(root->childAtIndex(1));
    auto* lineNode = static_cast<QSGGeometryNode*>(root->childAtIndex(2));
    auto* polyNode = static_cast<QSGGeometryNode*>(root->childAtIndex(3));

    const float w = float(width());
    const float h = float(height());

    QSGGeometry* grid = gridNode->geometry();
    grid->allocate(6);
    auto* gv = grid->vertexDataAsColoredPoint2D();
    for (int i = 1; i <= 3; ++i) {
        const float y = std::floor(h * float(i) / 4.0f) + 0.5f;
        setPremultiplied(gv++, 0.0f, y, gridColor_, 1.0f);
        setPremultiplied(gv++, w, y, gridColor_, 1.0f);
    }
    gridNode->markDirty(QSGNode::DirtyGeometry);

    float maxNps = 1.0f, maxPoly = 1.0f;
    const double since = samples_.empty() ? 0.0 : samples_.back().time - span_;
    for (const auto& s : samples_) {
        if (s.time < since)
            continue;
        maxNps = std::max(maxNps, s.nps);
        maxPoly = std::max(maxPoly, s.polyphony);
    }

    const int count = int(samples_.size());
    const double now = samples_.empty() ? 0.0 : samples_.back().time;
    const float top = 3.0f, bottom = h - 1.0f;
    const auto xOf = [&](const Sample& s) {
        return std::clamp(float(w * (1.0 - (now - s.time) / span_)), 0.0f, w);
    };
    const auto yOf = [&](float value, float maxValue) {
        return bottom - (value / maxValue) * (bottom - top);
    };

    QSGGeometry* area = areaNode->geometry();
    QSGGeometry* line = lineNode->geometry();
    area->allocate(count * 2);
    line->allocate(count * 2);
    auto* av = area->vertexDataAsColoredPoint2D();
    auto* lv = line->vertexDataAsColoredPoint2D();
    for (const auto& s : samples_) {
        const float x = xOf(s);
        const float y = yOf(s.nps, maxNps);
        // Vertical gradient of the fill: .75 at the top of the chart, .05 at the bottom.
        setPremultiplied(av++, x, y, accent_, 0.05f + 0.70f * (1.0f - y / std::max(1.0f, h)));
        setPremultiplied(av++, x, h, accent_, 0.05f);
        setPremultiplied(lv++, x, y - 0.75f, lineColor_, 1.0f);
        setPremultiplied(lv++, x, y + 0.75f, lineColor_, 1.0f);
    }
    areaNode->markDirty(QSGNode::DirtyGeometry);
    lineNode->markDirty(QSGNode::DirtyGeometry);

    // Dashed polyphony line: 3 px on, 2 px off along x.
    std::vector<QPointF> dashes;
    for (int i = 1; i < count; ++i) {
        const float xa = xOf(samples_[i - 1]), xb = xOf(samples_[i]);
        const float ya = yOf(samples_[i - 1].polyphony, maxPoly);
        const float yb = yOf(samples_[i].polyphony, maxPoly);
        if (xb <= xa)
            continue;
        for (float d = std::floor(xa / 5.0f) * 5.0f; d < xb; d += 5.0f) {
            const float x0 = std::max(xa, d), x1 = std::min(xb, d + 3.0f);
            if (x1 <= x0)
                continue;
            dashes.emplace_back(x0, ya + (yb - ya) * (x0 - xa) / (xb - xa));
            dashes.emplace_back(x1, ya + (yb - ya) * (x1 - xa) / (xb - xa));
        }
    }
    QSGGeometry* poly = polyNode->geometry();
    poly->allocate(int(dashes.size()));
    auto* pv = poly->vertexDataAsColoredPoint2D();
    for (const auto& point : dashes)
        setPremultiplied(pv++, float(point.x()), float(point.y()), polyColor_, 1.0f);
    polyNode->markDirty(QSGNode::DirtyGeometry);

    return root;
}
