/*
 * MCP (Model Context Protocol) server for DSView - implementation.
 */

#include "mcpserver.h"

#include <QTcpServer>
#include <QTcpSocket>
#include <QEventLoop>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonValue>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QDateTime>

#include <libsigrokdecode.h>

#include "../sigsession.h"
#include "../deviceagent.h"
#include "../interface/icallbacks.h"
#include "../storesession.h"
#include "../config/appconfig.h"
#include "../view/view.h"
#include "../view/decodetrace.h"
#include "../view/cursor.h"
#include "../view/signal.h"
#include "../data/decoderstack.h"
#include "../data/decode/annotation.h"
#include "../data/decode/decoder.h"
#include "../data/decode/decoderstatus.h"

namespace pv {
namespace mcp {

// Protocol version advertised to MCP clients.
static const char *MCP_PROTOCOL_VERSION = "2025-06-18";

struct ToolDef {
	const char *name;
	const char *description;
	const char *schema; // JSON object string for inputSchema
};

static const ToolDef TOOL_DEFS[] = {
	// --- script / decode introspection (original 8) ---
	{"list_decoders",
	 "List all loaded protocol decoder packages (module name, id, long name).",
	 R"({"type":"object","properties":{}})"},
	{"reload_changed_scripts",
	 "Reload only the decoder scripts that changed since the baseline, then re-decode.",
	 R"({"type":"object","properties":{}})"},
	{"get_annotations",
	 "Return decoded annotations. Optional trace index, row name and a result limit.",
	 R"({"type":"object","properties":{
	     "trace_index":{"type":"integer"},
	     "row":{"type":"string"},
	     "limit":{"type":"integer"}}})"},
	{"open_dsl",
	 "Open a DSView .dsl session file and decode it.",
	 R"({"type":"object","properties":{"path":{"type":"string"}},
	     "required":["path"]})"},
	{"get_status",
	 "Report per-trace decode state, sample counts and progress.",
	 R"({"type":"object","properties":{}})"},
	{"get_console",
	 "Return captured stdout/stderr text from decoder scripts and optionally clear it.",
	 R"({"type":"object","properties":{"clear":{"type":"boolean"}}})"},
	{"read_script",
	 "Read a file inside a decoder package (default pd.py).",
	 R"({"type":"object","properties":{"module":{"type":"string"},
	     "file":{"type":"string"}},"required":["module"]})"},
	{"write_script",
	 "Write a file inside a decoder package (default pd.py), used to edit scripts.",
	 R"({"type":"object","properties":{"module":{"type":"string"},
	     "file":{"type":"string"},"content":{"type":"string"}},
	     "required":["module","content"]})"},

	// --- capture & device ---
	{"get_device_info",
	 "Report the active device name, type, sample rate, sample limit and work mode.",
	 R"({"type":"object","properties":{}})"},
	{"start_capture",
	 "Start a capture. Pass instant=true for an immediate (single) run.",
	 R"({"type":"object","properties":{"instant":{"type":"boolean"}}})"},
	{"stop_capture",
	 "Stop the running capture.",
	 R"({"type":"object","properties":{}})"},
	{"set_collect_mode",
	 "Select collection mode: 0=single, 1=repeat, 2=loop.",
	 R"({"type":"object","properties":{"mode":{"type":"integer"}},
	     "required":["mode"]})"},
	{"list_channels",
	 "List all device channels with index, name and enabled state.",
	 R"({"type":"object","properties":{}})"},
	{"set_channel_enabled",
	 "Enable or disable a channel by its index.",
	 R"({"type":"object","properties":{"index":{"type":"integer"},
	     "enabled":{"type":"boolean"}},"required":["index","enabled"]})"},
	{"set_channel_name",
	 "Rename a channel by its index.",
	 R"({"type":"object","properties":{"index":{"type":"integer"},
	     "name":{"type":"string"}},"required":["index","name"]})"},
	{"set_sample_config",
	 "Set sample rate and/or sample limit (omit a key to leave it unchanged).",
	 R"({"type":"object","properties":{"sample_rate":{"type":"integer"},
	     "sample_limit":{"type":"integer"}}})"},

	// --- decoder management ---
	{"add_decoder",
	 "Add a decoder by its module name or id. Optional stacked parent trace index.",
	 R"({"type":"object","properties":{"name":{"type":"string"},
	     "parent_trace_index":{"type":"integer"}},"required":["name"]})"},
	{"remove_decoder",
	 "Remove the decoder trace at the given index.",
	 R"({"type":"object","properties":{"index":{"type":"integer"}},
	     "required":["index"]})"},
	{"reset_decoder",
	 "Reset (clear) the decoder at the given index.",
	 R"({"type":"object","properties":{"index":{"type":"integer"}},
	     "required":["index"]})"},
	{"list_decoder_stacks",
	 "List active decoder traces with their rows, options and probe bindings.",
	 R"({"type":"object","properties":{}})"},
	{"get_decoder_options",
	 "List a decoder's option schema (id, type, default, choices) and current values.",
	 R"({"type":"object","properties":{"trace_index":{"type":"integer"}},
	     "required":["trace_index"]})"},
	{"set_decoder_option",
	 "Set one decoder option (typed value) and re-decode.",
	 R"({"type":"object","properties":{"trace_index":{"type":"integer"},
	     "option":{"type":"string"},"value":{}},
	     "required":["trace_index","option"]})"},
	{"bind_probe",
	 "Bind a decoder channel (its id) to a signal probe index and re-decode.",
	 R"({"type":"object","properties":{"trace_index":{"type":"integer"},
	     "channel":{"type":"string"},"probe_index":{"type":"integer"}},
	     "required":["trace_index","channel","probe_index"]})"},

	// --- export & signals ---
	{"save_dsl",
	 "Save the current session to a .dsl file at the given path (optional range).",
	 R"({"type":"object","properties":{"path":{"type":"string"},
	     "start":{"type":"integer"},"end":{"type":"integer"}},
	     "required":["path"]})"},
	{"export_decoded",
	 "Export decoded data to the given path; format is derived from the extension.",
	 R"({"type":"object","properties":{"path":{"type":"string"},
	     "start":{"type":"integer"},"end":{"type":"integer"}},
	     "required":["path"]})"},
	{"list_signals",
	 "List all signals with index, name and type.",
	 R"({"type":"object","properties":{}})"},
	{"set_trace_name",
	 "Rename a trace by its index.",
	 R"({"type":"object","properties":{"index":{"type":"integer"},
	     "name":{"type":"string"}},"required":["index","name"]})"},

	// --- cursors & measurement ---
	{"add_cursor",
	 "Add a cursor at the given sample index.",
	 R"({"type":"object","properties":{"sample_index":{"type":"integer"}},
	     "required":["sample_index"]})"},
	{"remove_cursor",
	 "Remove the cursor at the given cursor index.",
	 R"({"type":"object","properties":{"index":{"type":"integer"}},
	     "required":["index"]})"},
	{"list_cursors",
	 "List cursors with their sample indices.",
	 R"({"type":"object","properties":{}})"},
	{"get_measurements",
	 "Return standard measurements (frequency, period, pulse width, duty cycle, etc.).",
	 R"({"type":"object","properties":{}})"},

	// --- 1PPS measurement mode ---
	{"get_pps_config",
	 "Return the 1PPS configuration (reference channel, edge, trigger position, "
	 "capture window, samplerate, channel range, measured channels, save dir) "
	 "together with the device capabilities that constrain it.",
	 R"({"type":"object","properties":{}})"},
	{"set_pps_config",
	 "Apply a partial 1PPS configuration; omitted keys keep their current value. "
	 "The stored configuration is normalised against the device afterwards "
	 "(samplerate wins over channel range, channels above the usable count are "
	 "dropped) and the normalised result is returned.",
	 R"({"type":"object","properties":{
	     "ref_channel":{"type":"integer"},
	     "ref_edge":{"type":"integer"},
	     "trigger_pos_percent":{"type":"integer"},
	     "capture_ms":{"type":"number"},
	     "sample_rate":{"type":"integer"},
	     "save_dir":{"type":"string"},
	     "range":{"type":"integer"},
	     "channels":{"type":"array","items":{"type":"object"}},
	     "disabled":{"type":"array","items":{"type":"integer"}}}})"},
	{"start_pps",
	 "Start the 1PPS measurement loop on the configured (or device-derived) "
	 "configuration. Fails when a capture is already running.",
	 R"({"type":"object","properties":{}})"},
	{"stop_pps",
	 "Stop the 1PPS measurement loop and report how many frames were measured.",
	 R"({"type":"object","properties":{}})"},
	{"get_pps_status",
	 "Report the 1PPS running state, frame count, capture/upload timing of the "
	 "last frame, per-frame results and per-channel statistics.",
	 R"({"type":"object","properties":{}})"},
};

