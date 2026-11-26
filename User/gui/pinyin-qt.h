// Application-wide input method for Qt Widgets, including text fields in dialogs.
#pragma once
#include <QApplication>
#include <QFile>
#include <QHash>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QPointer>
#include <QLabel>
#include <QWidget>

class Pinyin : public QObject {
    QHash<QByteArray, QByteArray> entries;
    QString spelling;
    QStringList choices;
    QPointer<QWidget> target;
    QPointer<QLabel> candidates;
    int page = 0;
    bool enabled = false;
    void update() {
        if (!target)
            return;
        QInputMethodEvent event(spelling, {{QInputMethodEvent::Cursor, int(spelling.size()), 1, {}}});
        QApplication::sendEvent(target, &event);
        if (spelling.isEmpty()) {
            if (candidates)
                candidates->hide();
            return;
        }
        QStringList labels;
        for (int i = page * 9; i < qMin(qsizetype(page * 9 + 9), choices.size()); ++i)
            labels << QString("%1 %2").arg(i % 9 + 1).arg(choices[i]);
        QString hint = choices.isEmpty() ? "Enter: " + spelling : labels.join("   ");
        if (choices.size() > 9)
            hint += QString("   [%1/%2]  −/+").arg(page + 1).arg((choices.size() + 8) / 9);
        QRect rect = target->inputMethodQuery(Qt::ImCursorRectangle).toRect();
        auto window = target->window();
        if (!candidates || candidates->parentWidget() != window) {
            delete candidates.data();
            candidates = new QLabel(window);
            candidates->setAttribute(Qt::WA_TransparentForMouseEvents);
            candidates->setTextFormat(Qt::PlainText);
            candidates->setStyleSheet(
                "padding:5px 8px;background:#293749;color:#eef4ff;border:1px solid #91b5db;");
        }
        candidates->setMaximumWidth(window->width());
        candidates->setWordWrap(true);
        candidates->setText(hint);
        candidates->adjustSize();
        QPoint at = target->mapTo(window, rect.bottomLeft());
        candidates->move(qBound(0, at.x(), qMax(0, window->width() - candidates->width())),
                         qBound(0, at.y(), qMax(0, window->height() - candidates->height())));
        candidates->show();
        candidates->raise();
    }
    void commit(QString text) {
        spelling.clear();
        choices.clear();
        page = 0;
        if (target) {
            QInputMethodEvent event;
            event.setCommitString(text);
            QApplication::sendEvent(target, &event);
        }
        if (candidates)
            candidates->hide();
    }

public:
    explicit Pinyin(QObject *parent) : QObject(parent) { qApp->installEventFilter(this); }
    bool toggle(bool on) {
        if (on && entries.isEmpty()) {
            QFile dict("/lib/code/pinyin.tsv");
            if (!dict.open(QIODevice::ReadOnly)) {
                qWarning("Pinyin dictionary: %s", qPrintable(dict.errorString()));
                return false;
            }
            auto data = dict.readAll();
            for (auto row : data.split('\n')) {
                int split = row.indexOf('\t');
                if (split > 0)
                    entries.insert(row.left(split), row.mid(split + 1));
            }
        }
        if (!spelling.isEmpty())
            commit({});
        enabled = on && !entries.isEmpty();
        return enabled == on;
    }
    bool eventFilter(QObject *object, QEvent *event) override {
        if (!enabled)
            return false;
        if (event->type() == QEvent::FocusOut && object == target) {
            commit({});
            target.clear();
        }
        if (event->type() != QEvent::KeyPress && event->type() != QEvent::ShortcutOverride)
            return false;
        auto widget = qobject_cast<QWidget *>(object);
        auto key = static_cast<QKeyEvent *>(event);
        if (!widget || !widget->testAttribute(Qt::WA_InputMethodEnabled) ||
            key->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))
            return false;
        QString letter = key->text();
        if (event->type() == QEvent::ShortcutOverride) {
            // Workbenches can bind bare letters and digits to commands. Keep
            // composition keystrokes in the focused text field.
            bool starts = letter.size() == 1 && letter.front() >= 'a' && letter.front() <= 'z';
            if (!starts && spelling.isEmpty()) return false;
            event->accept();
            return true;
        }
        target = widget;
        if (letter.size() == 1 && letter[0] >= 'a' && letter[0] <= 'z') {
            if (spelling.size() < 64)
                spelling += letter;
        } else if (spelling.isEmpty())
            return false;
        else if (letter == "'")
            spelling += letter;
        else if (key->key() == Qt::Key_Backspace)
            spelling.chop(1);
        else if (key->key() == Qt::Key_Escape) {
            commit({});
            return true;
        } else if (key->key() == Qt::Key_Return) {
            commit(spelling);
            return true;
        } else if (letter == "-" || letter == "=" || letter == "+") {
            page = qBound(0, page + (letter == "-" ? -1 : 1), qMax(0, int(choices.size() - 1) / 9));
            update();
            return true;
        } else if (letter == " " || (key->key() >= Qt::Key_1 && key->key() <= Qt::Key_9)) {
            int index = page * 9 + (letter == " " ? 0 : key->key() - Qt::Key_1);
            if (index < choices.size())
                commit(choices[index]);
            else if (letter == " ")
                commit(spelling);
            return true;
        } else {
            commit(spelling);
            return false;
        }
        page = 0;
        choices = QString::fromUtf8(entries.value(QString(spelling).remove('\'').toLatin1()))
                      .split('\t', Qt::SkipEmptyParts);
        update();
        return true;
    }
};
