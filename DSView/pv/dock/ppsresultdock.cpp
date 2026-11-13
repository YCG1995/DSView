/*
 * Result dock for the 1PPS measurement mode — implementation.
 */
#include "ppsresultdock.h"
#include "../pps/ppsmanager.h"
#include "../config/appconfig.h"
#include "../log.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QDesktopServices>
#include <QFileInfo>
#include <QHeaderView>
#include <QMap>
#include <QBrush>
#include <QColor>
#include <QDir>
#include <QIcon>
#include <QPainter>
#include <QPixmap>
#include <QProcess>
#include <QSize>
#include <QSizePolicy>
#include <QSignalBlocker>
#include <QTimer>
#include <QCoreApplication>
#include <QUrl>
#include <algorithm>

// Qt's automatic "disabled" rendering of an SVG icon is barely visible on this
// dark theme - a disabled button looks enabled. Render a faded copy instead, so
// "校准" clearly greys out while a measurement is running.
static QIcon faded_icon(const QIcon &src) 
{
	QPixmap pm(22, 22);
	pm.fill(Qt::transparent);
	QPainter p(&pm);
	p.setOpacity(0.28);
	src.paint(&p, QRect(0, 0, 22, 22));
	return QIcon(pm);
}

namespace pv {
namespace dock {

// Eight channel rows per column: a 16-channel run then shows two columns side
// by side instead of one long table that needs scrolling.
static const int kRowsPerColumn = 8;

QToolButton *PpsResultDock::make_button(QToolButton *btn, const QString &icon,
		const QString &tip)
{
	btn->setToolButtonStyle(Qt::ToolButtonIconOnly);
	btn->setAutoRaise(true);
	btn->setIconSize(QSize(22, 22));
	btn->setToolTip(tip);
	btn->setIcon(QIcon(_icon_path + icon));
	return btn;
}

void PpsResultDock::apply_icons()
{
	const QString path = GetIconPath();
	if (path == _icon_path)
		return;
	_icon_path = path;

	_config_btn->setIcon(QIcon(_icon_path + "/gear.svg"));
	_start_btn->setIcon(QIcon(_icon_path + "/start.svg"));
	_stop_btn->setIcon(QIcon(_icon_path + "/stop.svg"));
	_calib_btn->setIcon(QIcon(_icon_path + "/params.svg"));
	_wave_btn->setIcon(QIcon(_icon_path + "/osc.svg"));
}

PpsResultDock::PpsResultDock(pps::PpsManager *manager, QWidget *parent)
	: QScrollArea(parent)
	, _manager(manager)
	, _icon_path()
{
	_host = new QWidget(this);
	setWidget(_host);
	setWidgetResizable(true);

	QVBoxLayout *layout = new QVBoxLayout(_host);

	// The whole PPS command set sits here as icons: the toolbar button only
	// toggles this window, so stopping or reconfiguring never means leaving it.
	QHBoxLayout *buttons = new QHBoxLayout();
	buttons->setSpacing(6);
	_config_btn = make_button(new QToolButton(_host), "/gear.svg",
		QString::fromUtf8("配置…（采集参数、参考通道、通道边沿、校准系数）"));
	_start_btn = make_button(new QToolButton(_host), "/start.svg",
		QString::fromUtf8("开始测量"));
	_stop_btn = make_button(new QToolButton(_host), "/stop.svg",
		QString::fromUtf8("停止测量"));
	_calib_btn = make_button(new QToolButton(_host), "/params.svg",
		QString::fromUtf8("校准…（用标准 1PPS 或 10 ms 输入自动算校准系数）"));
	_wave_btn = make_button(new QToolButton(_host), "/osc.svg",
		QString::fromUtf8("打开独立波形窗口，看每帧延时随时间的变化"));
	_config_btn->setObjectName("ppsConfigButton");
	_start_btn->setObjectName("ppsStartButton");
	_stop_btn->setObjectName("ppsStopButton");
	_calib_btn->setObjectName("ppsCalibButton");
	_wave_btn->setObjectName("ppsWaveButton");
	buttons->addWidget(_config_btn);
	buttons->addWidget(_start_btn);
	buttons->addWidget(_stop_btn);

	// "自动保存波形" sits right of 停止 so it can be flipped while a measurement
	// is running: every trigger then writes its raw frame as a DSL file. It used
	// to be the "无条件保存" tick of the configuration dialog, which could only be
	// changed before a run started.
	_autosave_chk = new QCheckBox(QString::fromUtf8("自动保存波形"), _host);
	_autosave_chk->setObjectName("ppsAutosaveCheck");
	_autosave_chk->setToolTip(QString::fromUtf8(
		"勾选后每一次触发都把原始帧保存成 DSL 文件（每个 CSV 旁边一个同名文件夹）。"
		"测量运行中也可以随时切换。"));
	_autosave_chk->setChecked(_manager->dsl_save_always());
	buttons->addSpacing(8);
	buttons->addWidget(_autosave_chk);

	buttons->addSpacing(12);
	buttons->addWidget(_calib_btn);
	buttons->addWidget(_wave_btn);
	buttons->addStretch(1);
	layout->addLayout(buttons);

	// One line instead of three stacked labels: frames, round-trip time and the
	// CSV file name, with the button that opens the result next to it. The full
	// path and the capture/report split live in the tooltip, where they do not
	// push the table off the dock.
	QHBoxLayout *info_row = new QHBoxLayout();
	info_row->setSpacing(8);
	_info_label = new QLabel(_host);
	_info_label->setObjectName("ppsCsvLabel");
	_info_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
	_info_label->setWordWrap(false);
	_info_label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
	_info_label->setText(QString::fromUtf8("帧数:0  耗时:—  测试时长:00:00"));
	info_row->addWidget(_info_label, 1);

	_open_btn = new QPushButton(QString::fromUtf8("打开文件"), _host);
	_open_btn->setObjectName("ppsOpenFileButton");
	_open_btn->setToolTip(QString::fromUtf8(
		"打开结果 CSV 所在文件夹，并在其中选中该文件"));
	_open_btn->setEnabled(false);
	info_row->addWidget(_open_btn);
	layout->addLayout(info_row);

	_table = new QTableWidget(_host);
	_table->setColumnCount(7);
	_table->setHorizontalHeaderLabels(QStringList()
		<< QString::fromUtf8("通道") << QString::fromUtf8("当前值(ns)")
		<< QString::fromUtf8("均值(ns)") << QString::fromUtf8("最小值(ns)")
		<< QString::fromUtf8("最大值(ns)") << QString::fromUtf8("标准差(ns)")
		<< QString::fromUtf8("次数"));
	_table->verticalHeader()->setVisible(false);
	_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
	_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
	// Keep the table at the height its rows need: it used to swallow every
	// free pixel of the dock, which pushed the CSV path below the screen edge.
	_table->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
	_table->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
	layout->addWidget(_table);

	// Where the current run is being written: asked for explicitly so the CSV
	// can be found while a measurement is still running. It sits above the table
	// because below it the dock's height cut it off the screen.
	layout->addStretch(1);

	connect(_manager, &pps::PpsManager::frame_updated,
		this, &PpsResultDock::refresh);
	connect(_config_btn, &QToolButton::clicked,
		this, &PpsResultDock::config_requested);
	connect(_start_btn, &QToolButton::clicked,
		this, &PpsResultDock::start_requested);
	connect(_stop_btn, &QToolButton::clicked,
		this, &PpsResultDock::stop_requested);
	connect(_calib_btn, &QToolButton::clicked,
		this, &PpsResultDock::calib_requested);
	connect(_wave_btn, &QToolButton::clicked,
		this, &PpsResultDock::wave_requested);
	connect(_open_btn, &QPushButton::clicked,
		this, &PpsResultDock::on_open_file);
	connect(_autosave_chk, &QCheckBox::toggled, this, [this](bool on) {
		// Applied to the running measurement immediately: the tick decides
		// whether the next trigger writes a DSL, not whether the run was started
		// with it.
		_manager->set_dsl_save_always(on);
	});
	// "测试时长" has to run even when no frame arrives (a 1 Hz reference means
	// one frame per second), so the label is refreshed on its own clock.
	_run_tick = new QTimer(this);
	_run_tick->setInterval(1000);
	connect(_run_tick, &QTimer::timeout, this, [this]() {
		if (_manager->is_running())
			update_info_line();
	});
	_run_tick->start();
	refresh();
}

static QString fmt(double v)
{
	// One decimal place, matching CSV and MCP output.
	QString s = QString::number(v, 'f', 1);
	if (s == "-0.0")
		s = "0.0";
	return s;
}

// Run duration, readable at a glance: mm:ss, or h:mm:ss once it passes an hour.
static QString fmtDuration(qint64 ms)
{
	if (ms < 0)
		ms = 0;
	const qint64 total_s = ms / 1000;
	const qint64 h = total_s / 3600;
	const qint64 m = (total_s % 3600) / 60;
	const qint64 s = total_s % 60;
	if (h > 0)
		return QString("%1:%2:%3").arg(h)
			.arg(m, 2, 10, QChar('0')).arg(s, 2, 10, QChar('0'));
	return QString("%1:%2").arg(m, 2, 10, QChar('0'))
		.arg(s, 2, 10, QChar('0'));
}

void PpsResultDock::update_info_line()
{
	// Single summary line. The frame count and the round-trip time are plain
	// numbers here; the breakdown (capture vs. report), the run duration and the
	// absolute path are in the tooltip, so the line stays short enough to keep
	// the file name readable next to the "打开文件" button.
	const int frames = _manager->frame_count();
	const QString csv = _manager->csv_path();
	const QString file_name = csv.isEmpty()
		? QString::fromUtf8("—") : QFileInfo(csv).fileName();
	QString cost = QString::fromUtf8("—");
	QString breakdown = QString::fromUtf8("尚未开始测量。");
	if (frames > 0) {
		// Whole milliseconds: a per-frame breakdown to 0.1 ms in the summary line
		// was noise, the tooltip keeps the exact split.
		cost = QString::number(qRound(_manager->last_total_ms())) + " ms";
		breakdown = QString::fromUtf8(
			"采集 %1 ms + 上报 %2 ms = 共 %3 ms")
			.arg(fmt(_manager->last_capture_ms()))
			.arg(fmt(_manager->last_report_ms()))
			.arg(fmt(_manager->last_total_ms()));
	}
	// 测试时长: wall time since 开始测量, frozen once the run stops.
	const QString elapsed = fmtDuration(_manager->run_elapsed_ms());
	const QString full = QString::fromUtf8("帧数:%1  耗时:%2  测试时长:%3  %4")
		.arg(frames).arg(cost).arg(elapsed).arg(file_name);
	const int room = std::max(220, _host->width() - 40);
	QString info = _info_label->fontMetrics().elidedText(
			full, Qt::ElideMiddle, room);
	if (info != _info_shown) {
		_info_shown = info;
		_info_label->setText(info);
	}
	QString tip = breakdown;
	tip += QString::fromUtf8("\n测试时长：%1").arg(elapsed);
	if (!csv.isEmpty())
		tip += QString::fromUtf8("\n结果文件：%1").arg(csv);
	_info_label->setToolTip(tip);
}

void PpsResultDock::refresh()
{
	// Follow a theme switch without needing a separate hook.
	apply_icons();

	const auto &cfg = _manager->config();
	const auto &last = _manager->last_results();
	const auto stats = _manager->statistics();

	QMap<int, pps::PpsResult> last_map;
	for (const pps::PpsResult &r : last)
		last_map.insert(r.channel_index, r);
	QMap<int, pps::PpsStats> stats_map;
	for (const pps::PpsStats &s : stats)
		stats_map.insert(s.channel_index, s);

	// Single summary line (frames / 耗时 / 测试时长 / file name).
	update_info_line();
	const QString csv = _manager->csv_path();
	const QString file_name = csv.isEmpty()
		? QString::fromUtf8("—") : QFileInfo(csv).fileName();
	// Nothing to reveal before the first frame has opened a file.
	_open_btn->setEnabled(!csv.isEmpty());
	_open_btn->setToolTip(csv.isEmpty()
		? QString::fromUtf8("还没有结果文件，开始测量后可用")
		: QString::fromUtf8(
			"打开结果所在文件夹并选中：%1").arg(file_name));
	// The tick is the single source of truth for 无条件保存; the dialog no
	// longer edits it, so it can only drift if something else sets the config.
	{
		const QSignalBlocker block(_autosave_chk);
		_autosave_chk->setChecked(_manager->dsl_save_always());
	}
	const bool running = _manager->is_running();
	_start_btn->setEnabled(!running);
	_stop_btn->setEnabled(running);
	// Calibration takes the device for itself, so it is not offered while a
	// measurement is running: stop first, then calibrate.
	_calib_btn->setEnabled(!running);
	_calib_btn->setIcon(running
		? faded_icon(QIcon(_icon_path + "/params.svg"))
		: QIcon(_icon_path + "/params.svg"));
	_calib_btn->setToolTip(running
		? QString::fromUtf8("测量运行中不可校准，请先停止测量")
		: QString::fromUtf8("校准…"));

	_table->setRowCount(static_cast<int>(cfg.channels.size()));

	int row = 0;
	for (const pps::PpsChannel &ch : cfg.channels) {
		const int index = ch.channel_index;
		_table->setItem(row, 0, new QTableWidgetItem(
			"CH" + QString::number(index)));

		if (last_map.contains(index) && last_map[index].valid) {
			_table->setItem(row, 1, new QTableWidgetItem(
				fmt(last_map[index].delta_ns)));
		} else if (_manager->frame_count() > 0 && last_map.contains(index)) {
			// A blank cell reads like a bug. Say what actually happened: the
			// wanted edge is not inside the captured window around the
			// reference edge (the window is only capture_time wide).
			QTableWidgetItem *item = new QTableWidgetItem(
				QString::fromUtf8("无沿（窗口内）"));
			item->setToolTip(QString::fromUtf8(
				"参考沿前后各半个采集时间的窗口内没有该方向的边沿。"
				"把测量边沿改成与脉冲同一侧，或缩短与参考沿的距离。"));
			item->setForeground(QBrush(QColor(128, 128, 128)));
			_table->setItem(row, 1, item);
		} else {
			_table->setItem(row, 1, new QTableWidgetItem(""));
		}

		if (stats_map.contains(index) && stats_map[index].count > 0) {
			const pps::PpsStats &s = stats_map[index];
			_table->setItem(row, 2, new QTableWidgetItem(fmt(s.mean)));
			_table->setItem(row, 3, new QTableWidgetItem(fmt(s.minimum)));
			_table->setItem(row, 4, new QTableWidgetItem(fmt(s.maximum)));
			_table->setItem(row, 5, new QTableWidgetItem(fmt(s.stddev)));
			_table->setItem(row, 6, new QTableWidgetItem(QString::number(s.count)));
		} else {
			for (int c = 2; c <= 6; ++c)
				_table->setItem(row, c, new QTableWidgetItem(""));
		}
		++row;
	}

	// Size the table to exactly the rows it holds, so the CSV path below it
	// stays on screen instead of being pushed to the dock's last pixel.
	_table->resizeRowsToContents();
	int table_h = _table->horizontalHeader()->sizeHint().height()
		+ 2 * _table->frameWidth();
	for (int r = 0; r < _table->rowCount(); ++r)
		table_h += _table->rowHeight(r);
	table_h += 2 * _table->frameWidth();
	_table->setFixedHeight(table_h);
}

void PpsResultDock::on_open_file()
{
	const QString csv = _manager->csv_path();

#ifdef Q_OS_WIN
	// The CSV only appears on disk once the first frame is written, but the
	// button is enabled as soon as the run has a path. "explorer.exe
	// /select,<missing file>" does not fail loudly: Explorer drops the /select
	// argument and opens its default location (文档), which is what made the
	// button look like it ignored the autosave folder. So resolve the deepest
	// directory that really exists and open that instead.
	QString file;
	QString target;
	if (!csv.isEmpty()) {
		file = QFileInfo(csv).absoluteFilePath();
		target = QFileInfo(file).absolutePath();
		while (!target.isEmpty() && !QFileInfo(target).isDir()) {
			const QString up = QFileInfo(target).absolutePath();
			if (up == target)
				break;
			target = up;
		}
	}
	if (target.isEmpty() || !QFileInfo(target).isDir()) {
		// No run yet: fall back to the configured save directory, then to the
		// application's own folder, so the button always opens something real.
		target = _manager->config().save_dir;
		if (target.isEmpty() || !QFileInfo(target).isDir())
			target = QCoreApplication::applicationDirPath();
	}

	if (!file.isEmpty() && QFileInfo(file).isFile()) {
		dsv_info("1PPS open file: %s",
			QDir::toNativeSeparators(file).toUtf8().constData());
		QProcess::startDetached("explorer.exe",
			QStringList() << (QString("/select,") +
				QDir::toNativeSeparators(file)));
	} else {
		// File not written yet: show the folder it will appear in.
		dsv_info("1PPS open file: %s (not created yet), opening its folder",
			QDir::toNativeSeparators(file).toUtf8().constData());
		QProcess::startDetached("explorer.exe",
			QStringList() << QDir::toNativeSeparators(target));
	}
#else
	const QString dir = csv.isEmpty()
		? _manager->config().save_dir : QFileInfo(csv).absolutePath();
	QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
#endif
}

} // namespace dock
} // namespace pv
