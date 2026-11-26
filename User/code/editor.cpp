#include "page.h"
#include "workspace.h"
Q_IMPORT_PLUGIN(QWaylandIntegrationPlugin)
Q_IMPORT_PLUGIN(QWaylandXdgShellIntegrationPlugin)

int main(int argc, char **argv) {
    qputenv("QT_QPA_PLATFORM", "wayland");
    if (qEnvironmentVariableIsEmpty("QT_QPA_FONTDIR"))
        qputenv("QT_QPA_FONTDIR", QFileInfo::exists("/usr/share/fonts/JetBrainsMono-Regular.ttf")
                                      ? "/usr/share/fonts" : "/lib/code/fonts");
    QApplication app(argc, argv);
    const QString cjkFont = "/lib/lite-xl/data/fonts/NotoSansMonoCJKsc-Regular.otf";
    if (!QFileInfo::exists("/usr/share/fonts/NotoSansMonoCJKsc-Regular.otf") && QFileInfo::exists(cjkFont))
        QFontDatabase::addApplicationFont(cjkFont);
    Editor window;
    if (!window.start(argc > 1 ? QString::fromLocal8Bit(argv[1]) : QString())) {
        std::fputs("code: could not open the editor or document\n", stderr);
        return 1;
    }
    window.show();
    const int status = app.exec();
    // The window has accepted close and pending file commits are complete.
    // QtWayland teardown can block joining its display thread on the TCC kernel.
    // Closing the process releases the display socket and wakes the compositor.
    std::fflush(nullptr);
    std::_Exit(status);
}
