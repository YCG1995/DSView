#ifndef DECODERCONSOLEDOCK_H
#define DECODERCONSOLEDOCK_H

#include <QWidget>
#include <QTimer>

class QPlainTextEdit;
class QCheckBox;
class QSpinBox;
class QPushButton;

namespace pv {

class SigSession;

namespace dock {

/**
 * Debug console for protocol decoder scripts.
 *
 * Shows everything decoder scripts write to stdout/stderr (print(), logging,
 * tracebacks) and hosts the script-debug controls: decode range limiting and
 * auto reload on save.
 */
class DecoderConsoleDock : public QWidget
{
    Q_OBJECT

public:
    explicit DecoderConsoleDock(QWidget *parent, SigSession *session);
    ~DecoderConsoleDock();

    // Push the dock's default control state (auto reload / debug sample
    // limit) into the session; toggled() only fires on user input.
    void apply_initial_state();

    // Everything the console window currently shows.
    QString text() const;

private slots:
    void on_poll();
    void on_clear();
    void on_copy();
    void on_apply_range();
    void on_auto_reload_clicked(bool checked);

private:
    SigSession *_session;
    QPlainTextEdit *_console;
    QCheckBox *_auto_scroll;
    QCheckBox *_auto_reload;
    QSpinBox *_limit_ms;
    QTimer _poll_timer;
};

} // namespace dock
} // namespace pv

#endif // DECODERCONSOLEDOCK_H
