// Exercise the same widget, compositor and scene graph APIs used by the CAD view.
#include <QApplication>
#include <QOpenGLWidget>
#include <QSurfaceFormat>
#include <QTimer>
#include <QtPlugin>
#include <cstdio>

Q_IMPORT_PLUGIN(QWaylandIntegrationPlugin)
Q_IMPORT_PLUGIN(QWaylandXdgShellIntegrationPlugin)
Q_IMPORT_PLUGIN(QWaylandEglClientBufferPlugin)

bool test_scene();

class View : public QOpenGLWidget
{
public:
    bool painted = false;
    bool correct = false;
protected:
    void paintGL() override
    {
        correct = test_scene();
        painted = true;
    }
};

int main(int argc, char **argv)
{
    Q_INIT_RESOURCE(gui_shaders);
    qputenv("QT_QPA_PLATFORM", "wayland");
    qputenv("QT_WAYLAND_CLIENT_BUFFER_INTEGRATION", "wayland-egl");
    qputenv("LIBGL_ALWAYS_SOFTWARE", "1");
    qputenv("QT_QPA_FONTDIR", "/lib/freecad-qt-test/fonts");
    QSurfaceFormat fmt;
    fmt.setRenderableType(QSurfaceFormat::OpenGL);
    fmt.setVersion(2, 1);
    fmt.setDepthBufferSize(24);
    QSurfaceFormat::setDefaultFormat(fmt);
    QApplication app(argc, argv);
    View view;
    view.resize(32, 32);
    bool checked = false;
    QObject::connect(&view, &QOpenGLWidget::frameSwapped, &view, [&]() {
        if (checked || !view.painted)
            return;
        checked = true;
        if (!view.correct) {
            std::fputs("Incorrect Coin viewport pixels\n", stderr);
            app.exit(1);
            return;
        }
        // The serial test captures the result while the displayed frame is live.
        std::puts("Wayland FreeCAD 3D viewport passed");
        std::fflush(stdout);
    });
    QTimer::singleShot(15000, &app, [&]() {
        if (!checked)
            app.exit(1);
    });
    view.show();
    return app.exec();
}
