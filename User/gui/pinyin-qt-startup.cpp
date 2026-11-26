// Link into a Qt Widgets application to enable pinyin in editable controls.
#include "pinyin-qt.h"
#include <QFontDatabase>
#include <QMainWindow>
#include <QStatusBar>

class PinyinShortcut : public QObject {
    Pinyin input{this};
    bool enabled = false;

public:
    explicit PinyinShortcut(QObject *parent) : QObject(parent) { qApp->installEventFilter(this); }
    bool eventFilter(QObject *, QEvent *event) override {
        if (event->type() != QEvent::KeyPress && event->type() != QEvent::ShortcutOverride)
            return false;
        auto key = static_cast<QKeyEvent *>(event);
        if (key->key() != Qt::Key_F7 || key->modifiers() != Qt::NoModifier)
            return false;
        if (event->type() == QEvent::ShortcutOverride) {
            event->accept();
            return true;
        }
        const bool ok = input.toggle(!enabled);
        if (ok)
            enabled = !enabled;
        if (auto w = qobject_cast<QMainWindow *>(QApplication::activeWindow()))
            w->statusBar()->showMessage(
                ok ? (enabled ? "Chinese pinyin" : "Latin input") : "Cannot open the pinyin dictionary", 2500);
        return true;
    }
};
void lxInstallPinyin(QApplication *app) {
    QFontDatabase::addApplicationFont("/lib/lite-xl/data/fonts/NotoSansMonoCJKsc-Regular.otf");
    new PinyinShortcut(app);
}