McpServer::McpServer(QObject *parent) :
	QObject(parent),
	_server(new QTcpServer(this)),
	_session(nullptr),
	_view(nullptr),
	_data_getter(nullptr),
	_pps(nullptr)
{
	connect(_server, &QTcpServer::newConnection,
		this, &McpServer::on_new_connection);
}

bool McpServer::start(quint16 port)
{
	// Loopback only: never expose the control endpoint to the network.
	if (!_server->listen(QHostAddress::LocalHost, port))
		return false;
	return true;
}

void McpServer::on_new_connection()
{
	while (QTcpSocket *sock = _server->nextPendingConnection()) {
		connect(sock, &QTcpSocket::readyRead,
			this, &McpServer::on_ready_read);
		connect(sock, &QTcpSocket::disconnected,
			this, &McpServer::on_disconnected);
		_buffers.insert(sock, QByteArray());
	}
}

void McpServer::on_disconnected()
{
	QTcpSocket *sock = qobject_cast<QTcpSocket *>(sender());
	if (sock)
		_buffers.remove(sock);
}

void McpServer::on_ready_read()
{
	QTcpSocket *sock = qobject_cast<QTcpSocket *>(sender());
	if (!sock)
		return;

	_buffers[sock].append(sock->readAll());
	drain_socket(sock);
}

