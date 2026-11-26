#pragma once
#include "quickjs.h"
#include <QApplication>
#include <QCloseEvent>
#include <QFileDialog>
#include <QFontDatabase>
#include <QInputDialog>
#include <QLabel>
#include <QMainWindow>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QSaveFile>
#include <QStatusBar>
#include <QTextDocument>
#include <QScrollBar>
#include <QTimer>
#include <QtPlugin>
#include <cstdio>
#include <cstdlib>

class DocumentModel {
    JSRuntime *runtime = JS_NewRuntime();
    JSContext *ctx = runtime ? JS_NewContext(runtime) : nullptr;
    JSValue global = ctx ? JS_GetGlobalObject(ctx) : JS_UNDEFINED;

    bool check(JSValue value) {
        if (!JS_IsException(value))
            return true;
        JSValue exception = JS_GetException(ctx);
        const char *message = JS_ToCString(ctx, exception);
        std::fprintf(stderr, "code: document model: %s\n", message ? message : "exception");
        JS_FreeCString(ctx, message);
        JS_FreeValue(ctx, exception);
        return false;
    }
    JSValue string(const QString &s) {
        QByteArray bytes = s.toUtf8();
        return JS_NewStringLen(ctx, bytes.constData(), bytes.size());
    }
    bool call(const char *name, int argc, JSValue *args, QString *result = nullptr) {
        JSValue fn = JS_GetPropertyStr(ctx, global, name);
        JSValue value = JS_Call(ctx, fn, global, argc, args);
        bool ok = check(value);
        if (ok && result) {
            size_t size;
            const char *s = JS_ToCStringLen(ctx, &size, value);
            ok = s != nullptr;
            if (ok)
                *result = QString::fromUtf8(s, size);
            JS_FreeCString(ctx, s);
        }
        JS_FreeValue(ctx, value);
        JS_FreeValue(ctx, fn);
        for (int i = 0; i < argc; ++i)
            JS_FreeValue(ctx, args[i]);
        return ok;
    }

public:
    ~DocumentModel() {
        if (ctx) {
            JS_FreeValue(ctx, global);
            JS_FreeContext(ctx);
        }
        if (runtime)
            JS_FreeRuntime(runtime);
    }
    bool init() {
        QFile js("/lib/code/buffer.js");
        if (!ctx || !js.open(QIODevice::ReadOnly))
            return false;
        QByteArray script = js.readAll();
        JSValue value = JS_Eval(ctx, script.constData(), script.size(), "buffer.js", JS_EVAL_TYPE_GLOBAL);
        bool ok = check(value);
        JS_FreeValue(ctx, value);
        return ok;
    }
    bool load(const QString &text, QString &normalized) {
        JSValue args[] = {string(text)};
        return call("resetDocument", 1, args, &normalized);
    }
    bool change(int offset, int removed, const QString &inserted) {
        JSValue args[] = {JS_NewInt32(ctx, offset), JS_NewInt32(ctx, removed), string(inserted)};
        return call("changeDocument", 3, args);
    }
    bool read(QString &text) { return call("documentText", 0, nullptr, &text); }
};

#include "code-view.h"
#include "../gui/pinyin-qt.h"
#include "../gui/shared-clipboard.h"
#include <QVBoxLayout>
#include <functional>

class Page : public QWidget {
    DocumentModel model;

public:
    CodeView *view = new CodeView(this);
    std::function<void()> changed;
    std::function<void(QString)> message;
    QString path;
    QByteArray original, observed;
    bool observedExists = false;
    int modelLength = 0;
    bool bom = false, syncing = false, valid = false, existed = false;

