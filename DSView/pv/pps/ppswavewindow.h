/*
 * Independent waveform window for the 1PPS measurement mode.
 *
 * Plots one curve per measured channel with time on the X axis, so the
 * per-frame delta_ns drift and the odd outlier are visible instead of only
 * the aggregated statistics shown in the result dock.
 *
 * Data can come from two sources:
 *   - live: follows PpsManager::frame_updated() while a measurement runs
 *   - file: loads a CSV written by PpsWriter (时间戳,CHn,...)
 */
#ifndef DSVIEW_PV_PPS_PPSWAVEWINDOW_H
#define DSVIEW_PV_PPS_PPSWAVEWINDOW_H

#include <QVector>
#include <QWidget>
#include <QPixmap>
#include <QTimer>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QMenu;
class QAction;
class QPushButton;
class QToolButton;

namespace pv {

namespace pps {

class PpsManager;

/*
 * Plot canvas. Pure painting + a little interaction (hover readout, wheel
 * zoom on the time axis, drag to pan, double click to reset).
 */
class PpsWaveCanvas : public QWidget
{
    Q_OBJECT

public:
    enum ViewMode {
        StackedOverlay = 0, // one plot area, every channel in its own band
        SharedOverlay  = 1, // one plot area, one shared Y axis
        Grid           = 2  // one sub plot per channel, stacked vertically
    };

    explicit PpsWaveCanvas(QWidget *parent = nullptr);

    void clear();
    void setViewMode(ViewMode mode);
    void setShowPoints(bool on);
    void setTimeAxisIsIndex(bool on);
    // Wall clock of t == 0 (epoch milliseconds). With a base set the time axis
    // prints HH:mm:ss instead of seconds since the first frame.
    void setTimeBase(double epoch_ms);
    // Auto range follows the data as it grows; zoom/pan turns it off.
    void setAutoX(bool on);
    bool autoX() const { return _auto_x; }
    // Frame level API used by the live feed.
    void beginSeries(const QVector<QString> &names);
    void appendFrame(double t, const QVector<double> &values,
                     const QVector<char> &valid);
    // Bulk load used by the CSV reader.
    void setData(const QVector<QString> &names,
                 const QVector<double> &times,
                 const QVector<QVector<double> > &values,
                 const QVector<QVector<char> > &valid,
                 bool time_axis_is_index);

