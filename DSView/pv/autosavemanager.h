/*
 * This file is part of the DSView project.
 *
 * AutoSaveManager: in repeat (COLLECT_REPEAT) mode, every completed frame is
 * saved in the background as a native .dsl file. The capture/trigger path is
 * never blocked: occupied buffers are marked and skipped via the three-buffer
 * scheme in SigSession.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef DSVIEW_PV_AUTOSAVEMANAGER_H
#define DSVIEW_PV_AUTOSAVEMANAGER_H

#include <QObject>
#include <QTimer>
#include <QString>
#include <vector>

#include "interface/icallbacks.h"

namespace pv {

class SigSession;
class StoreSession;
class SessionData;

class AutoSaveManager : public QObject
{
	Q_OBJECT

public:
    explicit AutoSaveManager(SigSession *session, QObject *parent = 0);
    ~AutoSaveManager();

    // Claim the current view buffer and start a background .dsl save.
    void start_frame(ISessionDataGetter *getter);

    // Block until every background save has finished (used on app close).
    void wait_all();

    int active_count();

signals:
    void status_changed(const QString &text);
    void queue_idle();

private slots:
    void on_poll();

private:
    struct Job {
        StoreSession *store;
        SessionData  *buffer;
    };

    SigSession            *_session;
    std::vector<Job>       _jobs;
    QTimer                 _timer;
    int                    _seq;
};

} // namespace pv

#endif // DSVIEW_PV_AUTOSAVEMANAGER_H
