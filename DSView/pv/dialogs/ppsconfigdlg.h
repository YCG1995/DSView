/*
 * Configuration dialog for the 1PPS measurement mode.
 */
#ifndef DSVIEW_PV_DIALOGS_PPSCONFIGDLG_H
#define DSVIEW_PV_DIALOGS_PPSCONFIGDLG_H

#include <QDialog>
#include <QComboBox>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QLabel>
#include <QLineEdit>
#include <QTableWidget>
#include <QPushButton>

#include "../pps/ppsmanager.h"

namespace pv {

class SigSession;

namespace dialogs {

class PpsConfigDlg : public QDialog
{
    Q_OBJECT

public:
    PpsConfigDlg(SigSession *session, const pps::PpsConfig &cfg,
                 QWidget *parent = nullptr);

    pps::PpsConfig result_config() const { return _result; }

private slots:
    void browse_dir();
    void on_reference_changed();
    void on_range_changed();
    void on_rate_changed();
    void on_time_changed();
    void on_auto_save_toggle();
    void on_accept();

private:
    void build_rate_options();
    void sync_range_to_rate();
    void build_time_options();
    void build_channel_table();
    void make_channel_table(QTableWidget *&table, QWidget *parent);
    void fit_channel_table(QTableWidget *table);
    uint64_t max_rate_for_range(int channels) const;
    int range_for_rate(uint64_t rate) const;

    SigSession *_session;
    pps::PpsConfig _result;
    pps::PpsDeviceCaps _caps;
    bool _building;

    QComboBox *_ref_combo;
    QComboBox *_range_combo;        // CH0-CH7 (1GHz) / CH0-CH15 (500MHz)
    QCheckBox *_ref_edge_check;     // ticked = falling edge
    QSpinBox *_pos_spin;
    QComboBox *_time_combo;
    QComboBox *_rate_combo;
    QLineEdit *_dir_edit;
    QPushButton *_browse_btn;
    QCheckBox *_auto_save_loss_check; // save the raw frame when data is missing
    QDoubleSpinBox *_calib_spin;    // multiplier applied to every delta
    QTableWidget *_channel_table;   // CH0-CH7, left column
    QTableWidget *_channel_table2;  // CH8-CH15, right column
    QWidget *_channel_col1;         // label + table of the left column
    QWidget *_channel_col2;         // label + table of the right column
};

} // namespace dialogs
} // namespace pv

#endif // DSVIEW_PV_DIALOGS_PPSCONFIGDLG_H
