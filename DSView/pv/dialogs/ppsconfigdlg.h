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
    // `live_only` builds the dialog for a running measurement: the capture
    // parameters and the channel set are locked, only the DSL save policy
    // (数据丢失 / 异常脉冲 / 每通道“保存” / CSV 目录) stays editable.
    PpsConfigDlg(SigSession *session, const pps::PpsConfig &cfg,
                 bool live_only = false, QWidget *parent = nullptr);

    pps::PpsConfig result_config() const { return _result; }
    bool is_live_only() const { return _live_only; }

private slots:
    void browse_dir();
    void on_reference_changed();
    void on_range_changed();
    void on_rate_changed();
    void on_time_changed();
    void on_auto_save_toggle();
    void on_channel_state_changed();
    void on_accept();

private:
    void build_rate_options();
    void sync_range_to_rate();
    void build_time_options();
    void build_channel_table();
    void collect_channel_state();
    // Samples the next frame will actually need: the reference channel plus every
    // ticked measured channel. Device memory is shared, so this decides how long
    // a capture may be - the same rule the main window applies.
    int active_channel_count() const;
    void make_channel_table(QTableWidget *&table, QWidget *parent);
    void fit_channel_table(QTableWidget *table);
    uint64_t max_rate_for_range(int channels) const;
    int range_for_rate(uint64_t rate) const;

    SigSession *_session;
    pps::PpsConfig _result;
    pps::PpsDeviceCaps _caps;
    bool _building;
    bool _table_built = false;   // channel tables already hold the user's state
    bool _live_only = false;     // running measurement: only save options editable
    QWidget *_ref_box = nullptr;
    void lock_acquire_controls();

    QComboBox *_ref_combo;
    QComboBox *_range_combo;        // CH0-CH7 (1GHz) / CH0-CH15 (500MHz)
    QCheckBox *_ref_edge_check;     // ticked = falling edge
    QSpinBox *_pos_spin;
    QComboBox *_time_combo;
    QComboBox *_rate_combo;
    QLineEdit *_dir_edit;
    QPushButton *_browse_btn;
    QCheckBox *_auto_save_loss_check; // "数据丢失" DSL 保存
    QCheckBox *_save_multi_check;     // "异常脉冲" DSL 保存
    QDoubleSpinBox *_calib_spin;    // multiplier applied to every delta
    QTableWidget *_channel_table;   // CH0-CH7, left column
    QTableWidget *_channel_table2;  // CH8-CH15, right column
    QWidget *_channel_col1;         // label + table of the left column
    QWidget *_channel_col2;         // label + table of the right column
};

} // namespace dialogs
} // namespace pv

#endif // DSVIEW_PV_DIALOGS_PPSCONFIGDLG_H
