#include "decoderconsoledock.h"

#include <libsigrokdecode.h>

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPlainTextEdit>
#include <QCheckBox>
#include <QSpinBox>
#include <QLabel>
#include <QPushButton>
#include <QTime>
#include <QTextCursor>
#include <QApplication>
#include <QClipboard>
#include <QFontDatabase>

#include "../sigsession.h"

namespace pv {
namespace dock {

DecoderConsoleDock::DecoderConsoleDock(QWidget *parent, SigSession *session)
    : QWidget(parent),
      _session(session),
      _console(nullptr),
      _auto_scroll(nullptr),
      _auto_reload(nullptr),
      _limit_ms(nullptr)
{
    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(4, 4, 4, 4);
    lay->setSpacing(4);

    // ---- command row ----
    auto *row = new QHBoxLayout();
    row->setSpacing(6);

    QPushButton *clear_btn = new QPushButton(tr("清空"), this);
    QPushButton *copy_btn = new QPushButton(tr("复制"), this);
    clear_btn->setFixedHeight(22);
    copy_btn->setFixedHeight(22);
    row->addWidget(clear_btn);
    row->addWidget(copy_btn);

    _auto_scroll = new QCheckBox(tr("自动滚动"), this);
    _auto_scroll->setChecked(true);
    row->addWidget(_auto_scroll);

    _auto_reload = new QCheckBox(tr("保存后自动重载"), this);
    _auto_reload->setChecked(session->auto_reload_scripts());
    row->addWidget(_auto_reload);

    row->addSpacing(8);
    row->addWidget(new QLabel(tr("只解前"), this));
    _limit_ms = new QSpinBox(this);
    _limit_ms->setRange(0, 3600 * 1000);
    _limit_ms->setSuffix(tr(" ms"));
    _limit_ms->setSpecialValueText(tr("全部"));
    _limit_ms->setSingleStep(10);
    _limit_ms->setValue(0);
    _limit_ms->setFixedWidth(96);
    _limit_ms->setToolTip(tr("0 表示解码全部样本；调试脚本时限制前若干毫秒可大幅提速"));
    row->addWidget(_limit_ms);

    QPushButton *apply_btn = new QPushButton(tr("应用并重解"), this);
    apply_btn->setFixedHeight(22);
    row->addWidget(apply_btn);
    row->addStretch();

    lay->addLayout(row);

    // ---- console ----
    _console = new QPlainTextEdit(this);
    _console->setReadOnly(true);
    _console->setMaximumBlockCount(20000);
    _console->setLineWrapMode(QPlainTextEdit::NoWrap);
    QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    mono.setPointSizeF(mono.pointSizeF() > 0 ? mono.pointSizeF() : 9.0);
    _console->setFont(mono);
    _console->setPlaceholderText(
            tr("解码器脚本里 print() 的输出、异常 traceback 会出现在这里"));
    lay->addWidget(_console, 1);

    connect(clear_btn, &QPushButton::clicked, this, &DecoderConsoleDock::on_clear);
    connect(copy_btn, &QPushButton::clicked, this, &DecoderConsoleDock::on_copy);
    connect(apply_btn, &QPushButton::clicked, this, &DecoderConsoleDock::on_apply_range);
    connect(_auto_reload, &QCheckBox::clicked, this,
            &DecoderConsoleDock::on_auto_reload_clicked);

    _poll_timer.setInterval(250);
    connect(&_poll_timer, &QTimer::timeout, this, &DecoderConsoleDock::on_poll);
    _poll_timer.start();

    // Never steal keyboard focus from the waveform.
    setFocusPolicy(Qt::NoFocus);
}

DecoderConsoleDock::~DecoderConsoleDock()
{
    _poll_timer.stop();
}

void DecoderConsoleDock::apply_initial_state(){
    if (!_session)
        return;
    _session->set_auto_reload_scripts(_auto_reload->isChecked());
    _session->set_decode_debug_limit_ms(_limit_ms->value());
}

QString DecoderConsoleDock::text() const
{
    return _console ? _console->toPlainText() : QString();
}

void DecoderConsoleDock::on_poll()
{
    // Drain the buffer even while the dock is closed. Returning early here
    // used to freeze the C side's ring buffer: the script's print() output of a
    // reload/re-decode was never taken, so the user reopened the console and
    // saw nothing (the reload looked like it had not happened).
    char *text = srd_console_take(1);
    if (!text)
        return;

    if (*text) {
        const QString chunk = QString::fromUtf8(text).trimmed();
        if (!chunk.isEmpty()) {
            const QString stamp = QTime::currentTime().toString("HH:mm:ss");
            const QStringList lines = chunk.split('\n');
            QString out;
            for (int i = 0; i < lines.size(); i++) {
                if (i == 0)
                    out += QString("[%1] %2\n").arg(stamp, lines.at(i));
                else
                    out += lines.at(i) + "\n";
            }

            _console->moveCursor(QTextCursor::End);
            _console->insertPlainText(out);

            if (_auto_scroll->isChecked()) {
                QTextCursor cur = _console->textCursor();
                cur.movePosition(QTextCursor::End);
                _console->setTextCursor(cur);
            }
        }
    }

    g_free(text);
}

void DecoderConsoleDock::on_clear()
{
    _console->clear();
    srd_console_clear();
}

void DecoderConsoleDock::on_copy()
{
    QApplication::clipboard()->setText(_console->toPlainText());
}

void DecoderConsoleDock::on_apply_range()
{
    if (_session)
        _session->set_decode_debug_limit_ms(_limit_ms->value());
}

void DecoderConsoleDock::on_auto_reload_clicked(bool checked)
{
    if (_session)
        _session->set_auto_reload_scripts(checked);
}

} // namespace dock
} // namespace pv