    void title() {
        if (changed)
            changed();
    }
    void error(const QString &message) { QMessageBox::warning(this, "Code", message); }
    bool load(const QString &fileName) {
        QByteArray data;
        bool exists = !fileName.isEmpty() && QFileInfo::exists(fileName);
        if (exists) {
            QFile file(fileName);
            if (!file.open(QIODevice::ReadOnly)) {
                error(file.errorString());
                return false;
            }
            data = file.readAll();
            if (file.error() != QFileDevice::NoError) {
                error(file.errorString());
                return false;
            }
        }
        return applyBytes(fileName, data, exists);
    }
    bool applyBytes(QString fileName, const QByteArray &data, bool exists) {
        bool hasBom = data.startsWith("\xef\xbb\xbf");
        QByteArray content = hasBom ? data.mid(3) : data;
        QString text = QString::fromUtf8(content);
        if (text.toUtf8() != content || content.contains('\0')) {
            error("This file is binary or uses an encoding other than UTF-8.");
            return false;
        }
        QString normalized;
        if (!model.load(text, normalized)) {
            modelError();
            return false;
        }
        syncing = true;
        view->setPlainText(normalized);
        view->document()->setModified(false);
        syncing = false;
        modelLength = normalized.size();
        valid = true;
        view->setReadOnly(false);
        path = fileName;
        view->syntax.setPath(path);
        original = observed = data;
        existed = observedExists = exists;
        bom = hasBom;
        title();
        return true;
    }
    void checkExternal() {
        if (path.isEmpty() || !valid || QApplication::activeModalWidget())
            return;
        QFile disk(path);
        const bool present = disk.exists();
        QByteArray bytes;
        if (present) {
            if (!disk.open(QIODevice::ReadOnly))
                return;
            bytes = disk.readAll();
            if (disk.error() != QFileDevice::NoError)
                return;
        }
        if (present == observedExists && bytes == observed)
            return;
        observedExists = present;
        observed = bytes;
        if (!present) {
            view->document()->setModified(true);
            message("File removed on disk. Your open copy is available to save.");
            return;
        }
        if (view->document()->isModified() &&
            QMessageBox::question(this, "File changed on disk",
                                  "Reload the disk version and discard your unsaved edits?",
                                  QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
            return;
        int offset = view->textCursor().position(), scroll = view->verticalScrollBar()->value();
        if (applyBytes(path, bytes, true)) {
            QTextCursor cursor(view->document());
            cursor.setPosition(qMin(offset, view->document()->characterCount() - 1));
            view->setTextCursor(cursor);
            view->verticalScrollBar()->setValue(scroll);
            message("Reloaded " + path);
        }
    }
    void modelError() {
        valid = false;
        view->setReadOnly(true);
        error("The document model failed. Saving is disabled to protect the file.");
    }
    bool save(bool choose = false) {
        if (!valid)
            return false;
        QString target = path;
        if (target.isEmpty() || choose)
            target = QFileDialog::getSaveFileName(this, "Save file", target);
        if (target.isEmpty())
            return false;
        if (target == path) {
            QFile current(target);
            if (current.exists()) {
                if (!current.open(QIODevice::ReadOnly)) {
                    error(current.errorString());
                    return false;
                }
                QByteArray bytes = current.readAll();
                if (current.error() != QFileDevice::NoError) {
                    error(current.errorString());
                    return false;
                }
                if ((!existed || bytes != original) &&
                    QMessageBox::question(this, "File changed", "Replace the file changed on disk?",
                                          QMessageBox::Yes | QMessageBox::Cancel,
                                          QMessageBox::Cancel) != QMessageBox::Yes)
                    return false;
            } else if (existed && QMessageBox::question(this, "File removed", "Create the file again?",
                                                        QMessageBox::Yes | QMessageBox::Cancel,
                                                        QMessageBox::Cancel) != QMessageBox::Yes)
                return false;
        }
        QString text;
        if (!model.read(text)) {
            modelError();
            return false;
        }
        QByteArray data = text.toUtf8();
        if (bom)
            data.prepend("\xef\xbb\xbf");
        QSaveFile file(target);
        if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) {
            error(file.errorString());
            return false;
        }
        original = observed = data;
        existed = observedExists = true;
        path = target;
        view->syntax.setPath(path);
        view->document()->setModified(false);
        title();
        message("Saved " + path);
        return true;
    }
    bool mayLeave() {
        if (!view->document()->isModified())
            return true;
        auto choice = QMessageBox::warning(this, "Unsaved changes", "Save changes to this document?",
                                           QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
                                           QMessageBox::Save);
        return choice == QMessageBox::Discard || (choice == QMessageBox::Save && save());
    }
    explicit Page(QWidget *parent) : QWidget(parent) {
        auto layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->addWidget(view);
        view->setFrameShape(QFrame::NoFrame);
        view->setLineWrapMode(QPlainTextEdit::NoWrap);
        view->setFont(QFont("JetBrains Mono", 12));
        view->setTabStopDistance(4 * QFontMetricsF(view->font()).horizontalAdvance(' '));
        view->document()->setDocumentMargin(12);
        connect(view, &QPlainTextEdit::cursorPositionChanged, this, [this] { title(); });
        connect(view->document(), &QTextDocument::modificationChanged, this, [this] { title(); });
        connect(view->document(), &QTextDocument::contentsChange, this,
                [this](int pos, int removed, int added) {
                    if (!valid || syncing)
                        return;
                    // Qt can include its final paragraph marker in both counts when pasting
                    // over a selection. This marker is outside the file's text.
                    int newLength = view->document()->characterCount() - 1;
                    removed = qMin(removed, modelLength - pos);
                    added = qMin(added, newLength - pos);
                    if (removed < 0 || added < 0 || modelLength - removed + added != newLength) {
                        modelError();
                        return;
                    }
                    QTextCursor c(view->document());
                    c.setPosition(pos);
                    c.setPosition(pos + added, QTextCursor::KeepAnchor);
                    QString inserted = c.selectedText().replace(QChar::ParagraphSeparator, '\n');
                    if (model.change(pos, removed, inserted))
                        modelLength = newLength;
                    else
                        modelError();
                });
        auto watch = new QTimer(this);
        connect(watch, &QTimer::timeout, this, [this] {
            if (isVisible())
                checkExternal();
        });
        watch->start(1500);

        view->setFocus();
    }
    ~Page() override {
        valid = false;
        QObject::disconnect(view->document(), nullptr, this, nullptr);
        QObject::disconnect(view, nullptr, this, nullptr);
        delete view;
    }
    bool start(const QString &name) {
        if (!model.init() || !load(QString()))
            return false;
        return name.isEmpty() || load(QFileInfo(name).absoluteFilePath());
    }
};
