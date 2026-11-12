/*
 * CSV writer for 1PPS measurement frames.
 * Creates the file with a header on first use, then appends one row per frame.
 */
#ifndef DSVIEW_PV_PPS_PPSWRITER_H
#define DSVIEW_PV_PPS_PPSWRITER_H

#include <QString>
#include <QDateTime>
#include <vector>
#include "ppsdecoder.h"

namespace pv {
namespace pps {

struct PpsConfig;

class PpsWriter
{
public:
    PpsWriter();

    // Build the standard file name: PPS_YYYYMMDD_HHMMSS.csv
    static QString build_file_name(const QDateTime &dt);

    // Append one frame. If the file does not exist it is created and the
    // header written. Returns false on I/O error.
    bool write_frame(const QString &file_path,
                     const PpsConfig &config,
                     const std::vector<PpsResult> &results,
                     const QDateTime &dt);
};

} // namespace pps
} // namespace pv

#endif // DSVIEW_PV_PPS_PPSWRITER_H
