#include "keyboard.hpp"
#include "mainwindow.hpp"

#include <GLES3/gl3.h>

#include <QOpenGLFramebufferObject>
#include <QQuickOpenGLUtils>
#include <QQuickWindow>

#include <algorithm>
#include <array>
#include <cstddef>
#include <vector>

namespace {

struct KeyInstance {
    float x, y, w, h;
    float r, g, b, pressed;
    float kind, separator;
};

// Instance kinds: a key row (white face), a black key's row (white face with
// the short divider) and the black bar drawn over it.
constexpr float KindWhiteRow = 0.0f;
constexpr float KindBlackRow = 1.0f;
constexpr float KindBlackBar = 2.0f;

// Black bars cover the side next to the roll: 37 of the keyboard's 62 px.
constexpr float BlackBarShare = 37.0f / 62.0f;

GLuint compileShader(GLenum type, const char* source)
{
    const GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);

    if (!ok) {
        glDeleteShader(shader);
        return 0;
    }

    return shader;
}

GLuint makeProgram()
{
    static const char* vertex = R"GLSL(#version 300 es
precision highp float;

layout(location=0) in vec2 aCorner;
layout(location=1) in vec4 aRect;
layout(location=2) in vec4 aColor;
layout(location=3) in vec2 aKind;

uniform vec2 uSize;

out vec2 vLocal;
flat out vec2 vSizePx;
flat out vec4 vColor;
flat out vec2 vKind;

void main()
{
    vec2 p =
        aRect.xy +
        aCorner * aRect.zw;

    gl_Position =
        vec4(
            p * 2.0 - 1.0,
            0.0,
            1.0);

    vSizePx = aRect.zw * uSize;
    vLocal = aCorner * vSizePx;
    vColor = aColor;
    vKind = aKind;
}
)GLSL";

    // Same look as the roll's notes: gradients and the 2 px checker texture
    // on pressed keys (HANDOFF sec. 93).
    static const char* fragment = R"GLSL(#version 300 es
precision highp float;

in vec2 vLocal;
flat in vec2 vSizePx;
flat in vec4 vColor;
flat in vec2 vKind;

out vec4 fragColor;

vec3 ramp(vec3 a, vec3 b, vec3 c, float stop, float t)
{
    return t < stop
        ? mix(a, b, t / stop)
        : mix(b, c, (t - stop) / (1.0 - stop));
}

void main()
{
    const vec3 divider = vec3(43.0, 32.0, 90.0) / 255.0;
    float t = vLocal.x / max(1.0, vSizePx.x);
    bool pressed = vColor.a > 0.5;
    vec3 pc = vColor.rgb;
    vec3 color;

    if (vKind.x > 1.5) {
        color = pressed
            ? ramp(pc * 0.5, pc, pc + (1.0 - pc) * 0.3, 0.75, t)
            : ramp(vec3(11.0, 9.0, 34.0) / 255.0,
                   vec3(46.0, 39.0, 104.0) / 255.0,
                   vec3(62.0, 48.0, 128.0) / 255.0, 0.75, t);
        if (!pressed && vLocal.y >= vSizePx.y - 1.0)
            color += (1.0 - color) * 0.18;
    } else {
        bool whitePressed = pressed && vKind.x < 0.5;
        color = whitePressed
            ? ramp(pc * 0.7, pc, pc + (1.0 - pc) * 0.4, 0.6, t)
            : ramp(vec3(169.0, 159.0, 214.0) / 255.0,
                   vec3(239.0, 234.0, 255.0) / 255.0,
                   vec3(216.0, 208.0, 246.0) / 255.0, 0.55, t);
        float mid = floor(vSizePx.y * 0.5);
        if ((vKind.y > 0.5 && vLocal.y >= vSizePx.y - 1.0) ||
            (vKind.x > 0.5 && vLocal.x < 24.0 &&
             vLocal.y >= mid - 1.0 && vLocal.y < mid))
            color = mix(color, divider, 0.75);
        pressed = whitePressed;
    }

    ivec2 p = ivec2(gl_FragCoord.xy);
    if (pressed && ((p.x ^ p.y) & 1) == 1)
        color += (1.0 - color) * 0.13;

    fragColor = vec4(color, 1.0);
}
)GLSL";

    const GLuint vs =
        compileShader(
            GL_VERTEX_SHADER,
            vertex);

    const GLuint fs =
        compileShader(
            GL_FRAGMENT_SHADER,
            fragment);

    if (!vs || !fs)
        return 0;

    const GLuint program =
        glCreateProgram();

    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);

    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint ok = 0;
    glGetProgramiv(
        program,
        GL_LINK_STATUS,
        &ok);

    if (!ok) {
        glDeleteProgram(program);
        return 0;
    }

    return program;
}