void McpServer::drain_socket(QTcpSocket *sock)
{
	QByteArray &buf = _buffers[sock];

	for (;;) {
		int header_end = buf.indexOf("\r\n\r\n");
		if (header_end < 0)
			return;

		const QByteArray raw_header = buf.left(header_end);
		const QList<QByteArray> header_lines = raw_header.split('\n');
		if (header_lines.isEmpty()) {
			buf.remove(0, header_end + 4);
			continue;
		}

		const QList<QByteArray> request_line =
				header_lines.first().split(' ');
		if (request_line.size() < 3) {
			buf.remove(0, header_end + 4);
			continue;
		}

		const QByteArray method = request_line[0];
		const QByteArray target = request_line[1];

		int content_length = 0;
		for (int i = 1; i < header_lines.size(); ++i) {
			const int colon = header_lines[i].indexOf(':');
			if (colon <= 0)
				continue;
			const QByteArray key =
					header_lines[i].left(colon).trimmed().toLower();
			if (key == "content-length")
				content_length = header_lines[i].mid(colon + 1)
						.trimmed().toInt();
		}

		const int body_start = header_end + 4;
		if (buf.size() < body_start + content_length)
			return; // wait for the rest of the body

		const QByteArray body = buf.mid(body_start, content_length);
		buf.remove(0, body_start + content_length);

		auto send = [&](int status, const QByteArray &reason,
				const QByteArray &resp_body, bool empty) {
			QByteArray resp("HTTP/1.1 ");
			resp.append(QByteArray::number(status)).append(' ')
				.append(reason).append("\r\n");
			resp.append("Content-Type: application/json; charset=utf-8\r\n");
			resp.append("Content-Length: ")
				.append(QByteArray::number(empty ? 0 : resp_body.size()))
				.append("\r\n");
			resp.append("Connection: keep-alive\r\n\r\n");
			if (!empty)
				resp.append(resp_body);
			sock->write(resp);
		};

		if (method != "POST" || !target.startsWith("/mcp")) {
			send(404, "Not Found", QByteArray(), true);
			continue;
		}

		bool notification = false;
		QByteArray result;
		const QJsonDocument doc = QJsonDocument::fromJson(body);
		if (doc.isObject()) {
			result = handle_payload(body, notification);
		} else if (doc.isArray()) {
			QJsonArray batch;
			for (const QJsonValue &v : doc.array()) {
				QJsonObject one =
						dispatch(v.toObject(), notification);
				if (!one.isEmpty())
					batch.append(one);
			}
			result = QJsonDocument(batch).toJson(
					QJsonDocument::Compact);
			notification = false;
		} else {
			send(400, "Bad Request", QByteArray(), true);
			continue;
		}

		if (notification)
			send(202, "Accepted", QByteArray(), true);
		else
			send(200, "OK", result, false);
	}
}

QByteArray McpServer::handle_payload(const QByteArray &payload,
		bool &notification)
{
	const QJsonObject request = QJsonDocument::fromJson(payload).object();
	const QJsonObject response = dispatch(request, notification);
	if (notification)
		return QByteArray();
	return QJsonDocument(response).toJson(QJsonDocument::Compact);
}

QJsonObject McpServer::dispatch(const QJsonObject &request, bool &notification)
{
	notification = !request.contains("id");
	const QString method = request.value("method").toString();
	const QJsonValue id = request.value("id");
	const QJsonObject params = request.value("params").toObject();

	if (method == "notifications/initialized") {
		notification = true;
		return QJsonObject();
	}

	if (method == "initialize") {
		QJsonObject result;
		result["protocolVersion"] = MCP_PROTOCOL_VERSION;
		result["capabilities"] = QJsonObject{{"tools", QJsonObject()}};
		result["serverInfo"] = QJsonObject{
			{"name", "DSView MCP"}, {"version", "1.4.0"}};
		QJsonObject out{{"jsonrpc", "2.0"}, {"id", id}, {"result", result}};
		notification = false;
		return out;
	}

	if (method == "ping") {
		return QJsonObject{{"jsonrpc", "2.0"}, {"id", id},
				{"result", QJsonObject()}};
	}

	if (method == "tools/list") {
		QJsonArray tools;
		for (const ToolDef &t : TOOL_DEFS) {
			QJsonObject tool;
			tool["name"] = t.name;
			tool["description"] = t.description;
			tool["inputSchema"] = QJsonDocument::fromJson(t.schema).object();
			tools.append(tool);
		}
		return QJsonObject{{"jsonrpc", "2.0"}, {"id", id},
				{"result", QJsonObject{{"tools", tools}}}};
	}

	if (method == "tools/call") {
		const QString name = params.value("name").toString();
		const QJsonObject args = params.value("arguments").toObject();

		QJsonObject result;
		if (name == "list_decoders")
			result = tool_list_decoders(args);
		else if (name == "reload_changed_scripts")
			result = tool_reload_changed(args);
		else if (name == "get_annotations")
			result = tool_get_annotations(args);
		else if (name == "open_dsl")
			result = tool_open_dsl(args);
		else if (name == "get_status")
			result = tool_get_status(args);
		else if (name == "get_console")
			result = tool_get_console(args);
		else if (name == "read_script")
			result = tool_read_script(args);
		else if (name == "write_script")
			result = tool_write_script(args);
		else if (name == "get_device_info")
			result = tool_get_device_info(args);
		else if (name == "start_capture")
			result = tool_start_capture(args);
		else if (name == "stop_capture")
			result = tool_stop_capture(args);
		else if (name == "set_collect_mode")
			result = tool_set_collect_mode(args);
		else if (name == "list_channels")
			result = tool_list_channels(args);
		else if (name == "set_channel_enabled")
			result = tool_set_channel_enabled(args);
		else if (name == "set_channel_name")
			result = tool_set_channel_name(args);
		else if (name == "set_sample_config")
			result = tool_set_sample_config(args);
		else if (name == "add_decoder")
			result = tool_add_decoder(args);
		else if (name == "remove_decoder")
			result = tool_remove_decoder(args);
		else if (name == "reset_decoder")
			result = tool_reset_decoder(args);
		else if (name == "list_decoder_stacks")
			result = tool_list_decoder_stacks(args);
		else if (name == "get_decoder_options")
			result = tool_get_decoder_options(args);
		else if (name == "set_decoder_option")
			result = tool_set_decoder_option(args);
		else if (name == "bind_probe")
			result = tool_bind_probe(args);
		else if (name == "save_dsl")
			result = tool_save_dsl(args);
		else if (name == "export_decoded")
			result = tool_export_decoded(args);
		else if (name == "list_signals")
			result = tool_list_signals(args);
		else if (name == "set_trace_name")
			result = tool_set_trace_name(args);
		else if (name == "add_cursor")
			result = tool_add_cursor(args);
		else if (name == "remove_cursor")
			result = tool_remove_cursor(args);
		else if (name == "list_cursors")
			result = tool_list_cursors(args);
		else if (name == "get_measurements")
			result = tool_get_measurements(args);
		else if (name == "get_pps_config")
			result = tool_get_pps_config(args);
		else if (name == "set_pps_config")
			result = tool_set_pps_config(args);
		else if (name == "start_pps")
			result = tool_start_pps(args);
		else if (name == "stop_pps")
			result = tool_stop_pps(args);
		else if (name == "get_pps_status")
			result = tool_get_pps_status(args);
		else
			result = text_result("Unknown tool: " + name, true);

		return QJsonObject{{"jsonrpc", "2.0"}, {"id", id},
				{"result", result}};
	}

	QJsonObject err{{"code", -32601}, {"message", "Method not found: " + method}};
	return QJsonObject{{"jsonrpc", "2.0"}, {"id", id}, {"error", err}};
}

