/*
 * Configuration dialog for the 1PPS measurement mode — implementation.
 * All options are derived from the active logic analyzer, so the dialog works
 * for every supported model and channel mode instead of one fixed device.
 */
#include "ppsconfigdlg.h"

#include "../sigsession.h"
#include "../deviceagent.h"
#include "../log.h"

#include <libsigrok.h>
#include <glib.h>

#include <algorithm>
#include <cmath>

#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QMap>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollBar>
#include <QSizePolicy>
#include <QTimer>
#include <QVariant>
#include <QVBoxLayout>

namespace pv {
namespace dialogs {

// Test channels are laid out as two columns: CH0–CH7 and CH8–CH15.
static const int kColumnSpan = 8;

static QString rate_text(uint64_t rate)
{
	char *s = sr_samplerate_string(rate);
	const QString out = QString::fromLatin1(s ? s : "");
	if (s)
		g_free(s);
	return out;
}

static QString time_text(double ms)
{
	QString s = QString::number(ms, 'f', 3);
	if (s.contains('.')) {
		while (s.endsWith('0'))
			s.chop(1);
		if (s.endsWith('.'))
			s.chop(1);
	}
	return s + " ms";
}

PpsConfigDlg::PpsConfigDlg(SigSession *session, const pps::PpsConfig &cfg,
		QWidget *parent)
	: QDialog(parent)
	, _session(session)
	, _result(cfg)
	, _building(true)
{
	setWindowTitle(QString::fromUtf8("1PPS 测量配置"));

	pps::query_device_caps(_session, _caps);

	// Two columns: the reference-channel box sits above the capture-parameter
	// box on the left, the test-channel tables fill the right column.
	QGridLayout *root = new QGridLayout(this);
	root->setColumnStretch(1, 1);
	root->setRowStretch(2, 0);

	QGroupBox *ref_box = new QGroupBox(QString::fromUtf8("参考通道"), this);
	QFormLayout *ref_form = new QFormLayout(ref_box);
	ref_form->setLabelAlignment(Qt::AlignRight);

	_ref_combo = new QComboBox(ref_box);
	const int ch_num = std::max(1, _caps.channel_num);
	for (int i = 0; i < ch_num; ++i)
		_ref_combo->addItem("CH" + QString::number(i), i);
	ref_form->addRow(QString::fromUtf8("参考通道"), _ref_combo);

	// Edge selection is a two-state switch, not a dropdown: the dialog is
	// filled in with the mouse wheel all the time and a combo box silently
	// changes value while scrolling. Ticked = falling, unticked = rising.
	_ref_edge_check = new QCheckBox(QString::fromUtf8("下降沿（不勾选为上升沿）"),
		ref_box);
	ref_form->addRow(QString::fromUtf8("参考边沿"), _ref_edge_check);

	_pos_spin = new QSpinBox(ref_box);
	_pos_spin->setRange(0, 100);
	_pos_spin->setSuffix(" %");
	ref_form->addRow(QString::fromUtf8("触发位置"), _pos_spin);
	root->addWidget(ref_box, 0, 0, Qt::AlignTop);

	QGroupBox *cap_box = new QGroupBox(QString::fromUtf8("采集参数"), this);
	QFormLayout *cap_form = new QFormLayout(cap_box);
	cap_form->setLabelAlignment(Qt::AlignRight);

	// Channel range is a separate choice: the buffer modes of this device trade
	// channel count against samplerate, so CH0-CH7 reaches 1GHz while CH0-CH15
	// stops at 500MHz. Picking the range first makes the rate list honest.
	_range_combo = new QComboBox(cap_box);
	QList<int> counts;
	for (const pps::PpsModeInfo &m : _caps.modes) {
		if (m.channels > 0 && !counts.contains(m.channels))
			counts.append(m.channels);
	}
	if (counts.isEmpty() && _caps.channel_num > 0)
		counts.append(_caps.channel_num);
	std::sort(counts.begin(), counts.end());
	for (int n : counts) {
		uint64_t best = 0;
		for (const pps::PpsModeInfo &m : _caps.modes) {
			if (m.channels == n && m.max_rate > best)
				best = m.max_rate;
		}
		if (best == 0)
			best = _caps.max_rate;
		_range_combo->addItem(QString::fromUtf8("CH0–CH%1（%2 通道，最高 %3）")
			.arg(n - 1).arg(n).arg(rate_text(best)), n);
	}
	cap_form->addRow(QString::fromUtf8("通道范围"), _range_combo);

	_rate_combo = new QComboBox(cap_box);
	cap_form->addRow(QString::fromUtf8("采样率"), _rate_combo);

	_time_combo = new QComboBox(cap_box);
	cap_form->addRow(QString::fromUtf8("采集时间"), _time_combo);

	_dir_edit = new QLineEdit(cap_box);
	_browse_btn = new QPushButton(QString::fromUtf8("浏览..."), cap_box);
	QHBoxLayout *dir_layout = new QHBoxLayout();
	dir_layout->setContentsMargins(0, 0, 0, 0);
	dir_layout->addWidget(_dir_edit);
	dir_layout->addWidget(_browse_btn);
	cap_form->addRow(QString::fromUtf8("CSV 保存目录"), dir_layout);

	// One master switch for "keep the raw frame when a channel drops out"; the
	// per-channel tick lives in the channel table (column "保存").
	_auto_save_loss_check = new QCheckBox(
		QString::fromUtf8("数据丢失时自动保存 DSL 文件"), cap_box);
	cap_form->addRow(QString(), _auto_save_loss_check);

	// Calibration: the analyzer's own crystal is a few ppm off, so every
	// measured difference is multiplied by this factor (1.0 = raw).
	_calib_spin = new QDoubleSpinBox(cap_box);
	_calib_spin->setDecimals(6);
	_calib_spin->setRange(0.5, 2.0);
	_calib_spin->setSingleStep(0.000001);
	_calib_spin->setValue(_result.calib_factor);
	_calib_spin->setToolTip(QString::fromUtf8(
		"所有时间差都会乘以该系数；可用 PPS 菜单里的“校准”自动算出。"));
	cap_form->addRow(QString::fromUtf8("校准系数"), _calib_spin);
	root->addWidget(cap_box, 1, 0, Qt::AlignTop);

	QGroupBox *ch_box = new QGroupBox(QString::fromUtf8("测试通道"), this);
	QHBoxLayout *ch_layout = new QHBoxLayout(ch_box);

	// Two side-by-side columns with their own captions; each table grows to its
	// full content height so there is never a scrollbar, and both columns stay
	// pinned to the top of the group box instead of drifting with the window.
	auto make_column = [this, ch_box](const QString &caption,
		QTableWidget *&table, QWidget *&col) {
		col = new QWidget(ch_box);
		QVBoxLayout *v = new QVBoxLayout(col);
		v->setContentsMargins(0, 0, 0, 0);
		v->setSpacing(2);
		QLabel *label = new QLabel(caption, col);
		label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
		label->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
		v->addWidget(label);
		make_channel_table(table, col);
		v->addWidget(table);
		v->addStretch(1);
	};
	make_column(QString::fromUtf8("CH0–CH7"), _channel_table, _channel_col1);
	make_column(QString::fromUtf8("CH8–CH15"), _channel_table2, _channel_col2);
	ch_layout->addWidget(_channel_col1, 0, Qt::AlignTop);
	ch_layout->addWidget(_channel_col2, 0, Qt::AlignTop);
	ch_layout->addStretch(1);
	ch_layout->setAlignment(Qt::AlignTop);
	root->addWidget(ch_box, 0, 1, Qt::AlignTop);

	QDialogButtonBox *buttons = new QDialogButtonBox(this);
	QPushButton *ok = new QPushButton(QString::fromUtf8("确定"), buttons);
	QPushButton *cancel = new QPushButton(QString::fromUtf8("取消"), buttons);
	buttons->addButton(ok, QDialogButtonBox::AcceptRole);
	buttons->addButton(cancel, QDialogButtonBox::RejectRole);
	root->addWidget(buttons, 2, 0, 1, 2);
	root->setRowStretch(0, 0);
	root->setRowStretch(1, 0);
	// Spacer row: the dialog keeps its natural height instead of stretching the
	// channel tables when the window is enlarged.
	root->setRowStretch(2, 0);

	// Fill values from the incoming configuration.
	const int ref_index = _ref_combo->findData(_result.ref_channel);
	_ref_combo->setCurrentIndex(ref_index >= 0 ? ref_index : 0);
	_ref_edge_check->setChecked(_result.ref_edge == pps::PPS_EDGE_FALLING);
	_pos_spin->setValue(_result.trigger_pos_percent);
	_dir_edit->setText(_result.save_dir);
	_auto_save_loss_check->setChecked(_result.auto_save_loss);

	// Channel range: restore the pinned one, otherwise prefer the 8-channel
	// range because it is the one that reaches 1GHz.
	int range_index = _range_combo->findData(_result.mode_channels);
	if (range_index < 0)
		range_index = _range_combo->findData(8);
	if (range_index < 0)
		range_index = 0;
	_range_combo->setCurrentIndex(range_index);
	_result.mode_channels = _range_combo->itemData(range_index).toInt();

	build_rate_options();
	build_time_options();
	build_channel_table();
	_building = false;

	connect(_browse_btn, &QPushButton::clicked, this, &PpsConfigDlg::browse_dir);
	connect(_ref_combo, QOverload<int>::of(&QComboBox::currentIndexChanged),
		this, &PpsConfigDlg::on_reference_changed);
	connect(_range_combo, QOverload<int>::of(&QComboBox::currentIndexChanged),
		this, &PpsConfigDlg::on_range_changed);
	connect(_rate_combo, QOverload<int>::of(&QComboBox::currentIndexChanged),
		this, &PpsConfigDlg::on_rate_changed);
	connect(_time_combo, QOverload<int>::of(&QComboBox::currentIndexChanged),
		this, &PpsConfigDlg::on_time_changed);
	connect(_auto_save_loss_check, &QCheckBox::toggled,
		this, &PpsConfigDlg::on_auto_save_toggle);
	connect(ok, &QPushButton::clicked, this, &PpsConfigDlg::on_accept);
	connect(cancel, &QPushButton::clicked, this, &QDialog::reject);

	// The "no scrolling, two columns side by side" layout is a visual promise;
	// log the measured geometry once the layout has run so it can be checked from
	// the log instead of by eye.
	QTimer::singleShot(0, this, [this]() {
		dsv_info("1PPS dialog layout: %dx%d, left %d rows (%d px, scroll max %d), "
			"right %d rows (%d px, scroll max %d), right column visible %d",
			width(), height(),
			_channel_table->rowCount(), _channel_table->height(),
			_channel_table->verticalScrollBar()->maximum(),
			_channel_table2->rowCount(), _channel_table2->height(),
			_channel_table2->verticalScrollBar()->maximum(),
			_channel_col2->isVisible() ? 1 : 0);
	});
}

void PpsConfigDlg::make_channel_table(QTableWidget *&table, QWidget *parent)
{
	table = new QTableWidget(parent);
	table->setColumnCount(4);
	table->setHorizontalHeaderLabels(QStringList()
		<< QString::fromUtf8("启用")
		<< QString::fromUtf8("通道")
		<< QString::fromUtf8("边沿")
		<< QString::fromUtf8("保存"));
	table->verticalHeader()->setVisible(false);
	table->verticalHeader()->setDefaultSectionSize(26);
	// The caption row must be a constant height: a stretching header is what made
	// the table look like it zoomed together with the window.
	QHeaderView *hh = table->horizontalHeader();
	hh->setSectionResizeMode(QHeaderView::Stretch);
	hh->setFixedHeight(24);
	hh->setMinimumSectionSize(34);
	// The whole point of the two columns is that nothing needs scrolling: the
	// table is sized to its content instead of showing eight rows at a time.
	table->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	table->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	table->setSelectionMode(QAbstractItemView::NoSelection);
	table->setMinimumWidth(292);
	table->setRowCount(0);
}

void PpsConfigDlg::fit_channel_table(QTableWidget *table)
{
	const int rows = table->rowCount();
	const int header_h = table->horizontalHeader()->sizeHint().height();
	const int row_h = table->verticalHeader()->defaultSectionSize();
	table->setFixedHeight(header_h + row_h * rows + 2 * table->frameWidth() + 2);
}

uint64_t PpsConfigDlg::max_rate_for_range(int channels) const
{
	uint64_t best = 0;
	for (const pps::PpsModeInfo &m : _caps.modes) {
		if (channels > 0 && m.channels == channels && m.max_rate > best)
			best = m.max_rate;
	}
	return best != 0 ? best : _caps.max_rate;
}

int PpsConfigDlg::range_for_rate(uint64_t rate) const
{
	// Widest channel count that still reaches the rate: 1GHz -> 8 channels on
	// the U2Pro16, 500MHz -> 16 channels.
	int best = 0;
	for (const pps::PpsModeInfo &m : _caps.modes) {
		if (m.max_rate >= rate && m.channels > best)
			best = m.channels;
	}
	return best;
}

void PpsConfigDlg::build_rate_options()
{
	_building = true;
	_rate_combo->clear();

	// Every rate the device can reach is listed; the channel range is what gets
	// adjusted to make the chosen rate legal. That keeps the two controls
	// equivalent: picking 1GHz here moves the range to CH0-CH7 exactly as if the
	// range had been picked first.
	bool any = false;
	for (uint64_t r : _caps.rates) {
		_rate_combo->addItem(rate_text(r), QVariant::fromValue(r));
		any = true;
	}
	if (!any && _caps.max_rate != 0)
		_rate_combo->addItem(rate_text(_caps.max_rate),
			QVariant::fromValue(_caps.max_rate));

	int index = _rate_combo->findData(QVariant::fromValue(_result.sample_rate));
	if (index < 0 && !_caps.rates.empty())
		index = _rate_combo->count() - 1; // highest rate by default
	if (index >= 0) {
		_rate_combo->setCurrentIndex(index);
		_result.sample_rate =
			_rate_combo->itemData(index).value<uint64_t>();
	}

	sync_range_to_rate();
	_building = false;
	const QString top = _rate_combo->count() > 0
		? _rate_combo->itemText(_rate_combo->count() - 1) : "—";
	dsv_info("1PPS dialog: %d samplerate(s) listed (max %s), range CH0-CH%d, "
		"selected %" G_GUINT64_FORMAT " Hz", _rate_combo->count(),
		top.toUtf8().constData(), _result.mode_channels - 1,
		(uint64_t)_result.sample_rate);
}

void PpsConfigDlg::sync_range_to_rate()
{
	// A rate the selected range cannot reach wins: the range shrinks to the
	// widest one that still supports the rate. A rate the range supports leaves
	// the range untouched, so 500MHz with CH0-CH15 stays on 16 channels.
	if (_caps.modes.empty() || _result.sample_rate == 0)
		return;
	int range = _result.mode_channels;
	int wanted = range_for_rate(_result.sample_rate);
	if (wanted <= 0)
		return;
	if (range > 0 && max_rate_for_range(range) >= _result.sample_rate)
		return;
	const int at = _range_combo->findData(wanted);
	if (at < 0)
		return;
	_range_combo->setCurrentIndex(at);
	_result.mode_channels = wanted;
	dsv_info("1PPS dialog: rate %" G_GUINT64_FORMAT " Hz needs CH0-CH%d, "
		"range switched from CH0-CH%d",
		(uint64_t)_result.sample_rate, wanted - 1, range - 1);
}


void PpsConfigDlg::build_time_options()
{
	_building = true;
	const uint64_t rate = _rate_combo->currentData().value<uint64_t>();
	int channels = _caps.channels_for_rate(rate);
	if (_result.mode_channels > 0 && channels > _result.mode_channels)
		channels = _result.mode_channels;
	double max_ms = _caps.max_capture_ms(rate, channels);
	// The measurement is a burst per 1PPS second: keep the window well under
	// 500 ms so the upload and the next trigger always fit.
	if (max_ms <= 0.0 || max_ms > 500.0)
		max_ms = 500.0;

	QList<double> values;
	for (double decade = 0.001; decade <= 1000.0; decade *= 10.0) {
		for (int m : {1, 2, 5}) {
			const double v = m * decade;
			if (v <= max_ms)
				values.append(v);
		}
	}
	if (values.isEmpty())
		values.append(max_ms);

	std::sort(values.begin(), values.end());
	values.erase(std::unique(values.begin(), values.end()), values.end());
	std::reverse(values.begin(), values.end());

	_time_combo->clear();
	for (double v : values)
		_time_combo->addItem(time_text(v), v);

	int index = -1;
	for (int i = 0; i < _time_combo->count(); ++i) {
		if (std::fabs(_time_combo->itemData(i).toDouble()
				- _result.capture_time_ms) < 1e-6) {
			index = i;
			break;
		}
	}
	if (index < 0) {
		// Prefer the 10 ms default when it fits, else the closest smaller one.
		const int def = _time_combo->findData(QVariant::fromValue(10.0));
		if (def >= 0)
			index = def;
		else
			index = 0;
	}
	_time_combo->setCurrentIndex(index);
	_result.capture_time_ms = _time_combo->itemData(index).toDouble();
	_building = false;
}

void PpsConfigDlg::build_channel_table()
{
	_building = true;
	int ref = _ref_combo->currentData().toInt();
	const uint64_t rate = _rate_combo->currentData().value<uint64_t>();
	int usable = _caps.channels_for_rate(rate);
	if (_result.mode_channels > 0 && usable > _result.mode_channels)
		usable = _result.mode_channels;

	// The reference has to live inside the selected range: with CH0-CH7 only the
	// first eight channels exist, so CH9 can no longer be the reference.
	if (usable > 0 && _ref_combo->count() != usable) {
		const int keep = (ref >= 0 && ref < usable) ? ref : 0;
		_ref_combo->clear();
		for (int i = 0; i < usable; ++i)
			_ref_combo->addItem("CH" + QString::number(i), i);
		const int at = _ref_combo->findData(keep);
		_ref_combo->setCurrentIndex(at >= 0 ? at : 0);
		ref = _ref_combo->currentData().toInt();
	}
	_result.ref_channel = ref;

	// Keep the user's per-channel choices when rebuilding.
	QMap<int, pps::PpsChannel> previous;
	for (const pps::PpsChannel &ch : _result.channels)
		previous.insert(ch.channel_index, ch);
	QMap<int, bool> disabled;
	for (int idx : _result.disabled_channels)
		disabled.insert(idx, true);

	_channel_table->setRowCount(0);
	_channel_table2->setRowCount(0);
	int left_rows = 0, right_rows = 0;
	for (int i = 0; i < usable; ++i) {
		// The reference channel drives the trigger and is not measured.
		if (i == ref)
			continue;
		// Channels are split by their own number, not by row position, so the
		// captions CH0–CH7 / CH8–CH15 stay truthful even when the reference
		// channel is one of the first eight.
		QTableWidget *table = (i < kColumnSpan)
			? _channel_table : _channel_table2;
		const int table_row = (i < kColumnSpan)
			? left_rows++ : right_rows++;

		table->insertRow(table_row);

		QTableWidgetItem *enable_item =
			new QTableWidgetItem();
		enable_item->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled);
		const bool on = previous.contains(i) ? previous[i].enabled
			: !disabled.contains(i);
		enable_item->setCheckState(on ? Qt::Checked : Qt::Unchecked);
		table->setItem(table_row, 0, enable_item);

		QTableWidgetItem *name_item =
			new QTableWidgetItem("CH" + QString::number(i));
		name_item->setFlags(Qt::ItemIsEnabled);
		table->setItem(table_row, 1, name_item);

		// Edge is a switch as well, so scrolling the table can never change it.
		// Ticked = falling; unticked = rising (the default).
		QCheckBox *edge_check = new QCheckBox(QString::fromUtf8("下降沿"),
			table);
		int edge = pps::PPS_EDGE_RISING;
		if (previous.contains(i))
			edge = previous[i].edge;
		edge_check->setChecked(edge == pps::PPS_EDGE_FALLING);
		table->setCellWidget(table_row, 2, edge_check);

		// Per-channel "keep the raw frame when this channel drops out".
		QCheckBox *save_check = new QCheckBox(QString::fromUtf8("保存"), table);
		bool save_on_loss = previous.contains(i) ? previous[i].save_on_loss
			: _result.auto_save_loss;
		save_check->setChecked(save_on_loss);
		save_check->setEnabled(_auto_save_loss_check->isChecked());
		table->setCellWidget(table_row, 3, save_check);
	}
	_channel_col2->setVisible(right_rows > 0);
	fit_channel_table(_channel_table);
	fit_channel_table(_channel_table2);
	_building = false;
	if (isVisible())
		adjustSize();

