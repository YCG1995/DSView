/*
 * Calibration dialog implementation.
 */
#include "ppscalibdlg.h"
#include "../pps/ppstesthooks.h"

#include <QFormLayout>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QGroupBox>
#include <QDialogButtonBox>

namespace pv {
namespace dialogs {

PpsCalibDlg::PpsCalibDlg(SigSession *session, int default_channel,
		int max_channel, QWidget *parent)
	: QDialog(parent)
	, _session(session)
	, _factor(1.0)
	, _has_result(false)
{
	setWindowTitle(QString::fromUtf8("1PPS 校准"));
	setModal(true);

	QVBoxLayout *root = new QVBoxLayout(this);

	QGroupBox *box = new QGroupBox(QString::fromUtf8("校准参数"), this);
	QFormLayout *form = new QFormLayout(box);
	form->setLabelAlignment(Qt::AlignRight);

	_channel_spin = new QSpinBox(box);
	_channel_spin->setRange(0, max_channel > 0 ? max_channel : 1);
	_channel_spin->setValue(default_channel);
	form->addRow(QString::fromUtf8("校准通道"), _channel_spin);

	_count_spin = new QSpinBox(box);
	_count_spin->setRange(1, 100);
	// Head-less checks can shorten the series without clicking the spin box.
	const int count_override = test_env("DSVIEW_PPS_CALIB_COUNT").toInt();
	_count_spin->setValue(count_override > 0 ? count_override : 10);
	form->addRow(QString::fromUtf8("采集次数"), _count_spin);

	// Fixed by design: the window has to contain at least two edges of a 1PPS
	// input. A frame that catches only one edge is repeated, so the requested
	// number of captures is always averaged from two-edge frames.
	_window_label = new QLabel(QString::fromUtf8("1200 ms（100 MHz，1 通道，触发 0%）"), box);
	form->addRow(QString::fromUtf8("采集窗口"), _window_label);

	_state_label = new QLabel(QString::fromUtf8(
		"输入可以是 10ms 或 1PPS，自动识别。点“开始”后请让信号保持接入。"), box);
	_state_label->setWordWrap(true);
	form->addRow(QString(), _state_label);

	root->addWidget(box);

	_log = new QTextEdit(this);
	_log->setReadOnly(true);
	_log->setMinimumHeight(110);
	root->addWidget(_log);

	QHBoxLayout *buttons = new QHBoxLayout();
	_start_btn = new QPushButton(QString::fromUtf8("开始"), this);
	_stop_btn = new QPushButton(QString::fromUtf8("停止"), this);
	_stop_btn->setEnabled(false);
	QDialogButtonBox *box_buttons = new QDialogButtonBox(
		QDialogButtonBox::Close, this);
	buttons->addWidget(_start_btn);
	buttons->addWidget(_stop_btn);
	buttons->addStretch(1);
	buttons->addWidget(box_buttons);
	root->addLayout(buttons);

	connect(_start_btn, &QPushButton::clicked, this, &PpsCalibDlg::on_start);
	connect(_stop_btn, &QPushButton::clicked, this, &PpsCalibDlg::on_stop);
	connect(box_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

	_calib = new pps::PpsCalibration(_session, this);
	connect(_calib, &pps::PpsCalibration::progress,
			this, &PpsCalibDlg::on_progress);
	connect(_calib, &pps::PpsCalibration::finished,
			this, &PpsCalibDlg::on_finished);
	connect(_calib, &pps::PpsCalibration::failed,
			this, &PpsCalibDlg::on_failed);
	connect(_calib, &pps::PpsCalibration::status_message,
			this, &PpsCalibDlg::on_status);

	resize(520, 380);
}

PpsCalibDlg::~PpsCalibDlg()
{
	if (_calib) {
		_calib->stop();
		_calib = nullptr;
	}
}

void PpsCalibDlg::append_log(const QString &line)
{
	_log->append(line);
	_log->moveCursor(QTextCursor::End);
}

void PpsCalibDlg::set_running(bool running)
{
	_channel_spin->setEnabled(!running);
	_count_spin->setEnabled(!running);
	_start_btn->setEnabled(!running);
	_stop_btn->setEnabled(running);
}

void PpsCalibDlg::on_start()
{
	_log->clear();
	_has_result = false;
	_factor = 1.0;

	pps::PpsCalibConfig cfg;
	cfg.channel = _channel_spin->value();
	cfg.captures = _count_spin->value();
	cfg.window_ms = 1200.0;
	cfg.sample_rate = 100000000ULL;

	set_running(true);
	if (!_calib->start(cfg)) {
		set_running(false);
		append_log(_calib->error());
		_state_label->setText(QString::fromUtf8("校准未能开始：") + _calib->error());
	}
}

void PpsCalibDlg::on_stop()
{
	if (_calib)
		_calib->stop();
	set_running(false);
	_state_label->setText(QString::fromUtf8("已停止。"));
}

void PpsCalibDlg::on_progress(int done, int total, double measured_ms,
		int edges)
{
	if (measured_ms > 0.0) {
		append_log(QString::fromUtf8("第 %1/%2 次：%3 个沿，实测周期 %4 ms")
			.arg(done).arg(total).arg(edges)
			.arg(measured_ms, 0, 'f', 6));
	} else {
		append_log(QString::fromUtf8("第 %1/%2 次：只看到 %3 个沿，本次重测")
			.arg(done).arg(total).arg(edges));
	}
	_state_label->setText(QString::fromUtf8("采集中 %1/%2 ...").arg(done).arg(total));
}

void PpsCalibDlg::on_finished(const pps::PpsCalibResult &result)
{
	set_running(false);
	_factor = result.factor;
	_has_result = true;

	append_log(QString::fromUtf8(
		"识别为 %1 输入：标准 %2 ms，实测平均 %3 ms，校准系数 = %4")
		.arg(result.input_name)
		.arg(result.standard_ms, 0, 'f', 3)
		.arg(result.measured_ms, 0, 'f', 6)
		.arg(result.factor, 0, 'f', 9));
	// State what was really captured. The stream path now keeps the configured
	// window; it is only widened (once) when a frame really came back with a
	// single edge, so the number here still tells the two cases apart.
	append_log(QString::fromUtf8(
		"实际采集：窗口 %1 ms（%2），%3 MSa/s，%4 次")
		.arg(result.window_ms, 0, 'f', 0)
		.arg(result.stream_mode ? QString::fromUtf8("Stream 模式")
			: QString::fromUtf8("Buffer 模式"))
		.arg(result.sample_rate / 1000000.0, 0, 'f', 0)
		.arg(result.captures));
	_state_label->setText(QString::fromUtf8("校准完成：系数 %1 已填入配置。")
		.arg(result.factor, 0, 'f', 9));
}

void PpsCalibDlg::on_failed(const QString &message)
{
	set_running(false);
	append_log(message);
	_state_label->setText(message);
}

void PpsCalibDlg::on_status(const QString &message)
{
	append_log(message);
}

} // namespace dialogs
} // namespace pv
