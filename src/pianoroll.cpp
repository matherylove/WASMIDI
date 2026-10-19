#include "pianoroll.hpp"

#include "mainwindow.hpp"
#include "renderer/gl_renderer.hpp"

#include <QOpenGLFramebufferObject>
#include <QQuickOpenGLUtils>
#include <QQuickWindow>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <limits>
#include <memory>

namespace {

class PianoRollRenderer final
    : public QQuickFramebufferObject::Renderer {
public:
    PianoRollRenderer(
        qreal dpr,
        std::atomic<int>* fps)
        : dpr_(
            std::max<qreal>(
                1.0,
                dpr)),
          fps_(fps)
    {
    }

    QOpenGLFramebufferObject*
    createFramebufferObject(
        const QSize& size) override
    {
        const QSize cssSize(
            std::max(
                64,
                qRound(
                    size.width() /
                    dpr_)),
            std::max(
                64,
                qRound(
                    size.height() /
                    dpr_)));

        // SharpMIDI's note renderer uses depth (note duration is written to Z)
        // to resolve overlaps. WebGL2 supports a depth attachment, so the Qt
        // FBO must expose one instead of the old color-only page renderer FBO.
        QOpenGLFramebufferObjectFormat format;
        format.setAttachment(QOpenGLFramebufferObject::Depth);
        return new QOpenGLFramebufferObject(cssSize, format);
    }

    void synchronize(
        QQuickFramebufferObject* item) override
    {
        auto* roll =
            static_cast<PianoRoll*>(
                item);

        auto* controller =
            qobject_cast<MainWindow*>(
                roll->controller());

        if (!controller) {
            controller_.clear();
            return;
        }

        controller_ = controller;

        renderer_.setNoteSpeed(
            controller->noteSpeed());

        renderer_.setPostBuffer(
            controller->postBuffer());

        renderer_.setPerTrackColors(
            controller->
                perTrackColors());

        renderer_.setNeuralVisual(
            controller->dominantHue(),
            controller->neuralActivity());

        renderer_.setBackgroundAlpha(
            1.0f -
            controller->neuralRoll() *
            0.75f);

        const auto& colors =
            controller->channelColors();

        for (int i = 0;
             i < colors.size() &&
             i < 16;
             ++i) {
            renderer_.setChannelColor(
                static_cast<uint8_t>(i),
                static_cast<uint8_t>(
                    colors[i].red()),
                static_cast<uint8_t>(
                    colors[i].green()),
                static_cast<uint8_t>(
                    colors[i].blue()));
        }

        syncedTimeSeconds_ =
            controller->currentTime();

        renderer_.setTransportRevision(
            static_cast<uint64_t>(controller->transportRevision()));

        if (revision_ !=
            controller->
                documentRevision()) {
            revision_ =
                controller->
                    documentRevision();

            renderer_.setDocument(
                &controller->
                    document());
        }
    }

    void render() override
    {
        if (auto* fbo =
                framebufferObject()) {
            renderer_.resize(
                fbo->size().width(),
                fbo->size().height());
        }

        // MainWindow owns the authoritative visual clock. Never extrapolate
        // independently here: keyboard state and roll geometry must always
        // represent the exact same timeline sample.
        renderer_.setCurrentTime(
            syncedTimeSeconds_);

        const bool transportFrameReady =
            renderer_.renderRoll();

        QQuickOpenGLUtils::
            resetOpenGLState();

        // Playback is frame-paced by a *complete* horizontal frame. A mapped
        // page can take much longer than one GUI frame on a Black MIDI. Never
        // acknowledge a background-only/stale recovery frame, otherwise the
        // master clock runs through the missing page and the roll jumps forward
        // as soon as the cache finally catches up.
        // A held frame (renderRoll() false) is not counted as a new one.
        if (transportFrameReady) {
            if (controller_)
                QMetaObject::invokeMethod(
                    controller_.data(),
                    "notifyVisualizerFramePresented",
                    Qt::QueuedConnection);
            updateFps();
        }

        // GPU neural background animates even while paused, replacing the old
        // QML FrameAnimation/Canvas loop with this single scene-graph source.
        update();
    }

private:
    using Clock =
        std::chrono::steady_clock;

    void updateFps()
    {
        ++fpsFrames_;

        const auto now =
            Clock::now();

        const std::chrono::duration<double>
            elapsed =
                now -
                fpsWindowStart_;

        if (elapsed.count() >= .5) {
            if (fps_) {
                fps_->store(
                    static_cast<int>(
                        std::lround(
                            double(
                                fpsFrames_) /
                            elapsed.count())),
                    std::memory_order_relaxed);
            }

            fpsFrames_ = 0;
            fpsWindowStart_ = now;
        }
    }

    qreal dpr_ = 1.0;
    std::atomic<int>* fps_ = nullptr;
    QPointer<MainWindow> controller_;

    wasmidi::GLRenderer renderer_;

    quint64 revision_ =
        std::numeric_limits<
            quint64>::max();

    float syncedTimeSeconds_ = 0.0f;

    Clock::time_point fpsWindowStart_ =
        Clock::now();

    int fpsFrames_ = 0;
};

} // namespace

PianoRoll::PianoRoll(
    QQuickItem* parent)
    : QQuickFramebufferObject(
        parent)
{
    setMirrorVertically(false);
    setTextureFollowsItemSize(true);

    resizeSettleTimer_.setSingleShot(true);
    resizeSettleTimer_.setInterval(140);

    connect(
        &resizeSettleTimer_,
        &QTimer::timeout,
        this,
        [this]() {
            // Recreate exactly once at the settled dimensions. During a live
            // browser resize Qt stretches the previous FBO instead of allocating
            // and destroying a new WebGL texture for every geometry event.
            setTextureFollowsItemSize(true);
            update();
        });
}

void PianoRoll::geometryChange(
    const QRectF& newGeometry,
    const QRectF& oldGeometry)
{
    QQuickFramebufferObject::geometryChange(
        newGeometry,
        oldGeometry);

    if (newGeometry.size() == oldGeometry.size())
        return;

    setTextureFollowsItemSize(false);
    resizeSettleTimer_.start();
    update();
}

void PianoRoll::setController(
    QObject* controller)
{
    if (controller_ == controller)
        return;

    if (controller_) {
        QObject::disconnect(
            controller_.data(),
            nullptr,
            this,
            nullptr);
    }

    controller_ = controller;

    if (auto* player =
            qobject_cast<MainWindow*>(
                controller_.data())) {
        auto request =
            [this]() {
                update();
            };

        connect(
            player,
            &MainWindow::
                documentRevisionChanged,
            this,
            request);

        connect(
            player,
            &MainWindow::
                noteSpeedChanged,
            this,
            request);

        connect(
            player,
            &MainWindow::
                postBufferChanged,
            this,
            request);

        connect(
            player,
            &MainWindow::
                perTrackColorsChanged,
            this,
            request);

        connect(
            player,
            &MainWindow::
                channelColorsChanged,
            this,
            request);

        connect(
            player,
            &MainWindow::
                playingChanged,
            this,
            request);

        // Synchronize every authoritative visual-clock update. Renderer::update
        // still drives the animated background between clock samples.
        connect(
            player,
            &MainWindow::
                currentTimeChanged,
            this,
            request);
    }

    emit controllerChanged();
    update();
}

QQuickFramebufferObject::Renderer*
PianoRoll::createRenderer() const
{
    const qreal dpr =
        window()
            ? window()->
                devicePixelRatio()
            : 1.0;

    return new PianoRollRenderer(
        dpr,
        &renderFps_);
}