QJsonObject McpServer::text_result(const QString &text, bool is_error)
{
	QJsonObject item{{"type", "text"}, {"text", text}};
	QJsonObject result;
	result["content"] = QJsonArray{item};
	result["isError"] = is_error;
	return result;
}

QJsonValue McpServer::rpc_error(int code, const QString &message)
{
	return QJsonObject{{"code", code}, {"message", message}};
}

QJsonObject McpServer::tool_list_decoders(const QJsonObject &)
{
	QJsonArray arr;
	const GSList *l = srd_decoder_list();
	for (; l; l = l->next) {
		const srd_decoder *d = static_cast<const srd_decoder *>(l->data);
		QJsonObject o;
		o["module"] = d->module_name ? d->module_name : "";
		o["id"] = d->id ? d->id : "";
		o["name"] = d->longname ? d->longname : "";
		arr.append(o);
	}
	return text_result(QString::fromUtf8(
			QJsonDocument(arr).toJson(QJsonDocument::Compact)));
}

QJsonObject McpServer::tool_reload_changed(const QJsonObject &)
{
	if (!_session)
		return text_result("No active session.", true);
	const QString msg = _session->reload_decoder_scripts();
	return text_result(msg);
}

QJsonObject McpServer::tool_get_annotations(const QJsonObject &args)
{
	if (!_session)
		return text_result("No active session.", true);

	const bool has_trace = args.contains("trace_index");
	const int wanted_trace = has_trace ? args.value("trace_index").toInt() : -1;
	const QString wanted_row = args.value("row").toString();
	int limit = args.contains("limit") ? args.value("limit").toInt() : 500;
	if (limit <= 0)
		limit = 500;

	QJsonArray out;
	int count = 0;
	const std::vector<view::DecodeTrace *> traces =
			_session->get_decode_signals();

	for (size_t i = 0; i < traces.size() && count < limit; ++i) {
		if (has_trace && static_cast<int>(i) != wanted_trace)
			continue;

		data::DecoderStack *stack = traces[i]->decoder();
		if (!stack)
			continue;

		const int rows = stack->list_rows_size();
		for (int r = 0; r < rows && count < limit; ++r) {
			QString row_title;
			stack->list_row_title(r, row_title);
			if (!wanted_row.isEmpty() && row_title != wanted_row)
				continue;

			const uint64_t n = stack->list_annotation_size(
					static_cast<uint16_t>(r));
			for (uint64_t c = 0; c < n && count < limit; ++c) {
				data::decode::Annotation ann;
				if (!stack->list_annotation(&ann,
						static_cast<uint16_t>(r), c))
					continue;

				QJsonObject a;
				a["trace_index"] = static_cast<int>(i);
				a["row"] = row_title;
				a["start_sample"] = qint64(ann.start_sample());
				a["end_sample"] = qint64(ann.end_sample());
				a["type"] = ann.type();
				const std::vector<QString> &texts = ann.annotations();
				a["text"] = texts.empty() ? "" : texts.front();
				out.append(a);
				++count;
			}
		}
	}

	return text_result(QString::fromUtf8(
			QJsonDocument(out).toJson(QJsonDocument::Compact)));
}

QJsonObject McpServer::tool_open_dsl(const QJsonObject &args)
{
	if (!_session)
		return text_result("No active session.", true);

	const QString path = args.value("path").toString();
	if (path.isEmpty())
		return text_result("Missing path argument.", true);

	if (!QFileInfo::exists(path))
		return text_result("File not found: " + path, true);

	const bool ok = _session->set_file(path);
	return text_result(ok ? "Opened: " + path : "Failed to open: " + path,
			!ok);
}

QJsonObject McpServer::tool_get_status(const QJsonObject &)
{
	if (!_session)
		return text_result("No active session.", true);

	QJsonArray arr;
	const std::vector<view::DecodeTrace *> traces =
			_session->get_decode_signals();

	for (size_t i = 0; i < traces.size(); ++i) {
		data::DecoderStack *stack = traces[i]->decoder();
		if (!stack)
			continue;
		QJsonObject o;
		o["trace_index"] = static_cast<int>(i);
		o["running"] = stack->IsRunning();
		o["sample_count"] = qint64(stack->sample_count());
		o["sample_rate"] = qint64(stack->sample_rate());
		o["progress"] = stack->get_progress();
		o["result_count"] = qint64(stack->get_result_count());
		arr.append(o);
	}

	return text_result(QString::fromUtf8(
			QJsonDocument(arr).toJson(QJsonDocument::Compact)));
}

QJsonObject McpServer::tool_get_console(const QJsonObject &args)
{
	const bool clear = args.value("clear").toBool(false);
	char *raw = srd_console_take(clear ? 1 : 0);
	const QString text = QString::fromUtf8(raw ? raw : "");
	g_free(raw);
	return text_result(text);
}

