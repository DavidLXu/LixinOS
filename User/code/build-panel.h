#pragma once
#include <QListWidget>
#include <QRegularExpression>
#include <QTemporaryFile>
#include <QTimer>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;

class BuildPanel : public QListWidget {
    pid_t pid = -1;
    int pipeRead = -1;
    QByteArray pending;
    QString directory, target, input;
    std::unique_ptr<QTemporaryFile> staging;
    QTimer timer;
    void output(QByteArray bytes) {
        auto line = QString::fromUtf8(bytes).trimmed();
        if (line.isEmpty())
            return;
        if (count() >= 1000)
            delete takeItem(0);
        auto item = new QListWidgetItem(line, this);
        static QRegularExpression diagnostic(R"(^(.+?):(\d+)(?::(\d+))?:)");
        // The native TCC driver prefixes the upstream file:line diagnostic
        // with its input path, including when a header caused the error.
        auto detail = line.startsWith(input + ": ") ? line.mid(input.size() + 2) : line;
        auto match = diagnostic.match(detail);
        if (match.hasMatch()) {
            item->setData(Qt::UserRole, QDir(directory).absoluteFilePath(match.captured(1)));
            item->setData(Qt::UserRole + 1, match.captured(2).toInt());
            item->setData(Qt::UserRole + 2, match.captured(3).toInt());
        }
        scrollToBottom();
    }
    void pollChild() {
        char bytes[4096];
        ssize_t n;
        while ((n = read(pipeRead, bytes, sizeof bytes)) > 0) {
            pending.append(bytes, n);
            int newline;
            while ((newline = pending.indexOf('\n')) >= 0) {
                output(pending.left(newline));
                pending.remove(0, newline + 1);
            }
            if (pending.size() > 8192) {
                output(pending);
                pending.clear();
            }
        }
        if (n < 0 && (errno == EAGAIN || errno == EINTR))
            return;
        int status;
        auto result = waitpid(pid, &status, WNOHANG);
        if (!result || (result < 0 && errno == EINTR))
            return;
        output(pending);
        pending.clear();
        if (result > 0 && WIFEXITED(status) && !WEXITSTATUS(status) &&
            QFileInfo(staging->fileName()).size() > 0) {
            if (::rename(QFile::encodeName(staging->fileName()).constData(),
                         QFile::encodeName(target).constData()) == 0)
                addItem("Built " + target);
            else
                addItem("Cannot install output: " + QString::fromLocal8Bit(strerror(errno)));
        } else {
            addItem("Build failed");
            for (int i = 0; i < count(); ++i) {
                if (item(i)->data(Qt::UserRole).isValid()) {
                    setCurrentRow(i);
                    setFocus();
                    break;
                }
            }
        }
        timer.stop();
        closePipe();
        staging.reset();
        pid = -1;
    }
    void closePipe() {
        if (pipeRead >= 0) {
            ::close(pipeRead);
            pipeRead = -1;
        }
    }

public:
    bool busy() const { return pid >= 0; }
    explicit BuildPanel(QWidget *parent) : QListWidget(parent) {
        connect(&timer, &QTimer::timeout, this, [this] { pollChild(); });
    }
    void build(QString source) {
        if (busy())
            return;
        clear();
        QFileInfo info(source);
        input = source;
        directory = info.absolutePath();
        target = directory + "/" + info.completeBaseName() + ".lxe";
        staging = std::make_unique<QTemporaryFile>(directory + "/.code-build-XXXXXX");
        if (!staging->open()) {
            addItem("Cannot create output: " + staging->errorString());
            return;
        }
        staging->close();
        int fds[2];
        if (pipe(fds) < 0) {
            addItem("Cannot open compiler output pipe");
            staging.reset();
            return;
        }
        pipeRead = fds[0];
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(&actions, fds[1], 1);
        posix_spawn_file_actions_adddup2(&actions, fds[1], 2);
        posix_spawn_file_actions_addchdir_np(&actions, QFile::encodeName(directory).constData());
        QByteArray command = "/bin/tcc", filename = QFile::encodeName(source);
        QByteArray option = "-o", outputName = QFile::encodeName(staging->fileName());
        char *args[] = {command.data(), filename.data(), option.data(), outputName.data(), nullptr};
        int error = posix_spawn(&pid, args[0], &actions, nullptr, args, environ);
        posix_spawn_file_actions_destroy(&actions);
        ::close(fds[1]);
        if (error) {
            pid = -1;
            closePipe();
            staging.reset();
            addItem("Cannot start /bin/tcc: " + QString::fromLocal8Bit(strerror(error)));
            return;
        }
        fcntl(pipeRead, F_SETFL, O_NONBLOCK);
        addItem("Building " + source);
        timer.start(40);
    }
};
