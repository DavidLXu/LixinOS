// Run a widget from FreeCAD's src/Gui using the static guest Qt platform plugin.
#include "SplitButton.h"
#include <QApplication>
#include <QImage>
#include <QtPlugin>
#include <cstdio>

#ifdef TEST_WAYLAND
#include <QEventLoop>
#include <QTimer>
#include <QWindow>
Q_IMPORT_PLUGIN(QWaylandIntegrationPlugin)
Q_IMPORT_PLUGIN(QWaylandXdgShellIntegrationPlugin)
#else
Q_IMPORT_PLUGIN(QOffscreenIntegrationPlugin)
#endif

int main(int argc, char **argv)
{
#ifdef TEST_WAYLAND
    qputenv("QT_QPA_PLATFORM", "wayland");
#else
    qputenv("QT_QPA_PLATFORM", "offscreen");
#endif
    qputenv("QT_QPA_FONTDIR", "/lib/freecad-qt-test/fonts");
    QApplication app(argc, argv);
    Gui::SplitButton button(QStringLiteral("Part"));
    button.resize(160, 60);
    auto *main = button.mainButton();
#ifdef TEST_WAYLAND
    button.show();
    QEventLoop exposed;
    QTimer check;
    QObject::connect(&check, &QTimer::timeout, [&]() {
        if (button.windowHandle()->isExposed())
            exposed.quit();
    });
    check.start(25);
    QTimer::singleShot(10000, &exposed, &QEventLoop::quit);
    exposed.exec();
    if (!button.windowHandle()->isExposed()) {
        std::fputs("The Wayland compositor did not expose the window\n", stderr);
        return 1;
    }
#endif
    main->setCheckable(true);
    main->setStyleSheet("QPushButton { background: #224466; } "
                        "QPushButton:checked { background: #cc6633; }");
    int clicks = 0;
    QObject::connect(&button, &Gui::SplitButton::defaultClicked, [&]() { ++clicks; });
    QImage before(button.size(), QImage::Format_RGB32);
    QImage after(button.size(), QImage::Format_RGB32);
    before.fill(Qt::black);
    after.fill(Qt::black);
    button.render(&before);
    main->click();
    button.render(&after);
    if (!main->isChecked() || clicks != 1 || before == after) {
        std::fputs("FreeCAD button state, signal or repaint failed\n", stderr);
        return 1;
    }
#ifdef TEST_WAYLAND
    button.repaint();
    app.processEvents();
    std::puts("Wayland FreeCAD widget exposed");
#endif
    std::printf("Qt %s upstream FreeCAD SplitButton click and repaint passed\n", qVersion());
}
