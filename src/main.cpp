#include <QFontDatabase>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQuickWindow>
#include <QSGRendererInterface>

int main(int argc, char *argv[])
{
    // QQuickFramebufferObject is OpenGL-only in Qt 6. Force the scene graph
    // backend before constructing any QQuickWindow so WebAssembly maps this
    // path to WebGL/OpenGL ES consistently.
    QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);

    QGuiApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("WebAssembly MIDI Player"));
    app.setOrganizationName(QStringLiteral("Dekxtopia"));
    QFontDatabase::addApplicationFont(
        QStringLiteral(":/qt/qml/Wasmidi/assets/fonts/VT323-Regular.ttf"));

    QQmlApplicationEngine engine;
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed,
                     &app, [] { QCoreApplication::exit(-1); },
                     Qt::QueuedConnection);
    engine.loadFromModule(QStringLiteral("Wasmidi"), QStringLiteral("MainWindow"));

    return app.exec();
}