    int frameCount() const { return _times.size(); }
    int hoverFrame() const { return _hover; }
    int channelCount() const { return _names.size(); }
    double timeSpan() const;
    // Width of the time window currently on screen (zoom aware).
    double viewSpan() const { return _t1 - _t0; }
    double viewStart() const { return _t0; }
    double viewEnd() const { return _t1; }
    // Show / hide one channel's curve (the data stays, only the drawing stops).
    void setChannelVisible(int ch, bool on);
    bool channelVisible(int ch) const;
    // Robust outlier cut for the automatic Y range. One global value for every
    // channel: samples farther than k * 1.4826 * MAD from the median are left
    // out of the range fit. With no such sample the range is the plain min/max,
    // which is exactly what "no outliers, no trimming" means.
    void setClipSigma(double k);
    double clipSigma() const { return _clip_sigma; }
    // Bulk append: frames are added without a repaint per frame, one
    // update() is emitted by endBulk(). This is what keeps live follow cheap
    // when frames arrive faster than the plot needs to be redrawn.
    void beginBulk();
    void endBulk();
    // Data access for the CSV export of a time range.
    const QVector<QString> &names() const { return _names; }
    const QVector<double> &times() const { return _times; }
    const QVector<QVector<double> > &values() const { return _values; }
    const QVector<QVector<char> > &validFlags() const { return _valid; }
    double timeBaseMs() const { return _base_ms; }
    bool timeIsIndex() const { return _time_is_index; }
    // Region selection: Shift + left drag picks a time range. It is drawn as a
    // shaded block and can then be zoomed into or written out as a CSV.
    bool hasSelection() const { return _sel_active; }
    void selectionRange(double &t0, double &t1) const;
    void setSelection(double t0, double t1);
    void clearSelection();
    void zoomToSelection();
    // Head-less benchmark: force a full rebuild of the plot cache n times and
    // return the average milliseconds per repaint.
    double benchFullRepaint(int n);
    double debugCollectMs() const { return _dbg_collect_ms; }
    double debugAxisMs() const { return _dbg_axis_ms; }
    double debugCurvesMs() const { return _dbg_curves_ms; }
    int debugPoints() const { return _dbg_points; }
    double debugPathMs() const { return _dbg_path_ms; }
    double debugDrawMs() const { return _dbg_draw_ms; }

signals:
    // Emitted when the Shift + drag selection appears, changes or is dropped.
    void selection_changed();

protected:
    void paintEvent(QPaintEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;

private:
    int visibleCount() const;
    QRectF plotRect() const;
    void recomputeTimeRange();
    double mapTime(double t, const QRectF &r) const;
    double timeAtX(double x, const QRectF &r) const;
    int indexNear(double x, const QRectF &r) const;
    // Frame indices that fall inside the visible time window, so a week of
    // frames does not get walked on every repaint.
    void visibleFrameRange(int &begin, int &end) const;

    // --- level-of-detail pyramid -----------------------------------------
    // Per channel, per level min/max of every block. Painting then reads a few
    // thousand blocks instead of every frame, which is what made a long run
    // (hundreds of thousands of frames) crawl on every repaint.
    void invalidateLod();
    void invalidateLod(int ch);
    void ensureLod(int ch) const;
    void updateLodTail(int ch, int old_count, int new_count);
    // One sample pair (value range) per painted pixel column, gaps flagged.
    void collectColumns(int ch, int begin, int end, const QRectF &r,
            QVector<double> &xs, QVector<double> &los, QVector<double> &his,
            QVector<char> &gap_before) const;
    void drawColumns(QPainter &p, const QVector<double> &xs,
            const QVector<double> &los, const QVector<double> &his,
            const QVector<char> &gap_before, double lo, double hi,
            double top, double bottom) const;

    void renderPlot(QPainter &p, const QRectF &r);
    void paintStacked(QPainter &p, const QRectF &r);
    void paintShared(QPainter &p, const QRectF &r);
    void paintGrid(QPainter &p, const QRectF &r);
    void paintTimeAxis(QPainter &p, const QRectF &r);
    void paintHover(QPainter &p, const QRectF &r);

    QVector<QString> _names;
    QVector<double> _times;               // seconds (or frame index)
    QVector<QVector<double> > _values;    // [channel][frame]
    QVector<QVector<char> > _valid;       // [channel][frame], 0 = gap
    QVector<bool> _visible;               // [channel], false = curve hidden

    // Pyramid: _lod[ch][level], block sizes 16^n. Built incrementally as
    // frames arrive; rebuilt from scratch after a trim shifts the alignment.
    struct LodLevel {
        QVector<double> mn;
        QVector<double> mx;
        QVector<char> gap;                // 1 = block holds an invalid frame
    };
    mutable QVector<QVector<LodLevel> > _lod;
    mutable QVector<char> _lod_ready;
    // Painted once per data/view/size change; hover then blits this and only
    // redraws the readout, so moving the mouse no longer rescans any data.
    QPixmap _cache;
    bool _cache_dirty = true;
    QColor _cache_base;
    // Sample budget for the automatic Y range: |v - median| <= k * sigma is
    // kept, everything farther out is ignored when fitting the range.
    double _clip_sigma = 3.0;
    int _clip_outliers = 0;   // samples left out by the last fit (per band)
    mutable double _dbg_collect_ms = 0.0;  // bench only: time spent collecting
    mutable double _dbg_axis_ms = 0.0;
    mutable double _dbg_curves_ms = 0.0;
    mutable int _dbg_points = 0;      // bench only: path points built
    mutable double _dbg_path_ms = 0.0;
    mutable double _dbg_draw_ms = 0.0;

    ViewMode _mode = StackedOverlay;
    bool _show_points = false;
    bool _time_is_index = false;
    double _base_ms = 0.0;    // wall clock of t == 0; 0 means "no wall clock"

    // Visible time window. Follows the data unless the user zoomed/panned.
    bool _auto_x = true;
    double _t0 = 0.0;
    double _t1 = 1.0;

    int _hover = -1;
    bool _panning = false;
    double _pan_x0 = 0.0;
    double _pan_t0 = 0.0;
    double _pan_t1 = 1.0;

    bool _bulk = false;       // suppress per-frame repaints while bulk loading

    // Region selection state (pixels while dragging, seconds once released).
    bool _sel_dragging = false;
    bool _sel_active = false;
    double _sel_px0 = 0.0;
    double _sel_px1 = 0.0;
    double _sel_t0 = 0.0;
    double _sel_t1 = 0.0;
    void paintSelection(QPainter &p, const QRectF &r);
};

/*
 * Top level window: toolbar, canvas and a status line. Owned by MainWindow
 * but lives as an independent top level window so it can be moved to a second
 * screen without dragging the main window along.
 */
class PpsWaveWindow : public QWidget
{
    Q_OBJECT

public:
    explicit PpsWaveWindow(pps::PpsManager *manager, QWidget *parent = nullptr);

public slots:
    void on_frame_updated();
    bool openCsv(const QString &path, QString &error);

private:
    void buildUi();
    void appendLiveFrame();
    void recordFrame(double t_ms, const QVector<QString> &names,
                     const QVector<double> &values,
                     const QVector<char> &valid);
    void replayPending();
    void updateInfo();
    void rebuildChannelMenu();
    void onBrowse();
    void onExport();
    void onClear();
    void onSaveSelection();
    void onSaveVisible();
    void onViewSelection();
    void updateSelectionButtons();
    // Writes frames whose time falls in [t0, t1] as a CSV in the same format
    // PpsWriter produces, so the file can be opened by this window again.
    bool writeRangeCsv(const QString &path, double t0, double t1, int &rows,
                       QString &error);
    // Head-less validation hook: DSVIEW_PPS_WAVE_SHOT=<png path> grabs the
    // canvas once enough frames exist, so the rendering can be checked without
    // a person clicking around.
    void maybeSaveShot();

