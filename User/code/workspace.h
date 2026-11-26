#pragma once
#include <QClipboard>
#include <QDockWidget>
#include <QFormLayout>
#include <QPushButton>
#include <QShortcut>
#include <QTabWidget>
#include "project.h"
#include "build-panel.h"

class Editor : public QMainWindow {
    QTabWidget *tabs = new QTabWidget(this);
    QLabel *filename = new QLabel(this), *position = new QLabel(this);
    ProjectTree *tree = new ProjectTree(this);
    BuildPanel *output = new BuildPanel(this);
    QDockWidget *explorer = nullptr, *buildDock = nullptr;
    QString root;
    Pinyin ime{this};
    bool waylandSelection = false;
    QByteArray guestAtOffer;
    Page *page(int index) const { return static_cast<Page *>(tabs->widget(index)); }
    Page *active() const { return page(tabs->currentIndex()); }
    void notice(QString text) { statusBar()->showMessage(text, 3500); }
    void updateTitle(Page *p) {
        int index = tabs->indexOf(p);
        if (index < 0)
            return;
        QString label = p->path.isEmpty() ? "Untitled" : QFileInfo(p->path).fileName();
        if (p->view->document()->isModified())
            label += " *";
        tabs->setTabText(index, label);
        tabs->setTabToolTip(index, p->path);
        if (active() != p)
            return;
        filename->setText(label);
        setWindowTitle(label + " — Code");
        auto cursor = p->view->textCursor();
        position->setText(QString("Ln %1, Col %2     UTF-8    ")
                              .arg(cursor.blockNumber() + 1)
                              .arg(cursor.positionInBlock() + 1));
    }
    Page *open(QString name, int line = 0, int col = 1) {
        if (!name.isEmpty())
            name = QDir::cleanPath(QFileInfo(name).absoluteFilePath());
        Page *p = nullptr;
        if (!name.isEmpty())
            for (int i = 0; i < tabs->count(); ++i)
                if (page(i)->path == name) {
                    p = page(i);
                    break;
                }
        if (!p) {
            p = new Page(tabs);
            p->view->clipboard = [this](QKeySequence::StandardKey key) {
                if (key == QKeySequence::Paste)
                    paste(false);
                else
                    copy(key == QKeySequence::Cut, false);
            };
            p->changed = [this, p] { updateTitle(p); };
            p->message = [this](QString text) { notice(text); };
            if (!p->start(name)) {
                delete p;
                return nullptr;
            }
            tabs->addTab(p, "");
            updateTitle(p);
        }
        tabs->setCurrentWidget(p);
        p->view->setFocus();
        if (line > 0) {
            auto block = p->view->document()->findBlockByNumber(line - 1);
            if (block.isValid()) {
                QTextCursor c(block);
                c.setPosition(block.position() + qBound(0, col - 1, block.length() - 1));
                p->view->setTextCursor(c);
                p->view->centerCursor();
            }
        }
        return p;
    }
    void closeTab(int index) {
        if (tabs->count() == 1) {
            close();
            return;
        }
        auto p = page(index);
        if (p && p->mayLeave()) {
            tabs->removeTab(index);
            p->deleteLater();
        }
    }
    void search(bool text) {
        ProjectSearch dialog(this, root, text);
        if (dialog.exec() == QDialog::Accepted)
            open(dialog.file, dialog.number);
    }
    void replaceDialog(bool replace) {
        auto view = active()->view;
        QDialog dialog(this);
        dialog.setWindowTitle(replace ? "Replace in document" : "Find in document");
        QFormLayout form(&dialog);
        QLineEdit query(view->textCursor().selectedText()), replacement;
        QLabel count;
        QPushButton next("Find next"), one("Replace"), all("Replace all");
        form.addRow("Find", &query);
        if (replace)
            form.addRow("Replace with", &replacement);
        form.addRow(&next);
        auto find = [&] {
            if (query.text().isEmpty())
                return false;
            if (view->find(query.text()))
                return true;
            auto cursor = view->document()->find(query.text());
            if (cursor.isNull()) {
                count.setText("No matches");
                return false;
            }
            view->setTextCursor(cursor);
            return true;
        };
        connect(&next, &QPushButton::clicked, &dialog, find);
        if (replace) {
            form.addRow(&one, &all);
            connect(&one, &QPushButton::clicked, &dialog, [&] {
                if (query.text().isEmpty())
                    return;
                if (view->textCursor().selectedText().compare(query.text(), Qt::CaseInsensitive) && !find())
                    return;
                auto c = view->textCursor();
                c.insertText(replacement.text());
                view->setTextCursor(c);
                find();
            });
            connect(&all, &QPushButton::clicked, &dialog, [&] {
                if (query.text().isEmpty())
                    return;
                QTextCursor group(view->document()), cursor(view->document());
                group.beginEditBlock();
                int n = 0;
                while (!(cursor = view->document()->find(query.text(), cursor)).isNull()) {
                    cursor.insertText(replacement.text());
                    ++n;
                }
                group.endEditBlock();
                count.setText(QString("Replaced %1 matches").arg(n));
            });
        }
        form.addRow(&count);
        next.setDefault(true);
        query.selectAll();
        query.setFocus();
        dialog.exec();
    }
    void copy(bool cut, bool mac) {
        auto view = active()->view;
        auto cursor = view->textCursor();
        if (!cursor.hasSelection())
            return;
        auto value = cursor.selectedText().replace(QChar::ParagraphSeparator, '\n').toUtf8();
        if (value.size() > LX_CLIP_MAX) {
            notice("Shared clipboard limit is 1 MiB");
            return;
        }
        auto bytes = reinterpret_cast<unsigned char *>(value.data());
        unsigned size = unsigned(value.size());
        if (mac && clip_mac(1, &bytes, &size) < 0) {
            notice("Mac clipboard bridge unavailable");
            return;
        }
        if (clip_store(bytes, size) < 0) {
            notice("Cannot save the guest clipboard");
            return;
        }
        waylandSelection = false;
        view->copy();
        if (cut)
            cursor.removeSelectedText();
    }
    void paste(bool mac) {
        auto view = active()->view;
        if (mac) {
            unsigned char *bytes = nullptr;
            unsigned size = 0;
            if (clip_mac(0, &bytes, &size) < 0) {
                notice("Mac clipboard bridge unavailable");
                return;
            }
            view->insertPlainText(QString::fromUtf8(reinterpret_cast<char *>(bytes), size));
            clip_store(bytes, size);
            free(bytes);
        } else {
            QFile file(LX_CLIP_FILE);
            bool available = file.open(QIODevice::ReadOnly);
            QByteArray local = available ? file.readAll() : QByteArray();
            if (waylandSelection && local == guestAtOffer)
                view->paste();
            else if (available)
                view->insertPlainText(QString::fromUtf8(local));
            else
                view->paste();
        }
    }
    void build() {
        auto p = active();
        if (!p || output->busy())
            return;
        if (p->path.isEmpty() || p->view->document()->isModified()) {
            if (!p->save())
                return;
        }
        if (QFileInfo(p->path).suffix().toLower() != "c") {
            notice("Open a C source file to build with TCC");
            return;
        }
        buildDock->show();
        output->build(p->path);
    }
    QDockWidget *dock(QString title, QWidget *widget, Qt::DockWidgetArea area) {
        auto d = new QDockWidget(title, this);
        d->setAllowedAreas(area);
        d->setWidget(widget);
        d->setFeatures(QDockWidget::DockWidgetClosable);
        addDockWidget(area, d);
        return d;
    }

protected:
    void closeEvent(QCloseEvent *e) override {
        if (output->busy()) {
            notice("Waiting for the compiler to finish");
            e->ignore();
            return;
        }
        for (int i = 0; i < tabs->count(); ++i) {
            if (page(i)->view->document()->isModified())
                tabs->setCurrentIndex(i);
            if (!page(i)->mayLeave()) {
                e->ignore();
                return;
            }
        }
        e->accept();
    }

public:
    Editor() {
        setCentralWidget(tabs);
        tabs->setTabsClosable(true);
        tabs->setDocumentMode(true);
        connect(tabs, &QTabWidget::currentChanged, this, [this](int i) {
            if (i >= 0)
                updateTitle(page(i));
        });
        connect(tabs, &QTabWidget::tabCloseRequested, this, [this](int i) { closeTab(i); });
        explorer = dock("Project", tree, Qt::LeftDockWidgetArea);
        buildDock = dock("Build output", output, Qt::BottomDockWidgetArea);
        buildDock->hide();
        connect(tree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item) {
            auto path = item->data(0, Qt::UserRole).toString();
            if (QFileInfo(path).isFile())
                open(path);
        });
        auto diagnostic = [this](QListWidgetItem *item) {
            auto path = item->data(Qt::UserRole).toString();
            if (!path.isEmpty())
                open(path, item->data(Qt::UserRole + 1).toInt(), item->data(Qt::UserRole + 2).toInt());
        };
        connect(output, &QListWidget::itemClicked, this, diagnostic);
        connect(output, &QListWidget::itemActivated, this, diagnostic);
        setStyleSheet("QPlainTextEdit,QTreeWidget,QListWidget "
                      "{background:#1e1f23;color:#e4e6eb;selection-background-color:#315778;}"
                      "QMainWindow,QDockWidget,QMenuBar,QMenu,QTabBar::tab {background:#2c2e34;color:#eceef2;}"
                      "QTabBar::tab {padding:5px 8px;} "
                      "QTabBar::tab:selected,QMenu::item:selected,QMenuBar::item:selected {background:#405368;}"
                      "QStatusBar {background:#243850;color:#e0edf8;} QStatusBar::item {border:0;}");
        if (!qEnvironmentVariableIsSet("LIXINOS_SINGLE_CLIENT")) {
            auto clipboard = QApplication::clipboard();
            connect(clipboard, &QClipboard::dataChanged, this, [this, clipboard] {
                waylandSelection = !clipboard->ownsClipboard();
                if (waylandSelection) {
                    QFile local(LX_CLIP_FILE);
                    guestAtOffer = local.open(QIODevice::ReadOnly) ? local.readAll() : QByteArray();
                }
            });
        }
        auto file = menuBar()->addMenu("&File"), edit = menuBar()->addMenu("&Edit"),
             project = menuBar()->addMenu("&Project");
        file->addAction("New tab", Qt::CTRL | Qt::Key_N, this, [this] { open({}); });
        file->addAction("Open file…", Qt::CTRL | Qt::Key_O, this, [this] {
            auto name = QFileDialog::getOpenFileName(this, "Open file", root);
            if (!name.isEmpty())
                open(name);
        });
        file->addAction("Open folder…", this, [this] {
            auto name = QFileDialog::getExistingDirectory(this, "Open folder", root);
            if (!name.isEmpty()) {
                root = name;
                tree->setProject(root);
                explorer->show();
            }
        });
        file->addAction("Save", Qt::CTRL | Qt::Key_S, this, [this] { active()->save(); });
        file->addAction("Save as…", Qt::CTRL | Qt::SHIFT | Qt::Key_S, this, [this] { active()->save(true); });
        file->addAction("Close tab", Qt::CTRL | Qt::Key_W, this, [this] { closeTab(tabs->currentIndex()); });
        edit->addAction("Undo", Qt::CTRL | Qt::Key_Z, this, [this] { active()->view->undo(); });
        auto redo = edit->addAction("Redo", Qt::CTRL | Qt::Key_Y, this, [this] { active()->view->redo(); });
        redo->setShortcuts({Qt::CTRL | Qt::Key_Y, Qt::CTRL | Qt::SHIFT | Qt::Key_Z});
        edit->addAction("Cut", Qt::CTRL | Qt::Key_X, this, [this] { copy(true, false); });
        edit->addAction("Copy", Qt::CTRL | Qt::Key_C, this, [this] { copy(false, false); });
        edit->addAction("Paste", Qt::CTRL | Qt::Key_V, this, [this] { paste(false); });
        edit->addAction("Copy to Mac", Qt::Key_F8, this, [this] { copy(false, true); });
        edit->addAction("Paste from Mac", Qt::Key_F9, this, [this] { paste(true); });
        edit->addAction("Find…", Qt::CTRL | Qt::Key_F, this, [this] { replaceDialog(false); });
        edit->addAction("Replace…", Qt::CTRL | Qt::Key_H, this, [this] { replaceDialog(true); });
        auto input = edit->addAction("Chinese pinyin", Qt::Key_F7);
        input->setCheckable(true);
        connect(input, &QAction::toggled, this, [this, input](bool on) {
            if (!ime.toggle(on)) {
                input->setChecked(false);
                notice("Cannot load pinyin dictionary");
            } else
                notice(on ? "Pinyin · Space or 1–9 selects · −/+ changes page" : "Latin input");
        });
        project->addAction(explorer->toggleViewAction());
        project->addAction("Quick open…", Qt::CTRL | Qt::Key_P, this, [this] { search(false); });
        auto find = project->addAction("Search project…", Qt::Key_F6, this, [this] { search(true); });
        find->setShortcuts({Qt::Key_F6, Qt::CTRL | Qt::SHIFT | Qt::Key_F});
        project->addAction("Build with TCC", Qt::Key_F5, this, [this] { build(); });
        project->addAction(buildDock->toggleViewAction());
        filename->setTextFormat(Qt::PlainText);
        filename->setStyleSheet("padding-left:10px;color:#e0edf8;");
        position->setStyleSheet("color:#e0edf8;");
        statusBar()->addWidget(filename, 1);
        statusBar()->addPermanentWidget(position);
        resize(910, 600);
    }
    bool start(QString name) {
        QFileInfo info(name);
        bool folder = !name.isEmpty() && info.isDir();
        root = folder ? info.absoluteFilePath() : name.isEmpty() ? QDir::currentPath() : info.absolutePath();
        tree->setProject(root);
        explorer->setVisible(folder);
        return open(folder ? QString{} : name) != nullptr;
    }
};