	// Log the tick state so the "is my channel selection remembered" question
	// can be answered from the log without opening the dialog by hand.
	QString off_list, rising_list;
	int off_count = 0;
	for (QTableWidget *t : { _channel_table, _channel_table2 }) {
		for (int r = 0; r < t->rowCount(); ++r) {
			QTableWidgetItem *it = t->item(r, 0);
			QTableWidgetItem *name = t->item(r, 1);
			if (it == nullptr || name == nullptr)
				continue;
			if (it->checkState() != Qt::Checked) {
				if (!off_list.isEmpty())
					off_list += ",";
				off_list += name->text();
				++off_count;
			}
			QCheckBox *edge_check = qobject_cast<QCheckBox *>(
				t->cellWidget(r, 2));
			if (edge_check && !edge_check->isChecked()) {
				if (!rising_list.isEmpty())
					rising_list += ",";
				rising_list += name->text();
			}
		}
	}
	dsv_info("1PPS dialog: range CH0-CH%d (%d ch), ref CH%d %s, rows %d+%d, "
		"unticked %d [%s], rising-edge [%s]",
		_result.mode_channels - 1, _result.mode_channels, ref,
		_ref_edge_check->isChecked() ? "falling" : "rising",
		_channel_table->rowCount(), _channel_table2->rowCount(), off_count,
		off_list.toUtf8().constData(), rising_list.toUtf8().constData());
}

void PpsConfigDlg::on_range_changed()
{
	if (_building)
		return;
	_result.mode_channels = _range_combo->currentData().toInt();
	// The opposite direction of sync_range_to_rate(): a narrower range cannot
	// keep a rate it does not reach, so the rate drops to the range maximum.
	const uint64_t max_rate = max_rate_for_range(_result.mode_channels);
	if (max_rate != 0 && _result.sample_rate > max_rate) {
		_result.sample_rate = max_rate;
		const int at = _rate_combo->findData(QVariant::fromValue(max_rate));
		if (at >= 0) {
			_building = true;
			_rate_combo->setCurrentIndex(at);
			_building = false;
		}
		dsv_info("1PPS dialog: range CH0-CH%d tops out at %" G_GUINT64_FORMAT
			" Hz, rate lowered", _result.mode_channels - 1,
			(uint64_t)max_rate);
	}
	build_time_options();
	build_channel_table();
}

void PpsConfigDlg::on_reference_changed()
{
	if (_building)
		return;
	build_channel_table();
}

void PpsConfigDlg::on_rate_changed()
{
	if (_building)
		return;
	_result.sample_rate = _rate_combo->currentData().value<uint64_t>();
	// Choosing 1GHz is as good as choosing CH0-CH7: the range follows the rate.
	sync_range_to_rate();
	build_time_options();
	build_channel_table();
}

void PpsConfigDlg::on_time_changed()
{
	if (_building)
		return;
	_result.capture_time_ms = _time_combo->currentData().toDouble();
}

void PpsConfigDlg::browse_dir()
{
	const QString dir = QFileDialog::getExistingDirectory(this,
		QString::fromUtf8("选择 CSV 保存目录"), _dir_edit->text());
	if (!dir.isEmpty())
		_dir_edit->setText(dir);
}

void PpsConfigDlg::on_auto_save_toggle()
{
	// The master switch only gates the per-channel ticks; it does not overwrite
	// the choices the user already made.
	const bool on = _auto_save_loss_check->isChecked();
	_result.auto_save_loss = on;
	for (QTableWidget *t : { _channel_table, _channel_table2 }) {
		for (int r = 0; r < t->rowCount(); ++r) {
			QCheckBox *box = qobject_cast<QCheckBox *>(t->cellWidget(r, 3));
			if (box)
				box->setEnabled(on);
		}
	}
}

void PpsConfigDlg::on_accept()
{
	_result.ref_channel = _ref_combo->currentData().toInt();
	_result.ref_edge = _ref_edge_check->isChecked()
		? pps::PPS_EDGE_FALLING : pps::PPS_EDGE_RISING;
	_result.trigger_pos_percent = _pos_spin->value();
	_result.sample_rate = _rate_combo->currentData().value<uint64_t>();
	_result.mode_channels = _range_combo->currentData().toInt();
	_result.capture_time_ms = _time_combo->currentData().toDouble();
	_result.save_dir = _dir_edit->text();
	_result.auto_save_loss = _auto_save_loss_check->isChecked();
	_result.calib_factor = _calib_spin->value();
	_result.channels.clear();
	_result.disabled_channels.clear();
	for (QTableWidget *t : { _channel_table, _channel_table2 }) {
		for (int row = 0; row < t->rowCount(); ++row) {
			QTableWidgetItem *name_item = t->item(row, 1);
			if (name_item == nullptr)
				continue;
			const QString name = name_item->text();
			const int index = name.mid(2).toInt();

			QTableWidgetItem *enable_item = t->item(row, 0);
			if (enable_item == nullptr ||
				enable_item->checkState() != Qt::Checked) {
				// Remembered so the dialog shows the same tick state next time.
				_result.disabled_channels.push_back(index);
				continue;
			}

			pps::PpsChannel ch;
			ch.channel_index = index;
			QCheckBox *edge_check = qobject_cast<QCheckBox *>(
				t->cellWidget(row, 2));
			ch.edge = (edge_check && edge_check->isChecked())
				? pps::PPS_EDGE_FALLING : pps::PPS_EDGE_RISING;
			QCheckBox *save_check = qobject_cast<QCheckBox *>(
				t->cellWidget(row, 3));
			ch.save_on_loss = (save_check && save_check->isChecked());
			ch.enabled = true;
			_result.channels.push_back(ch);
		}
	}

	if (_result.channels.empty()) {
		QMessageBox::warning(this, QString::fromUtf8("1PPS 测量配置"),
			QString::fromUtf8("请至少启用一个测试通道。"));
		return;
	}
	accept();
}

} // namespace dialogs
} // namespace pv
