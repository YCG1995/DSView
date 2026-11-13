/*
 * Android / no-Python 构建下的解码器桩实现。
 *
 * 背景：DSView 的解码器（协议解码）完全建立在 libsigrokdecode + 嵌入式 CPython
 * 之上。Android 版首次移植时没有把 CPython 交叉编译进来，于是：
 *   - libsigrokdecode4DSL 的源码（大量 Py* 调用）
 *   - DSView/pv/data/decode/decoder.cpp（#include <Python.h>）
 * 都被 CMake 的 Android 分支从目标里剔除。
 *
 * 但界面层（decoder 面板、sigsession）仍在引用这些入口，必须给出一组
 * "空实现"才能链接出 .so。运行时表现：解码列表为空、无法添加解码器，
 * 其余功能（采集 / 波形 / 1PPS / MCP）不受影响。
 *
 * 后续把 CPython for Android 交叉编译进来（CMake 里 -DDSVIEW_ANDROID_PYTHON=ON）
 * 后，本文件会自动被排除，解码器恢复真实实现。
 */
#ifdef DSVIEW_ANDROID_NO_PYTHON
#include <glib.h>
// 必须放在 decoder.h 之前：decoder.h 里有基于 srd_channel / srd_decoder
// 成员的内联函数，先拿到完整结构定义才编得过
#include "../../../../libsigrokdecode4DSL/libsigrokdecode.h"
#include "decoder.h"

#include <string>
#include <map>
#include <vector>

namespace pv {
namespace data {
namespace decode {

Decoder::Decoder(const srd_decoder *const decoder) :
	_decoder(decoder),
	_decode_start(0),
	_decode_end(0),
	_decode_start_back(0),
	_decode_end_back(0),
	_setted(false),
	_shown(true)
{
}

Decoder::~Decoder()
{
}

int Decoder::first_probe_index()
{
	if (_probes.empty())
		return -1;
	return _probes.begin()->second;
}

bool Decoder::is_binded_probe(const srd_channel *const pdch)
{
	return _probes.find(pdch) != _probes.end();
}

int Decoder::binded_probe_index(const srd_channel *const pdch)
{
	std::map<const srd_channel*, int>::const_iterator it = _probes.find(pdch);
	if (it == _probes.end())
		return -1;
	return it->second;
}

std::vector<const srd_channel*> Decoder::binded_probe_list()
{
	std::vector<const srd_channel*> list;
	for (std::map<const srd_channel*, int>::const_iterator it = _probes.begin();
			it != _probes.end(); it++)
		list.push_back(it->first);
	return list;
}

void Decoder::set_probes(std::map<const srd_channel*, int> probes)
{
	_probes = probes;
	_probes_back = probes;
}

void Decoder::set_option(const char *id, GVariant *value)
{
	if (!id)
		return;
	_options[id] = value;
	_options_back[id] = value;
}

bool Decoder::have_required_probes()
{
	return true;
}

srd_decoder_inst* Decoder::create_decoder_inst(srd_session *session)
{
	(void)session;
	return nullptr;
}

void Decoder::set_decode_region(uint64_t start, uint64_t end)
{
	_decode_start = start;
	_decode_end = end;
	_decode_start_back = start;
	_decode_end_back = end;
}

bool Decoder::commit()
{
	return false;
}

std::string Decoder::module_name() const
{
	if (_decoder && _decoder->id)
		return std::string(_decoder->id);
	return std::string();
}

std::map<std::string, int> Decoder::export_probe_ids() const
{
	return std::map<std::string, int>();
}

void Decoder::attach_decoder(const srd_decoder *dec,
		const std::map<std::string, int> &probe_ids)
{
	_decoder = dec;
	_probes.clear();
	_probes_back.clear();
	_options.clear();
	_options_back.clear();
	(void)probe_ids;
}

} // namespace decode
} // namespace data
} // namespace pv

extern "C" {

const GSList *srd_decoder_list(void)
{
	return nullptr;
}

struct srd_decoder *srd_decoder_get_by_module_name(const char *name)
{
	(void)name;
	return nullptr;
}

int srd_decoder_reload(const char *id)
{
	(void)id;
	return 0;
}

int srd_decoder_check(const char *module_name, char **error)
{
	(void)module_name;
	if (error)
		*error = nullptr;
	return 0;
}

void srd_console_write(const char *text) { (void)text; }

/* ---- 会话级接口：Android 无 Python 时全部按"空会话"处理 ---- */

int srd_init(const char *path) { (void)path; return 0; }
int srd_exit(void) { return 0; }
int srd_decoder_load_all(void) { return 0; }
int srd_decoder_unload_all(void) { return 0; }

struct srd_decoder *srd_decoder_get_by_id(const char *id)
{
	(void)id;
	return nullptr;
}

int srd_session_start(struct srd_session *sess, char **error)
{
	(void)sess; (void)error;
	return 0;
}

int srd_session_metadata_set(struct srd_session *sess, int key, GVariant *data)
{
	(void)sess; (void)key; (void)data;
	return 0;
}

int srd_session_send(struct srd_session *sess,
		uint64_t abs_start_samplenum, uint64_t abs_end_samplenum,
		const uint8_t **inbuf, const uint8_t *inbuf_const,
		uint64_t inbuflen, char **error)
{
	(void)sess; (void)abs_start_samplenum; (void)abs_end_samplenum;
	(void)inbuf; (void)inbuf_const; (void)inbuflen; (void)error;
	return 0;
}

int srd_session_terminate_reset(struct srd_session *sess) { (void)sess; return 0; }
int srd_session_destroy(struct srd_session *sess) { (void)sess; return 0; }
int srd_session_end(struct srd_session *sess, char **error) { (void)sess; (void)error; return 0; }

int srd_pd_output_callback_add(struct srd_session *sess, int output_type,
		srd_pd_output_callback cb, void *cb_data)
{
	(void)sess; (void)output_type; (void)cb; (void)cb_data;
	return 0;
}

char *srd_console_take(int do_clear) { (void)do_clear; return nullptr; }
void srd_console_clear(void) { }
void srd_log_set_context(xlog_context *ctx) { (void)ctx; }

int srd_session_new(struct srd_session **sess)
{
	if (sess)
		*sess = nullptr;
	return 0;
}

int srd_inst_stack(struct srd_session *sess,
		struct srd_decoder_inst *di_from, struct srd_decoder_inst *di_to)
{
	(void)sess; (void)di_from; (void)di_to;
	return 0;
}

struct srd_decoder_inst *srd_inst_find_by_id(struct srd_session *sess,
		const char *inst_id)
{
	(void)sess; (void)inst_id;
	return nullptr;
}

} // extern "C"

#endif // DSVIEW_ANDROID_NO_PYTHON
