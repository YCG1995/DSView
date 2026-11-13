/*
 * This file is part of the DSView project.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "autosavemanager.h"
#include "sigsession.h"
#include "storesession.h"
#include "log.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QRegularExpression>

namespace pv {

AutoSaveManager::AutoSaveManager(SigSession *session, QObject *parent) :
    QObject(parent),
    _session(session),
    _seq(0)
{
    connect(&_timer, &QTimer::timeout, this, &AutoSaveManager::on_poll);
    _timer.setInterval(200);
}

AutoSaveManager::~AutoSaveManager()
{
    wait_all();
}

void AutoSaveManager::start_frame(ISessionDataGetter *getter)
{
    const QString base_dir = QCoreApplication::applicationDirPath() + "/autosave";
    QDir dir;
    if (!dir.mkpath(base_dir)){
        dsv_err("AutoSave: failed to create directory %s",
                base_dir.toLocal8Bit().constData());
        return;
    }

    // File name: <device>-R-<timestamp>-<seq>.dsl
    QString device_name = _session->get_device()->name();
    device_name.replace(QRegularExpression("[\\\\/:*?\"<>|]"), "_");

    const QString stamp =
        QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss-zzz");

    QString file_name = device_name + "-R-" + stamp +
                        QString("-%1.dsl").arg(_seq++, 4, 10, QChar('0'));
    const QString full_path = base_dir + "/" + file_name;

    // Claim the view buffer before the save thread starts so that no
    // subsequent swap or capture can clear its snapshots.
    SessionData *buffer = _session->auto_save_claim_view();

    StoreSession *store = new StoreSession(_session);
    store->set_file_name(full_path);
    store->_sessionDataGetter = getter;

    if (!store->save_start()){
        dsv_err("AutoSave: failed to start save for %s",
                full_path.toLocal8Bit().constData());
        _session->auto_save_release(buffer);
        delete store;
        return;
    }

    _jobs.push_back(Job{store, buffer});

    if (!_timer.isActive())
        _timer.start();

    emit status_changed(QString("Auto saving... pending %1").arg(_jobs.size()));
}

void AutoSaveManager::on_poll()
{
    for (auto it = _jobs.begin(); it != _jobs.end(); )
    {
        if (!it->store->is_busy())
        {
            // Join the finished worker thread.
            it->store->wait();

            const QString err = it->store->error();
            if (!err.isEmpty())
                dsv_err("AutoSave: %s", err.toLocal8Bit().constData());

            _session->auto_save_release(it->buffer);
            delete it->store;

            it = _jobs.erase(it);
        }
        else
        {
            ++it;
        }
    }

    if (_jobs.empty()){
        _timer.stop();
        emit status_changed("");
        emit queue_idle();
    }
    else{
        emit status_changed(
            QString("Auto saving... pending %1").arg(_jobs.size()));
    }
}

void AutoSaveManager::wait_all()
{
    _timer.stop();

    for (auto &job : _jobs)
    {
        job.store->wait();
        _session->auto_save_release(job.buffer);
        delete job.store;
    }

    _jobs.clear();
}

int AutoSaveManager::active_count()
{
    return (int)_jobs.size();
}

} // namespace pv