class KeyboardRenderer final
    : public QQuickFramebufferObject::Renderer {
public:
    explicit KeyboardRenderer(qreal dpr)
        : dpr_(
            std::max<qreal>(
                1.0,
                dpr))
    {
    }

    ~KeyboardRenderer() override
    {
        if (vbo_)
            glDeleteBuffers(1, &vbo_);

        if (instanceVbo_)
            glDeleteBuffers(
                1,
                &instanceVbo_);

        if (vao_)
            glDeleteVertexArrays(
                1,
                &vao_);

        if (program_)
            glDeleteProgram(program_);
    }

    QOpenGLFramebufferObject*
    createFramebufferObject(
        const QSize& size) override
    {
        const QSize cssSize(
            std::max(
                1,
                qRound(
                    size.width() /
                    dpr_)),
            std::max(
                1,
                qRound(
                    size.height() /
                    dpr_)));

        return new QOpenGLFramebufferObject(
            cssSize);
    }

    void synchronize(
        QQuickFramebufferObject* item) override
    {
        auto* keyboard =
            static_cast<Keyboard*>(item);

        auto* controller =
            qobject_cast<MainWindow*>(
                keyboard->controller());

        if (!controller)
            return;

        const auto nextMask =
            controller->activePitchMask();

        const auto nextColorIndices =
            controller->activePitchColorIndices();

        const auto nextColors =
            controller->channelColors();

        if (nextMask != mask_ ||
            nextColorIndices != colorIndices_ ||
            nextColors != colors_) {
            mask_ = nextMask;
            colorIndices_ = nextColorIndices;
            colors_ = nextColors;
            dirty_ = true;
        }
    }

    void render() override
    {
        if (!program_)
            initialize();

        if (!program_)
            return;

        if (auto* fbo =
                framebufferObject()) {
            width_ =
                std::max(
                    1,
                    fbo->size().width());

            height_ =
                std::max(
                    1,
                    fbo->size().height());
        }

        if (dirty_)
            rebuild();

        glViewport(
            0,
            0,
            width_,
            height_);

        glDisable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);

        glClearColor(
            11.0f / 255.0f,
            9.0f / 255.0f,
            34.0f / 255.0f,
            1.0f);

        glClear(GL_COLOR_BUFFER_BIT);

        glUseProgram(program_);

        glUniform2f(
            sizeUniform_,
            float(width_),
            float(height_));

        glBindVertexArray(vao_);

        glDrawArraysInstanced(
            GL_TRIANGLES,
            0,
            6,
            static_cast<GLsizei>(
                keys_.size()));

        glBindVertexArray(0);

        QQuickOpenGLUtils::
            resetOpenGLState();
    }