static bool unsafe_name(const QString &s)
{
	return s.isEmpty() || s.contains("..") || s.contains('/') ||
			s.contains('\\');
}

QJsonObject McpServer::tool_read_script(const QJsonObject &args)
{
	const QString module = args.value("module").toString();
	const QString file = args.value("file").toString("pd.py");
	if (unsafe_name(module) || unsafe_name(file))
		return text_result("Invalid module or file name.", true);

	const QString path = GetDecodeScriptDir() + "/" + module + "/" + file;
	QFile f(path);
	if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
		return text_result("Cannot open: " + path, true);

	return text_result(QString::fromUtf8(f.readAll()));
}

QJsonObject McpServer::tool_write_script(const QJsonObject &args)
{
	const QString module = args.value("module").toString();
	const QString file = args.value("file").toString("pd.py");
	const QString content = args.value("content").toString();
	if (unsafe_name(module) || unsafe_name(file))
		return text_result("Invalid module or file name.", true);

	const QString path = GetDecodeScriptDir() + "/" + module + "/" + file;
	QFile f(path);
	if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate |
			QIODevice::Text))
		return text_result("Cannot open for write: " + path, true);

	f.write(content.toUtf8());
	f.close();
	return text_result("Wrote " + QString::number(content.size()) +
			" chars to " + path);
}

// ---------- GVariant <-> JSON helpers ----------

static QJsonValue gv_to_json(GVariant *v)
{
	if (!v)
		return QJsonValue();

	if (g_variant_is_of_type(v, G_VARIANT_TYPE_BOOLEAN))
		return g_variant_get_boolean(v);
	if (g_variant_is_of_type(v, G_VARIANT_TYPE_BYTE))
		return int(g_variant_get_byte(v));
	if (g_variant_is_of_type(v, G_VARIANT_TYPE_INT16))
		return g_variant_get_int16(v);
	if (g_variant_is_of_type(v, G_VARIANT_TYPE_UINT16))
		return int(g_variant_get_uint16(v));
	if (g_variant_is_of_type(v, G_VARIANT_TYPE_INT32))
		return g_variant_get_int32(v);
	if (g_variant_is_of_type(v, G_VARIANT_TYPE_UINT32))
		return qint64(g_variant_get_uint32(v));
	if (g_variant_is_of_type(v, G_VARIANT_TYPE_INT64))
		return qint64(g_variant_get_int64(v));
	if (g_variant_is_of_type(v, G_VARIANT_TYPE_UINT64))
		return qint64(g_variant_get_uint64(v));
	if (g_variant_is_of_type(v, G_VARIANT_TYPE_DOUBLE))
		return g_variant_get_double(v);
	if (g_variant_is_of_type(v, G_VARIANT_TYPE_STRING))
		return QString::fromUtf8(g_variant_get_string(v, NULL));

	// Maybe / container: unwrap or fall back to its printed form.
	if (g_variant_type_is_maybe(g_variant_get_type(v))) {
		GVariant *child = g_variant_get_maybe(v);
		if (child)
			return gv_to_json(child);
		return QJsonValue();
	}

	GVariant *scalar = g_variant_is_of_type(v, G_VARIANT_TYPE_VARIANT)
			? g_variant_get_variant(v) : NULL;
	if (scalar)
		return gv_to_json(scalar);

	return QString::fromUtf8(g_variant_print(v, NULL));
}

// Build a GVariant of the same type as ref_type from a JSON value.
static GVariant *json_to_gv(const QJsonValue &j, const GVariantType *ref)
{
	if (g_variant_type_equal(ref, G_VARIANT_TYPE_BOOLEAN))
		return g_variant_new_boolean(j.toBool());
	if (g_variant_type_equal(ref, G_VARIANT_TYPE_BYTE))
		return g_variant_new_byte(quint8(j.toInt()));
	if (g_variant_type_equal(ref, G_VARIANT_TYPE_INT16))
		return g_variant_new_int16(j.toInt());
	if (g_variant_type_equal(ref, G_VARIANT_TYPE_UINT16))
		return g_variant_new_uint16(quint16(j.toInt()));
	if (g_variant_type_equal(ref, G_VARIANT_TYPE_INT32))
		return g_variant_new_int32(j.toInt());
	if (g_variant_type_equal(ref, G_VARIANT_TYPE_UINT32))
		return g_variant_new_uint32(quint32(j.toDouble()));
	if (g_variant_type_equal(ref, G_VARIANT_TYPE_INT64))
		return g_variant_new_int64(j.toVariant().toLongLong());
	if (g_variant_type_equal(ref, G_VARIANT_TYPE_UINT64))
		return g_variant_new_uint64(j.toVariant().toULongLong());
	if (g_variant_type_equal(ref, G_VARIANT_TYPE_DOUBLE))
		return g_variant_new_double(j.toDouble());
	if (g_variant_type_equal(ref, G_VARIANT_TYPE_STRING))
		return g_variant_new_string(j.toString().toUtf8().constData());

	// Fall back to a string for unknown/complex types.
	return g_variant_new_string(j.toString().toUtf8().constData());
}

// ---------- capture & device ----------

QJsonObject McpServer::tool_get_device_info(const QJsonObject &)
{
	DeviceAgent *dev = _session->get_device();
	QJsonObject o;
	o["has_device"] = dev->have_instance();
	o["name"] = dev->name();
	o["driver"] = dev->driver_name();
	o["is_file"] = dev->is_file();
	o["is_demo"] = dev->is_demo();
	o["sample_rate"] = qint64(_session->cur_samplerate());
	o["sample_limit"] = qint64(_session->cur_samplelimits());
	o["work_mode"] = _session->get_device()->get_work_mode();
	o["collect_mode"] = _session->get_collect_mode();
	o["working"] = _session->is_working();
	return text_result(QString::fromUtf8(
			QJsonDocument(o).toJson(QJsonDocument::Compact)));
}

