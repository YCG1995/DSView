/*
 * Result dock for the 1PPS measurement mode — implementation.
 */
#include "ppsresultdock.h"
#include "../pps/ppsmanager.h"
#include "../config/appconfig.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QMap>
#include <QBrush>
#include <QColor>
#include <QIcon>
#include <QPainter>
#include <QPixmap>
#include <QSize>
#include <QSizePolicy>
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
	buttons->addSpacing(12);
	buttons->addWidget(_calib_btn);
	buttons->addWidget(_wave_btn);
	buttons->addStretch(1);
	layout->addLayout(buttons);

	_frame_label = new QLabel(QString::fromUtf8("帧数: 0"), _host);
	layout->addWidget(_frame_label);

	// How long one measurement round trip takes: the capture window plus the
	// time the samples need to come over USB and be decoded.
	_timing_label = new QLabel(QString::fromUtf8("本次耗时: —"), _host);
	_timing_label->setToolTip(QString::fromUtf8(
		"采集 = 采集时间窗口；上报 = 从触发到数据搬完并解码完成的时间。"));
	layout->addWidget(_timing_label);

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
	// can be found while a measurement is still running.
	_csv_label = new QLabel(_host);
	_csv_label->setObjectName("ppsCsvLabel");
	_csv_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
	_csv_label->setWordWrap(false);
	_csv_label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
	_csv_label->setText(QString::fromUtf8("正在保存的 CSV: —"));
	layout->addWidget(_csv_label);
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

	_frame_label->setText(QString::fromUtf8("帧数: ")
		+ QString::number(_manager->frame_count()));
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

	if (_manager->frame_count() > 0) {
		_timing_label->setText(QString::fromUtf8(
			"本次耗时: 采集 %1 ms + 上报 %2 ms = 共 %3 ms")
			.arg(fmt(_manager->last_capture_ms()))
			.arg(fmt(_manager->last_report_ms()))
			.arg(fmt(_manager->last_total_ms())));
	} else {
		_timing_label->setText(QString::fromUtf8("本次耗时: —"));
	}

	// Path of the CSV being written. A run can be several minutes long, so the
	// file is created up front: printing the path here makes it reachable
	// without waiting for the measurement to stop.
	const QString csv = _manager->csv_path();
	QString csv_line;
	if (csv.isEmpty()) {
		csv_line = QString::fromUtf8("正在保存的 CSV: —");
	} else {
		// Elide the middle, not the tail: the file name stays readable.
		const int room = std::max(220, _host->width() - 40);
		const QString elided = _csv_label->fontMetrics().elidedText(
				csv, Qt::ElideMiddle, room);
		csv_line = QString::fromUtf8("正在保存的 CSV: %1").arg(elided);
		_csv_label->setToolTip(csv);
	}
	if (csv_line != _csv_shown) {
		_csv_shown = csv_line;
		_csv_label->setText(csv_line);
	}

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

} // namespace dock
} // namespace pv
