/*
 * MCP (Model Context Protocol) server for DSView.
 *
 * Exposes the running application's decoder tooling over a stateless
 * Streamable HTTP endpoint (single path "/mcp"), bound to 127.0.0.1 only.
 * This lets an MCP client drive script reload / decode / annotation /
 * console workflows without operating the graphical user interface.
 */

#ifndef DSVIEW_PV_MCP_MCPSERVER_H
#define DSVIEW_PV_MCP_MCPSERVER_H

#include <QHash>
#include <QObject>

class QTcpServer;
class QTcpSocket;
class QJsonObject;
class QJsonValue;

class ISessionDataGetter;

namespace pv {

class SigSession;

namespace view {
class View;
}

namespace mcp {

/*
 * Bridge between the MCP tool layer and the 1PPS measurement mode, which is
 * owned by the main window. Implemented by MainWindow.
 */
class IPpsControl
{
public:
	virtual ~IPpsControl() {}

	// Current configuration plus the device capabilities that constrain it.
	virtual QJsonObject pps_describe() = 0;
	// Apply a partial configuration (unknown keys are ignored). On success the
	// stored configuration is already normalised against the device.
	virtual bool pps_configure(const QJsonObject &cfg, QString &error) = 0;
	virtual bool pps_start(QString &error) = 0;
	virtual void pps_stop() = 0;
	// Running state, frame counters, per-frame timing and the latest results.
	virtual QJsonObject pps_status() = 0;
};

class McpServer : public QObject
{
	Q_OBJECT

public:
	explicit McpServer(QObject *parent = nullptr);

	bool start(quint16 port);
	void set_session(SigSession *session) { _session = session; }
	void set_view(view::View *view) { _view = view; }
	void set_data_getter(ISessionDataGetter *getter) { _data_getter = getter; }
	void set_pps_control(IPpsControl *pps) { _pps = pps; }


	static QJsonObject text_result(const QString &text, bool is_error = false);
	static QJsonValue rpc_error(int code, const QString &message);

private slots:
	void on_new_connection();
	void on_ready_read();
	void on_disconnected();

private:
	void drain_socket(QTcpSocket *sock);
	QByteArray handle_payload(const QByteArray &payload, bool &notification);
	QJsonObject dispatch(const QJsonObject &request, bool &notification);

	QJsonObject tool_list_decoders(const QJsonObject &args);
	QJsonObject tool_reload_changed(const QJsonObject &args);
	QJsonObject tool_get_annotations(const QJsonObject &args);
	QJsonObject tool_open_dsl(const QJsonObject &args);
	QJsonObject tool_get_status(const QJsonObject &args);
	QJsonObject tool_get_console(const QJsonObject &args);
	QJsonObject tool_read_script(const QJsonObject &args);
	QJsonObject tool_write_script(const QJsonObject &args);

	// Capture & device configuration.
	QJsonObject tool_get_device_info(const QJsonObject &args);
	QJsonObject tool_start_capture(const QJsonObject &args);
	QJsonObject tool_stop_capture(const QJsonObject &args);
	QJsonObject tool_set_collect_mode(const QJsonObject &args);
	QJsonObject tool_list_channels(const QJsonObject &args);
	QJsonObject tool_set_channel_enabled(const QJsonObject &args);
	QJsonObject tool_set_channel_name(const QJsonObject &args);
	QJsonObject tool_set_sample_config(const QJsonObject &args);

	// Decoder management, options and probes.
	QJsonObject tool_add_decoder(const QJsonObject &args);
	QJsonObject tool_remove_decoder(const QJsonObject &args);
	QJsonObject tool_reset_decoder(const QJsonObject &args);
	QJsonObject tool_list_decoder_stacks(const QJsonObject &args);
	QJsonObject tool_get_decoder_options(const QJsonObject &args);
	QJsonObject tool_set_decoder_option(const QJsonObject &args);
	QJsonObject tool_bind_probe(const QJsonObject &args);

	// Export, signals.
	QJsonObject tool_save_dsl(const QJsonObject &args);
	QJsonObject tool_export_decoded(const QJsonObject &args);
	QJsonObject tool_list_signals(const QJsonObject &args);
	QJsonObject tool_set_trace_name(const QJsonObject &args);

	// Cursors and measurements.
	QJsonObject tool_add_cursor(const QJsonObject &args);
	QJsonObject tool_remove_cursor(const QJsonObject &args);
	QJsonObject tool_list_cursors(const QJsonObject &args);
	QJsonObject tool_get_measurements(const QJsonObject &args);

	// 1PPS measurement mode.
	QJsonObject tool_get_pps_config(const QJsonObject &args);
	QJsonObject tool_set_pps_config(const QJsonObject &args);
	QJsonObject tool_start_pps(const QJsonObject &args);
	QJsonObject tool_stop_pps(const QJsonObject &args);
	QJsonObject tool_get_pps_status(const QJsonObject &args);

	QTcpServer *_server;
	SigSession *_session;
	view::View *_view;
	ISessionDataGetter *_data_getter;
	IPpsControl *_pps;
	QHash<QTcpSocket *, QByteArray> _buffers;
};

} // namespace mcp
} // namespace pv

#endif // DSVIEW_PV_MCP_MCPSERVER_H