QJsonObject McpServer::tool_start_capture(const QJsonObject &args)
{
	const bool instant = args.value("instant").toBool(false);
	const bool ok = _session->start_capture(instant);
	return text_result(ok ? "Capture started." : "Failed to start capture.",
			!ok);
}

QJsonObject McpServer::tool_stop_capture(const QJsonObject &)
{
	// Stop on an idle session is a no-op, not a failure: report success so
	// clients can call stop unconditionally in a cleanup path.
	if (!_session->is_working())
		return text_result("Capture stopped.", false);

	const bool ok = _session->stop_capture();
	return text_result(ok ? "Capture stopped." : "Failed to stop capture.",
			!ok);
}

QJsonObject McpServer::tool_set_collect_mode(const QJsonObject &args)
{
	const int mode = args.value("mode").toInt(-1);
	if (mode < 0 || mode > 2)
		return text_result("Invalid mode; use 0=single, 1=repeat, 2=loop.",
				true);
	_session->set_collect_mode(static_cast<DEVICE_COLLECT_MODE>(mode));
	return text_result("Collect mode set to " + QString::number(mode));
}

QJsonObject McpServer::tool_list_channels(const QJsonObject &)
{
	DeviceAgent *dev = _session->get_device();
	QJsonArray arr;
	for (GSList *l = dev->get_channels(); l; l = l->next) {
		const sr_channel *ch = static_cast<const sr_channel *>(l->data);
		QJsonObject o;
		o["index"] = ch->index;
		o["name"] = QString::fromUtf8(ch->name);
		o["type"] = ch->type;
		o["enabled"] = bool(ch->enabled);
		arr.append(o);
	}
	return text_result(QString::fromUtf8(
			QJsonDocument(arr).toJson(QJsonDocument::Compact)));
}

QJsonObject McpServer::tool_set_channel_enabled(const QJsonObject &args)
{
	DeviceAgent *dev = _session->get_device();
	const int index = args.value("index").toInt(-1);
	const bool enabled = args.value("enabled").toBool();
	const sr_channel *target = NULL;
	for (GSList *l = dev->get_channels(); l; l = l->next) {
		const sr_channel *ch = static_cast<const sr_channel *>(l->data);
		if (ch->index == index) { target = ch; break; }
	}
	if (!target)
		return text_result("Channel not found: " + QString::number(index),
				true);
	const bool ok = dev->enable_probe(target, enabled);
	return text_result(ok ? "Channel updated." : "Failed to update channel.",
			!ok);
}

QJsonObject McpServer::tool_set_channel_name(const QJsonObject &args)
{
	DeviceAgent *dev = _session->get_device();
	const int index = args.value("index").toInt(-1);
	const QString name = args.value("name").toString();
	const bool ok = dev->set_channel_name(index, name.toUtf8().constData());
	return text_result(ok ? "Channel renamed." : "Failed to rename channel.",
			!ok);
}

QJsonObject McpServer::tool_set_sample_config(const QJsonObject &args)
{
	DeviceAgent *dev = _session->get_device();
	QStringList done;

	if (args.contains("sample_rate")) {
		const uint64_t rate = args.value("sample_rate")
				.toVariant().toULongLong();
		if (!dev->set_config_uint64(SR_CONF_SAMPLERATE, rate))
			return text_result("Failed to set sample rate.", true);
		done << "sample_rate";
	}
	if (args.contains("sample_limit")) {
		const uint64_t limit = args.value("sample_limit")
				.toVariant().toULongLong();
		if (!dev->set_config_uint64(SR_CONF_LIMIT_SAMPLES, limit))
			return text_result("Failed to set sample limit.", true);
		done << "sample_limit";
	}

	_session->apply_samplerate();
	return text_result("Updated: " + (done.isEmpty() ? "(nothing)" : done.join(", ")));
}

// ---------- decoder management ----------

QJsonObject McpServer::tool_add_decoder(const QJsonObject &args)
{
	const QString name = args.value("name").toString();
	srd_decoder *dec = srd_decoder_get_by_module_name(
			name.toUtf8().constData());
	if (!dec)
		dec = srd_decoder_get_by_id(name.toUtf8().constData());
	if (!dec)
		return text_result("Decoder not found: " + name, true);

	DecoderStatus *dstatus = new DecoderStatus();
	std::list<data::decode::Decoder *> subs;
	view::Trace *out = NULL;
	const bool ok = _session->add_decoder(dec, true, dstatus, subs, out);
	if (!ok) {
		delete dstatus;
		return text_result("Failed to add decoder: " + name, true);
	}

	// Re-decode against the current data.
	_session->re_decode_trace();
	return text_result("Added decoder " + name + ".");
}

QJsonObject McpServer::tool_remove_decoder(const QJsonObject &args)
{
	const int index = args.value("index").toInt(-1);
	if (index < 0)
		return text_result("Missing index.", true);
	_session->remove_decoder(index);
	return text_result("Removed decoder at index " + QString::number(index));
}

QJsonObject McpServer::tool_reset_decoder(const QJsonObject &args)
{
	const int index = args.value("index").toInt(-1);
	if (index < 0)
		return text_result("Missing index.", true);
	// Re-run decoding without the modal options dialog that rst_decoder opens.
	_session->re_decode_trace(index);
	return text_result("Reset decoder at index " + QString::number(index));
}