    pps::PpsManager *_manager = nullptr;
    PpsWaveCanvas *_canvas = nullptr;

    QComboBox *_mode_box = nullptr;
    QCheckBox *_follow_box = nullptr;
    QCheckBox *_points_box = nullptr;
    QDoubleSpinBox *_clip_spin = nullptr;
    QPushButton *_open_btn = nullptr;
    QPushButton *_clear_btn = nullptr;
    QPushButton *_export_btn = nullptr;
    QToolButton *_ch_btn = nullptr;   // "通道" menu with per-channel tick boxes
    QMenu *_ch_menu = nullptr;
    QToolButton *_sel_btn = nullptr;  // "选区" menu: view / save / cancel
    QMenu *_sel_menu = nullptr;
    QAction *_sel_view_act = nullptr;
    QAction *_sel_save_act = nullptr;
    QAction *_sel_visible_act = nullptr;
    QAction *_sel_clear_act = nullptr;
    QLabel *_info = nullptr;

    bool _follow_live = true;
    QString _live_csv;       // CSV of the run currently being followed
    double _origin_ms = -1;  // wall clock of the first live frame
    QString _source;         // shown in the status line
    QString _shot_path;      // DSVIEW_PPS_WAVE_SHOT, empty when disabled
    bool _shot_done = false;
    bool _shot_scheduled = false;
    int _shot_tries = 0;
    bool _hide_env_applied = false;   // DSVIEW_PPS_WAVE_HIDE applied once
    int _shot_delay_ms = 150;
    // Coalesced live repaint: frames are buffered as they arrive and pushed to
    // the canvas at most 10 times per second. Following every frame repainted
    // the whole plot for each one, which is what pinned the CPU on a long run.
    QTimer *_flush_timer = nullptr;
    int _flushed = 0;                 // frames already handed to the canvas

    // Live frames are always buffered, whether or not the view follows them,
    // so switching 实时跟随 back on repaints the whole run instead of restarting.
    QVector<QString> _pend_names;
    QVector<double> _pend_t_ms;              // absolute epoch milliseconds
    QVector<QVector<double> > _pend_values;  // [channel][frame]
    QVector<QVector<char> > _pend_valid;
};

} // namespace pps
} // namespace pv

#endif // DSVIEW_PV_PPS_PPSWAVEWINDOW_H