private:
    void initialize()
    {
        program_ = makeProgram();

        if (!program_)
            return;

        sizeUniform_ =
            glGetUniformLocation(
                program_,
                "uSize");

        static const float quad[] = {
            0,0, 1,0, 0,1,
            0,1, 1,0, 1,1
        };

        glGenVertexArrays(1, &vao_);
        glBindVertexArray(vao_);

        glGenBuffers(1, &vbo_);
        glBindBuffer(
            GL_ARRAY_BUFFER,
            vbo_);

        glBufferData(
            GL_ARRAY_BUFFER,
            sizeof(quad),
            quad,
            GL_STATIC_DRAW);

        glEnableVertexAttribArray(0);

        glVertexAttribPointer(
            0,
            2,
            GL_FLOAT,
            GL_FALSE,
            0,
            nullptr);

        glGenBuffers(
            1,
            &instanceVbo_);

        glBindBuffer(
            GL_ARRAY_BUFFER,
            instanceVbo_);

        const struct {
            GLuint location;
            GLint size;
            std::size_t offset;
        } attributes[] = {
            {1, 4, offsetof(KeyInstance, x)},
            {2, 4, offsetof(KeyInstance, r)},
            {3, 2, offsetof(KeyInstance, kind)}
        };

        for (const auto& attribute : attributes) {
            glEnableVertexAttribArray(
                attribute.location);

            glVertexAttribPointer(
                attribute.location,
                attribute.size,
                GL_FLOAT,
                GL_FALSE,
                sizeof(KeyInstance),
                reinterpret_cast<void*>(
                    attribute.offset));

            glVertexAttribDivisor(
                attribute.location,
                1);
        }

        glBindVertexArray(0);
    }

    std::array<float, 4>
    activeColor(int note) const
    {
        if (!mask_[note] ||
            colorIndices_[note] < 0 ||
            colorIndices_[note] >=
                colors_.size()) {
            return {0, 0, 0, 0};
        }

        const QColor color =
            colors_[
                colorIndices_[note]];

        return {
            float(color.redF()),
            float(color.greenF()),
            float(color.blueF()),
            1.0f
        };
    }

    void rebuild()
    {
        static const bool black[12] = {
            false,true,false,true,false,false,
            true,false,true,false,true,false
        };

        keys_.clear();
        keys_.reserve(128 + 53);

        // One row per key, low notes at the bottom like the roll's rows.
        const float rowHeight =
            1.0f / 128.0f;

        for (int note = 0;
             note < 128;
             ++note) {
            const bool isBlack =
                black[note % 12];

            const auto color =
                activeColor(note);

            // A divider above E and B, where two white keys meet.
            const float separator =
                (note % 12 == 4 ||
                 note % 12 == 11)
                    ? 1.0f
                    : 0.0f;

            keys_.push_back({
                0.0f,
                note * rowHeight,
                1.0f,
                rowHeight,
                color[0],
                color[1],
                color[2],
                color[3],
                isBlack
                    ? KindBlackRow
                    : KindWhiteRow,
                separator
            });

            if (isBlack) {
                keys_.push_back({
                    1.0f - BlackBarShare,
                    note * rowHeight,
                    BlackBarShare,
                    rowHeight,
                    color[0],
                    color[1],
                    color[2],
                    color[3],
                    KindBlackBar,
                    0.0f
                });
            }
        }

        glBindBuffer(
            GL_ARRAY_BUFFER,
            instanceVbo_);

        glBufferData(
            GL_ARRAY_BUFFER,
            static_cast<GLsizeiptr>(
                keys_.size() *
                sizeof(KeyInstance)),
            keys_.data(),
            GL_DYNAMIC_DRAW);

        dirty_ = false;
    }

    qreal dpr_ = 1.0;

    GLuint program_ = 0;
    GLuint vao_ = 0;
    GLuint vbo_ = 0;
    GLuint instanceVbo_ = 0;

    GLint sizeUniform_ = -1;

    int width_ = 1;
    int height_ = 1;
    bool dirty_ = true;

    std::array<uint8_t, 128> mask_{};
    std::array<int8_t, 128> colorIndices_{};
    QVector<QColor> colors_;
    std::vector<KeyInstance> keys_;
};

} // namespace

Keyboard::Keyboard(QQuickItem* parent)
    : QQuickFramebufferObject(parent)
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
            setTextureFollowsItemSize(true);
            update();
        });
}

void Keyboard::geometryChange(
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

void Keyboard::setController(QObject* controller)
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
            &MainWindow::activePitchesChanged,
            this,
            request);

        connect(
            player,
            &MainWindow::channelColorsChanged,
            this,
            request);

        connect(
            player,
            &MainWindow::documentRevisionChanged,
            this,
            request);
    }

    emit controllerChanged();
    update();
}

QQuickFramebufferObject::Renderer*
Keyboard::createRenderer() const
{
    const qreal dpr =
        window()
            ? window()->devicePixelRatio()
            : 1.0;

    return new KeyboardRenderer(dpr);
}