QJsonObject McpServer::tool_list_decoder_stacks(const QJsonObject &)
{
	QJsonArray arr;
	const auto &traces = _session->get_decode_signals();
	for (size_t i = 0; i < traces.size(); ++i) {
		data::DecoderStack *stack = traces[i]->decoder();
		QJsonObject o;
		o["trace_index"] = static_cast<int>(i);
		o["root_id"] = QString::fromUtf8(stack->get_root_decoder_id());

		QJsonArray decs;
		for (auto dec : stack->stack()) {
			QJsonObject d;
			d["id"] = dec->id();
			d["module"] = dec->module_name().c_str();

			QJsonObject opts;
			for (auto it = dec->options().begin();
					it != dec->options().end(); ++it)
				opts[QString::fromStdString(it->first)] =
						gv_to_json(it->second);
			d["options"] = opts;

			QJsonArray probes;
			for (auto &kv : dec->binded_probe_list())
				probes.append(QString::fromUtf8(kv->id));
			d["bound_channels"] = probes;

			decs.append(d);
		}
		o["decoders"] = decs;

		QJsonArray rows;
		for (int r = 0; r < stack->list_rows_size(); ++r) {
			QString title;
			stack->list_row_title(r, title);
			rows.append(title);
		}
		o["rows"] = rows;
		arr.append(o);
	}
	return text_result(QString::fromUtf8(
			QJsonDocument(arr).toJson(QJsonDocument::Compact)));
}

QJsonObject McpServer::tool_get_decoder_options(const QJsonObject &args)
{
	const int trace_index = args.value("trace_index").toInt(-1);
	view::DecodeTrace *trace = _session->get_decoder_trace(trace_index);
	if (!trace || !trace->decoder())
		return text_result("Decoder trace not found.", true);

	data::DecoderStack *stack = trace->decoder();
	QJsonArray out;
	for (auto dec : stack->stack()) {
		const srd_decoder *sd = dec->get_dec_handel();
		QJsonObject d;
		d["module"] = dec->module_name().c_str();

		QJsonArray schema;
		for (GSList *l = sd->options; l; l = l->next) {
			const srd_decoder_option *o =
					static_cast<const srd_decoder_option *>(l->data);
			QJsonObject item;
			item["id"] = o->id;
			item["type"] = g_variant_get_type_string(o->def);
			item["default"] = gv_to_json(o->def);
			QJsonArray values;
			for (GSList *v = o->values; v; v = v->next)
				values.append(gv_to_json(
						static_cast<GVariant *>(v->data)));
			item["choices"] = values;
			schema.append(item);
		}
		d["schema"] = schema;

		QJsonObject current;
		for (auto it = dec->options().begin();
				it != dec->options().end(); ++it)
			current[QString::fromStdString(it->first)] =
					gv_to_json(it->second);
		d["current"] = current;
		out.append(d);
	}
	return text_result(QString::fromUtf8(
			QJsonDocument(out).toJson(QJsonDocument::Compact)));
}

QJsonObject McpServer::tool_set_decoder_option(const QJsonObject &args)
{
	const int trace_index = args.value("trace_index").toInt(-1);
	const QString option = args.value("option").toString();
	const QJsonValue value = args.value("value");

	view::DecodeTrace *trace = _session->get_decoder_trace(trace_index);
	if (!trace || !trace->decoder())
		return text_result("Decoder trace not found.", true);

	data::DecoderStack *stack = trace->decoder();
	data::decode::Decoder *target = stack->stack().front();
	const GVariantType *ref_type = NULL;
	for (auto dec : stack->stack()) {
		const srd_decoder *sd = dec->get_dec_handel();
		for (GSList *l = sd->options; l; l = l->next) {
			const srd_decoder_option *o =
					static_cast<const srd_decoder_option *>(l->data);
			if (option == o->id) {
				target = dec;
				ref_type = g_variant_get_type(o->def);
			}
		}
	}
	if (!ref_type)
		return text_result("Option not found: " + option, true);

	GVariant *gv = json_to_gv(value, ref_type);
	target->set_option(option.toUtf8().constData(), gv);
	_session->re_decode_trace(trace_index);
	return text_result("Set " + option + " and re-decoding.");
}

QJsonObject McpServer::tool_bind_probe(const QJsonObject &args)
{
	const int trace_index = args.value("trace_index").toInt(-1);
	const QString channel = args.value("channel").toString();
	const int probe_index = args.value("probe_index").toInt(-1);

	view::DecodeTrace *trace = _session->get_decoder_trace(trace_index);
	if (!trace || !trace->decoder())
		return text_result("Decoder trace not found.", true);

	data::DecoderStack *stack = trace->decoder();
	bool found = false;
	for (auto dec : stack->stack()) {
		std::map<const srd_channel *, int> probe_map;
		// Preserve existing bindings.
		for (const srd_channel *pdch : dec->binded_probe_list())
			probe_map[pdch] = dec->binded_probe_index(pdch);

		const srd_decoder *sd = dec->get_dec_handel();
		for (GSList *l = sd->channels; l; l = l->next) {
			const srd_channel *pdch =
					static_cast<const srd_channel *>(l->data);
			if (channel == pdch->id) {
				probe_map[pdch] = probe_index;
				found = true;
			}
		}
		dec->set_probes(probe_map);
	}

	if (!found)
		return text_result("Decoder channel not found: " + channel, true);

	_session->re_decode_trace(trace_index);
	return text_result("Bound " + channel + " to probe " +
			QString::number(probe_index) + " and re-decoding.");
}

// ---------- export & signals ----------

