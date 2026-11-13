/*
 * Build-time switch for the head-less test hooks of the 1PPS / decoder UI.
 *
 * Those hooks were developed to drive and screenshot the UI without a mouse:
 * DSVIEW_PPS_AUTOSTART, DSVIEW_PPS_WAVE_*, DSVIEW_PPS_CALIB_*, ...
 * They must not be reachable from a release build, otherwise a stray
 * environment variable silently changes how the application behaves.
 *
 * DSVIEW_TEST_HOOKS is 0 for release builds; configure with
 *   -DDSVIEW_TEST_HOOKS=ON
 * to get them back for a test build.
 */
#ifndef DSVIEW_PV_PPS_PPSTESTHOOKS_H
#define DSVIEW_PV_PPS_PPSTESTHOOKS_H

#include <QByteArray>

#ifndef DSVIEW_TEST_HOOKS
#define DSVIEW_TEST_HOOKS 0
#endif

namespace pv {

// Reads a test-hook environment variable. Same return type as qgetenv(), so
// existing comparisons / conversions keep working unchanged. Always empty in
// a release build.
inline QByteArray test_env(const char *name)
{
#if DSVIEW_TEST_HOOKS
	return qgetenv(name);
#else
	(void)name;
	return QByteArray();
#endif
}

} // namespace pv

#endif // DSVIEW_PV_PPS_PPSTESTHOOKS_H
