#pragma once
#include <QDialog>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <memory>

class ProjectTree : public QTreeWidget {
    QString project;
    static QFileInfoList children(const QString &path) {
        return QDir(path).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot, QDir::DirsFirst | QDir::Name);
    }
    void refresh(QTreeWidgetItem *parent, const QString &path) {
        auto entries = children(path);
        QHash<QString, QTreeWidgetItem *> items;
        for (int i = 0; i < parent->childCount(); ++i)
            items.insert(parent->child(i)->text(0), parent->child(i));
        int index = 0;
        for (const auto &entry : entries) {
            auto item = items.take(entry.fileName());
            if (!item) {
                item = new QTreeWidgetItem(QStringList{entry.fileName()});
                parent->insertChild(index, item);
            }
            bool dir = entry.isDir() && !entry.isSymLink();
            item->setData(0, Qt::UserRole, entry.absoluteFilePath());
            item->setChildIndicatorPolicy(dir ? QTreeWidgetItem::ShowIndicator
                                              : QTreeWidgetItem::DontShowIndicator);
            item->setForeground(0, dir ? QColor("#a6caff") : QColor("#e4e6eb"));
            if (dir && item->isExpanded())
                refresh(item, entry.absoluteFilePath());
            else if (!dir)
                qDeleteAll(item->takeChildren());
            ++index;
        }
        qDeleteAll(items);
    }

public:
    explicit ProjectTree(QWidget *parent) : QTreeWidget(parent) {
        setHeaderHidden(true);
        connect(this, &QTreeWidget::itemExpanded, this,
                [this](QTreeWidgetItem *item) { refresh(item, item->data(0, Qt::UserRole).toString()); });
        auto timer = new QTimer(this);
        connect(timer, &QTimer::timeout, this, [this] {
            if (isVisible())
                refresh(invisibleRootItem(), project);
        });
        timer->start(2200);
    }
    void setProject(QString path) {
        project = path;
        clear();
        refresh(invisibleRootItem(), project);
    }
};

// Enumerate one directory at a time so excluded directories are never traversed.
// Time slices let the dialog accept a different query or close during a search.
class ProjectSearch : public QDialog {
    const QString root;
    const bool contents;
    QLineEdit *query = new QLineEdit(this);
    QListWidget *results = new QListWidget(this);
    QLabel *summary = new QLabel(this);
    QTimer timer;
    QStringList pending;
    std::unique_ptr<QDirIterator> listing;
    QFile current;
    int line = 0;
    void add(const QString &file, int number, const QString &snippet = {}) {
        auto relative = QDir(root).relativeFilePath(file);
        auto item = new QListWidgetItem(
            number ? QString("%1:%2   %3").arg(relative).arg(number).arg(snippet) : relative, results);
        item->setData(Qt::UserRole, file);
        item->setData(Qt::UserRole + 1, number);
        if (results->count() == 1)
            results->setCurrentItem(item);
    }
    void restart() {
        timer.stop();
        results->clear();
        listing.reset();
        current.close();
        pending = QStringList{root};
        summary->setText(contents && query->text().isEmpty() ? "Enter text to search" : "Searching…");
        if (!contents || !query->text().isEmpty())
            timer.start(1);
    }
    void scan() {
        QElapsedTimer budget;
        budget.start();
        while (budget.elapsed() < 14 && results->count() < 300) {
            if (current.isOpen()) {
                if (current.atEnd()) {
                    current.close();
                    continue;
                }
                QByteArray raw = current.readLine();
                ++line;
                if (raw.contains('\0')) {
                    current.close();
                    continue;
                }
                QString row = QString::fromUtf8(raw);
                if (row.contains(query->text(), Qt::CaseInsensitive))
                    add(current.fileName(), line, row.trimmed().left(180));
                continue;
            }
            if (!listing || !listing->hasNext()) {
                if (pending.isEmpty())
                    break;
                listing = std::make_unique<QDirIterator>(
                    pending.takeLast(), QDir::AllEntries | QDir::NoDotAndDotDot | QDir::NoSymLinks);
                continue;
            }
            auto path = listing->next();
            auto info = listing->fileInfo();
            if (info.isDir()) {
                if (path != "/dev" && path != "/proc" && path != "/sys")
                    pending.append(path);
            } else if (info.isFile()) {
                if (!contents) {
                    if (QDir(root).relativeFilePath(path).contains(query->text(), Qt::CaseInsensitive))
                        add(path, 0);
                } else if (info.size() <= 4 * 1024 * 1024) {
                    current.setFileName(path);
                    if (current.open(QIODevice::ReadOnly)) {
                        if (current.peek(4096).contains('\0'))
                            current.close();
                        line = 0;
                    }
                }
            }
        }
        if (results->count() >= 300 ||
            (pending.isEmpty() && !current.isOpen() && (!listing || !listing->hasNext()))) {
            timer.stop();
            summary->setText(results->count() == 300 ? "First 300 matches · refine the query"
                                                     : QString("%1 matches").arg(results->count()));
        }
    }

public:
    QString file;
    int number = 0;
    ProjectSearch(QWidget *parent, QString directory, bool searchText)
        : QDialog(parent), root(directory), contents(searchText) {
        setWindowTitle(contents ? "Search project" : "Quick open");
        auto layout = new QVBoxLayout(this);
        query->setPlaceholderText(contents ? "Find text in project files" : "Filter by filename or path");
        layout->addWidget(query);
        layout->addWidget(results);
        layout->addWidget(summary);
        connect(&timer, &QTimer::timeout, this, [this] { scan(); });
        connect(query, &QLineEdit::textChanged, this, [this] { restart(); });
        auto choose = [this] {
            if (auto item = results->currentItem()) {
                file = item->data(Qt::UserRole).toString();
                number = item->data(Qt::UserRole + 1).toInt();
                accept();
            }
        };
        connect(query, &QLineEdit::returnPressed, this, choose);
        connect(results, &QListWidget::itemClicked, this, choose);
        connect(results, &QListWidget::itemActivated, this, choose);
        auto next = new QShortcut(QKeySequence(Qt::Key_Down), query);
        connect(next, &QShortcut::activated, this, [this] { results->setFocus(); });
        resize(570, 370);
        restart();
    }
};