static QJsonObject run_store(SigSession *s, const QJsonObject &args,
		bool is_export, ISessionDataGetter *getter)
{
	const QString path = args.value("path").toString();
	if (path.isEmpty())
		return McpServer::text_result("Missing path.", true);

	StoreSession store(s);
	store.set_file_name(path);
	if (args.contains("start") && args.contains("end"))
		store.SetDataRange(args.value("start").toVariant().toULongLong(),
				args.value("end").toVariant().toULongLong());

	if (!is_export)
		store._sessionDataGetter = getter;

	const bool started = is_export ? store.export_start() : store.save_start();
	if (!started)
		return McpServer::text_result("Failed to start store: " + store.error(), true);

	// Join the worker on a separate thread while pumping the main event
	// loop, so queued signals the save thread relies on are still delivered.
	QEventLoop loop;
	QString err;
	std::thread waiter([&]() {
		store.wait();
		err = store.error();
		QMetaObject::invokeMethod(&loop, "quit", Qt::QueuedConnection);
	});
	loop.exec();
	if (waiter.joinable())
		waiter.join();

	if (!err.isEmpty())
		return McpServer::text_result("Store failed: " + err, true);

	return McpServer::text_result((is_export ? "Exported: " : "Saved: ") + path);
}

QJsonObject McpServer::tool_save_dsl(const QJsonObject &args)
{
	return run_store(_session, args, false, _data_getter);
}

QJsonObject McpServer::tool_export_decoded(const QJsonObject &args)
{
	return run_store(_session, args, true, _data_getter);
}

QJsonObject McpServer::tool_list_signals(const QJsonObject &)
{
	QJsonArray arr;
	for (auto s : _session->get_signals()) {
		QJsonObject o;
		o["index"] = s->get_index();
		o["name"] = s->get_name();
		o["type"] = s->get_type();
		arr.append(o);
	}
	return text_result(QString::fromUtf8(
			QJsonDocument(arr).toJson(QJsonDocument::Compact)));
}

QJsonObject McpServer::tool_set_trace_name(const QJsonObject &args)
{
	const int index = args.value("index").toInt(-1);
	const QString name = args.value("name").toString();
	auto *s = _session->get_signal_by_index(index);
	if (!s)
		return text_result("Signal not found: " + QString::number(index),
				true);
	_session->set_trace_name(s, name);
	return text_result("Renamed trace " + QString::number(index) + " to " + name);
}

// ---------- cursors & measurement ----------

QJsonObject McpServer::tool_add_cursor(const QJsonObject &args)
{
	if (!_view)
		return text_result("View unavailable.", true);
	const uint64_t sample = args.value("sample_index")
			.toVariant().toULongLong();
	_view->add_cursor(sample);
	return text_result("Cursor added at sample " + QString::number(sample));
}

QJsonObject McpServer::tool_remove_cursor(const QJsonObject &args)
{
	if (!_view)
		return text_result("View unavailable.", true);
	const int index = args.value("index").toInt(-1);
	auto &list = _view->get_cursorList();
	if (index < 0 || index >= static_cast<int>(list.size()))
		return text_result("Cursor not found: " + QString::number(index),
				true);
	auto it = list.begin();
	std::advance(it, index);
	_view->del_cursor(*it);
	return text_result("Removed cursor " + QString::number(index));
}

QJsonObject McpServer::tool_list_cursors(const QJsonObject &)
{
	if (!_view)
		return text_result("View unavailable.", true);
	QJsonArray arr;
	int i = 0;
	for (auto *c : _view->get_cursorList()) {
		QJsonObject o;
		o["index"] = i++;
		o["sample_index"] = qint64(c->get_index());
		arr.append(o);
	}
	return text_result(QString::fromUtf8(
			QJsonDocument(arr).toJson(QJsonDocument::Compact)));
}

QJsonObject McpServer::tool_get_measurements(const QJsonObject &)
{
	if (!_view)
		return text_result("View unavailable.", true);

	// Standard measurement option names surfaced by the view.
	const QStringList options = {
		"freq", "period", "duty", "width", "min", "max",
		"rise", "fall", "vpp", "avg", "rms", "count"
	};
	QJsonObject out;
	for (const QString &opt : options) {
		const QString value = _view->get_measure(opt);
		if (!value.isEmpty() && value != "########")
			out[opt] = value;
	}
	return text_result(QString::fromUtf8(
			QJsonDocument(out).toJson(QJsonDocument::Compact)));
}

QJsonObject McpServer::tool_get_pps_config(const QJsonObject &)
{
	if (!_pps)
		return text_result("1PPS control unavailable.", true);

	return text_result(QString::fromUtf8(
			QJsonDocument(_pps->pps_describe()).toJson(QJsonDocument::Compact)));
}

QJsonObject McpServer::tool_set_pps_config(const QJsonObject &args)
{
	if (!_pps)
		return text_result("1PPS control unavailable.", true);

	QString error;
	if (!_pps->pps_configure(args, error))
		return text_result(error, true);

	return text_result(QString::fromUtf8(
			QJsonDocument(_pps->pps_describe()).toJson(QJsonDocument::Compact)));
}

QJsonObject McpServer::tool_start_pps(const QJsonObject &)
{
	if (!_pps)
		return text_result("1PPS control unavailable.", true);

	QString error;
	if (!_pps->pps_start(error))
		return text_result(error, true);

	return text_result("1PPS measurement started.");
}

QJsonObject McpServer::tool_stop_pps(const QJsonObject &)
{
	if (!_pps)
		return text_result("1PPS control unavailable.", true);

	_pps->pps_stop();

	QJsonObject status = _pps->pps_status();
	return text_result(QString::fromUtf8(
			QJsonDocument(status).toJson(QJsonDocument::Compact)));
}

QJsonObject McpServer::tool_get_pps_status(const QJsonObject &)
{
	if (!_pps)
		return text_result("1PPS control unavailable.", true);

	return text_result(QString::fromUtf8(
			QJsonDocument(_pps->pps_status()).toJson(QJsonDocument::Compact)));
}

} // namespace mcp
} // namespace pv
