#include "neural_background.hpp"

#include <QSGGeometryNode>
#include <QSGVertexColorMaterial>

#include <algorithm>
#include <cmath>

namespace {

constexpr float LinkDistance = 140.0f;
constexpr float Margin = 10.0f;

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

} // namespace

NeuralBackground::NeuralBackground(QQuickItem* parent)
    : QQuickItem(parent)
{
    setFlag(ItemHasContents, true);
    clock_.start();
}

void NeuralBackground::setRunning(bool running)
{
    if (running_ == running)
        return;
    running_ = running;
    lastNs_ = -1;
    emit runningChanged();
    update();
}

float NeuralBackground::random01()
{
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 17;
    rng_ ^= rng_ << 5;
    return float(rng_ & 0x00ffffffu) / float(0x01000000u);
}

void NeuralBackground::reseed()
{
    const float w = float(width());
    const float h = float(height());
    const int count = int(std::lround(std::min(80.0f, w * h / 18000.0f)));

    nodes_.resize(std::max(0, count));
    for (auto& node : nodes_) {
        node.x = random01() * w;
        node.y = random01() * h;
        node.vx = (random01() - 0.5f) * 0.16f;
        node.vy = (random01() - 0.5f) * 0.16f;
    }
}

void NeuralBackground::geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry)
{
    QQuickItem::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.size() != oldGeometry.size()) {
        reseed();
        update();
    }
}

void NeuralBackground::advance()
{
    const qint64 now = clock_.nsecsElapsed();
    // Velocities are per 60 Hz frame, as on dekx.cc, whatever the display rate.
    const float frames =
        lastNs_ < 0 ? 0.0f : std::clamp(float(now - lastNs_) * 60e-9f, 0.0f, 4.0f);
    lastNs_ = now;

    const float w = float(width());
    const float h = float(height());
    for (auto& node : nodes_) {
        node.x += node.vx * frames;
        node.y += node.vy * frames;
        if (node.x < -Margin) node.x = w + Margin;
        if (node.x > w + Margin) node.x = -Margin;
        if (node.y < -Margin) node.y = h + Margin;
        if (node.y > h + Margin) node.y = -Margin;
    }
}

QSGNode* NeuralBackground::updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*)
{
    QSGNode* root = oldNode;
    if (!root) {
        root = new QSGNode;
        root->appendChildNode(makeNode(QSGGeometry::DrawLines));
        root->appendChildNode(makeNode(QSGGeometry::DrawTriangles));
    }

    if (running_)
        advance();

    auto* lineNode = static_cast<QSGGeometryNode*>(root->firstChild());
    auto* dotNode = static_cast<QSGGeometryNode*>(root->lastChild());

    // Lines: rgba(167,139,250,(1 - d/140) * .2), premultiplied for the material.
    int lineCount = 0;
    const std::size_t n = nodes_.size();
    for (std::size_t i = 0; i < n; ++i)
        for (std::size_t j = i + 1; j < n; ++j) {
            const float dx = nodes_[i].x - nodes_[j].x;
            const float dy = nodes_[i].y - nodes_[j].y;
            if (dx * dx + dy * dy < LinkDistance * LinkDistance)
                ++lineCount;
        }

    QSGGeometry* lines = lineNode->geometry();
    lines->allocate(lineCount * 2);
    auto* lv = lines->vertexDataAsColoredPoint2D();
    for (std::size_t i = 0; i < n; ++i)
        for (std::size_t j = i + 1; j < n; ++j) {
            const float dx = nodes_[i].x - nodes_[j].x;
            const float dy = nodes_[i].y - nodes_[j].y;
            const float d2 = dx * dx + dy * dy;
            if (d2 >= LinkDistance * LinkDistance)
                continue;
            const float a = (1.0f - std::sqrt(d2) / LinkDistance) * 0.2f;
            const auto r = uchar(std::lround(167.0f * a));
            const auto g = uchar(std::lround(139.0f * a));
            const auto b = uchar(std::lround(250.0f * a));
            const auto alpha = uchar(std::lround(255.0f * a));
            (lv++)->set(nodes_[i].x, nodes_[i].y, r, g, b, alpha);
            (lv++)->set(nodes_[j].x, nodes_[j].y, r, g, b, alpha);
        }
    lineNode->markDirty(QSGNode::DirtyGeometry);

    // Dots: 2 px squares of rgba(196,181,253,.75) on the pixel grid.
    QSGGeometry* dots = dotNode->geometry();
    dots->allocate(int(n) * 6);
    auto* dv = dots->vertexDataAsColoredPoint2D();
    const uchar dr = uchar(std::lround(196 * 0.75)), dg = uchar(std::lround(181 * 0.75)),
                db = uchar(std::lround(253 * 0.75)), da = uchar(std::lround(255 * 0.75));
    for (const auto& node : nodes_) {
        const float x0 = std::round(node.x) - 1.0f, y0 = std::round(node.y) - 1.0f;
        const float x1 = x0 + 2.0f, y1 = y0 + 2.0f;
        (dv++)->set(x0, y0, dr, dg, db, da);
        (dv++)->set(x1, y0, dr, dg, db, da);
        (dv++)->set(x0, y1, dr, dg, db, da);
        (dv++)->set(x1, y0, dr, dg, db, da);
        (dv++)->set(x1, y1, dr, dg, db, da);
        (dv++)->set(x0, y1, dr, dg, db, da);
    }
    dotNode->markDirty(QSGNode::DirtyGeometry);

    if (running_)
        update();

    return root;
}
