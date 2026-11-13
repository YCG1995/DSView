/*
 * CSV writer for 1PPS measurement frames — implementation.
 */
#include "ppswriter.h"
#include "ppsmanager.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>

namespace pv {
namespace pps {

PpsWriter::PpsWriter() {}

QString PpsWriter::build_file_name(const QDateTime &dt)
{
	// Milliseconds included: a stop/start inside the same second used to reuse
	// the previous file name, and the new run was appended to the old data.
	return "PPS_" + dt.toString("yyyyMMdd_HHmmss_zzz") + ".csv";
}

static QString format_ns(double v)
{
	// One decimal place: deltas are reported after the calibration factor has
	// been applied, so sub-nanosecond digits are meaningful.
	QString s = QString::number(v, 'f', 1);
	if (s == "-0.0")
		s = "0.0";
	return s;
}

bool PpsWriter::write_frame(const QString &file_path,
		const PpsConfig &config,
		const std::vector<PpsResult> &results,
		const QDateTime &dt)
{
	QDir().mkpath(QFileInfo(file_path).absolutePath());

	const bool need_header = !QFile::exists(file_path);

	QFile f(file_path);
	if (!f.open(QIODevice::Append | QIODevice::Text))
		return false;

	QTextStream out(&f);
	// Qt6: QTextStream 默认 UTF-8，不再需要 setCodec
	if (need_header) {
		out.setGenerateByteOrderMark(true);
		out << QString::fromUtf8("触发时间");
		for (const PpsChannel &ch : config.channels)
			out << ",CH" << ch.channel_index;
		out << "\n";
	}

	out << dt.toString("yyyy-MM-dd HH:mm:ss.zzz");
	for (const PpsResult &r : results) {
		out << ",";
		if (r.valid)
			out << format_ns(r.delta_ns);
	}
	out << "\n";
	out.flush();
	f.close();
	return true;
}

} // namespace pps
} // namespace pv
