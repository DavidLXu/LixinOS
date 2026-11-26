#pragma once
#include <functional>
#include <QKeyEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QRegularExpression>
#include <QSyntaxHighlighter>
#include <QTextBlock>

class Syntax : public QSyntaxHighlighter {
    QString language;
    const QRegularExpression tokens{
        R"RX(//[^\n]*|/\*|#[^\n]*|"(?:\\.|[^"\\])*"?|'(?:\\.|[^'\\])*'?|\b\d+(?:\.\d+)?\b|\b[A-Za-z_][A-Za-z_0-9]*\b)RX"};
    const QStringList keywords =
        QString("as async await bool break case char class const continue def do double else enum except "
                "extern false float fn for from if impl import in int let long match mod mut None pass pub "
                "raise return self short signed sizeof static str struct switch True False true try typedef "
                "unsigned use void while with yield")
            .split(' ');

public:
    using QSyntaxHighlighter::QSyntaxHighlighter;
    void setPath(const QString &path) {
        auto ext = QFileInfo(path).suffix().toLower();
        language = ext == "py"                                                     ? "py"
                   : QStringList{"c", "h", "cpp", "hpp", "cc", "rs"}.contains(ext) ? "c"
                                                                                   : "";
        rehighlight();
    }
    void highlightBlock(const QString &text) override {
        if (language.isEmpty())
            return;
        bool multiline = language == "c" && previousBlockState() == 1;
        int at = 0;
        setCurrentBlockState(0);
        while (at < text.size()) {
            if (multiline) {
                int end = text.indexOf("*/", at);
                setFormat(at, end < 0 ? text.size() - at : end + 2 - at, QColor("#80ae94"));
                if (end < 0) {
                    setCurrentBlockState(1);
                    return;
                }
                at = end + 2;
                multiline = false;
                continue;
            }
            auto match = tokens.match(text, at);
            if (!match.hasMatch())
                break;
            QString token = match.captured();
            int start = match.capturedStart();
            at = match.capturedEnd();
            QColor color;
            if (token == "/*" && language == "c") {
                setFormat(start, 2, QColor("#80ae94"));
                multiline = true;
                if (at == text.size())
                    setCurrentBlockState(1);
            } else if (token.startsWith('#'))
                color = language == "py" ? QColor("#80ae94") : QColor("#c5a8ed");
            else if (token.startsWith("//") && language == "c")
                color = QColor("#80ae94");
            else if (token.startsWith('"') || token.startsWith('\''))
                color = QColor("#e4bb90");
            else if (token.front().isDigit())
                color = QColor("#9dd6ca");
            else if (keywords.contains(token))
                color = QColor("#a6caff");
            if (color.isValid())
                setFormat(start, token.size(), color);
        }
    }
};

class CodeView : public QPlainTextEdit {
    class Gutter : public QWidget {
        CodeView *edit;

    public:
        Gutter(CodeView *owner) : QWidget(owner), edit(owner) {}
        void paintEvent(QPaintEvent *event) override { edit->paintNumbers(event->rect()); }
    };
    Gutter *gutter = new Gutter(this);
    int gutterWidth() const {
        return 14 + fontMetrics().horizontalAdvance('9') * QString::number(blockCount()).size();
    }
    void layoutGutter() {
        int w = gutterWidth();
        setViewportMargins(w, 0, 0, 0);
        auto rect = contentsRect();
        gutter->setGeometry(rect.left(), rect.top(), w, rect.height());
    }
    void paintNumbers(const QRect &rect) {
        QPainter p(gutter);
        p.fillRect(rect, QColor("#23262c"));
        p.setPen(QColor("#9aa6b3"));
        for (auto block = firstVisibleBlock(); block.isValid(); block = block.next()) {
            auto top = blockBoundingGeometry(block).translated(contentOffset()).top();
            if (top > rect.bottom())
                break;
            if (block.isVisible())
                p.drawText(0, int(top), gutter->width() - 6, fontMetrics().height(), Qt::AlignRight,
                           QString::number(block.blockNumber() + 1));
        }
    }

protected:
    void resizeEvent(QResizeEvent *e) override {
        QPlainTextEdit::resizeEvent(e);
        layoutGutter();
    }
    void keyPressEvent(QKeyEvent *e) override {
        for (auto action : {QKeySequence::Cut, QKeySequence::Copy, QKeySequence::Paste})
            if (clipboard && e->matches(action)) {
                clipboard(action);
                return;
            }
        if ((e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) && e->modifiers() == Qt::NoModifier &&
            !isReadOnly()) {
            auto cursor = textCursor();
            auto prefix = cursor.block().text().left(cursor.positionInBlock());
            int first = prefix.indexOf(QRegularExpression("\\S"));
            QString indent = prefix.left(first < 0 ? prefix.size() : first);
            if (prefix.trimmed().endsWith('{') || prefix.trimmed().endsWith(':'))
                indent += "    ";
            cursor.beginEditBlock();
            cursor.insertText("\n" + indent);
            cursor.endEditBlock();
            setTextCursor(cursor);
            return;
        }
        QPlainTextEdit::keyPressEvent(e);
    }

public:
    std::function<void(QKeySequence::StandardKey)> clipboard;
    Syntax syntax{document()};
    explicit CodeView(QWidget *parent) : QPlainTextEdit(parent) {
        connect(this, &QPlainTextEdit::blockCountChanged, this, [this] { layoutGutter(); });
        connect(this, &QPlainTextEdit::updateRequest, gutter, [this](const QRect &r, int dy) {
            if (dy)
                gutter->scroll(0, dy);
            else
                gutter->update(0, r.y(), gutter->width(), r.height());
        });
        layoutGutter();
    }
};
