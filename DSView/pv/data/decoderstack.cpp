/*
 * This file is part of the PulseView project.
 * DSView is based on PulseView.
 * 
 * Copyright (C) 2012 Joel Holdsworth <joel@airwebreathe.org.uk>
 * Copyright (C) 2014 DreamSourceLab <support@dreamsourcelab.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 */
  

#include <stdexcept>
#include <algorithm>
#include <assert.h>

#include "decoderstack.h"
#include "logicsnapshot.h"
#include "decode/decoder.h"
#include "decode/annotation.h"
#include "decode/rowdata.h"
#include "../sigsession.h"
#include "../view/logicsignal.h"
#include "../dsvdef.h"
#include "../log.h"
#include "../ui/langresource.h"
#include <ds_types.h>

using namespace pv::data::decode;
using namespace std;
using namespace boost;

namespace pv {
namespace data {

const double DecoderStack::DecodeMargin = 1.0;
const double DecoderStack::DecodeThreshold = 0.2;
const int64_t DecoderStack::DecodeChunkLength = 4 * 1024; 
const unsigned int DecoderStack::DecodeNotifyPeriod = 1024;
 
DecoderStack::DecoderStack(pv::SigSession *session,
	const srd_decoder *const dec, DecoderStatus *decoder_status) :
	_session(session)
{
    assert(session);
    assert(dec);
    assert(decoder_status); 
    
    _samples_decoded = 0;
    _sample_count = 0; 
    _decode_state = Stopped;
    _options_changed = false;
    _no_memory = false;
    _mark_index = -1;
    _decoder_status = decoder_status;
    _stask_stauts = NULL; 
    _is_capture_end = true;
    _snapshot = NULL;
    _progress = 0;
    _is_decoding = false;
    _result_count = 0;
    
    _stack.push_back(new decode::Decoder(dec));
 
    build_row();
}

DecoderStack::~DecoderStack()
{   
    //release resource talbe
    DESTROY_OBJECT(_decoder_status);

    //release source
    for (auto &kv : _rows)
    {
        kv.second->clear(); //destory all annotations
        delete kv.second;
    }
    _rows.clear();

    //Decoder
    for (auto *p : _stack){
        delete p;
    }
    _stack.clear();
    
    _rows_gshow.clear();
    _rows_lshow.clear();
    _class_rows.clear();
}
 
void DecoderStack::add_sub_decoder(decode::Decoder *decoder)
{
	assert(decoder);
	_stack.push_back(decoder);
    build_row();
    _options_changed = true;
}

void DecoderStack::remove_sub_decoder(Decoder *decoder)
{
	// Find the decoder in the stack
    auto  iter = _stack.begin();
    for(unsigned int i = 0; i < _stack.size(); i++, iter++)
        if ((*iter) == decoder)
            break;

	// Delete the element
    if (iter != _stack.end())
    {
        _stack.erase(iter);
        delete decoder;
    }        

    build_row();
    _options_changed = true;
}

void DecoderStack::remove_decoder_by_handel(const srd_decoder *dec)
{
    Decoder *decoder = NULL;

    for (auto d : _stack){
        if (d->get_dec_handel() == dec){
            decoder = d;
            break;
        }
    }

    if (decoder){
        remove_sub_decoder(decoder);
    }
}

void DecoderStack::build_row()
{
    //release source
    for (auto &kv : _rows)
    {   
        kv.second->clear(); //destory all annotations
        delete kv.second;
    }
    _rows.clear();

    // Drop the class -> row map as well: its keys hold the decoder definition
    // the rows were built from (see Row), and a reloaded script replaces that
    // definition, so entries from the previous version must not survive.
    _class_rows.clear();

    // Add classes
    for (auto dec : _stack)
    { 
        const srd_decoder *const decc = dec->decoder();
        assert(dec->decoder());

        dec->reset_start();

        // Add a row for the decoder if it doesn't have a row list
        if (!decc->annotation_rows) {
            const Row row(decc);
            _rows[row] = new decode::RowData();
            std::map<const decode::Row, bool>::const_iterator iter = _rows_gshow.find(row);
            if (iter == _rows_gshow.end()) {
                _rows_gshow[row] = true;
                if (row.title().contains("bit", Qt::CaseInsensitive) ||
                    row.title().contains("warning", Qt::CaseInsensitive)) {
                    _rows_lshow[row] = false;
                } else {
                    _rows_lshow[row] = true;
                }
            }
        }

        // Add the decoder rows
        int order = 0;
        for (const GSList *l = decc->annotation_rows; l; l = l->next)
        {
            const srd_decoder_annotation_row *const ann_row =
                (srd_decoder_annotation_row *)l->data;
            assert(ann_row);

            const Row row(decc, ann_row, order);

            // Add a new empty row data object
            _rows[row] = new decode::RowData();
            std::map<const decode::Row, bool>::const_iterator iter = _rows_gshow.find(row);
            if (iter == _rows_gshow.end()) {
                _rows_gshow[row] = true;
                if (row.title().contains("bit", Qt::CaseInsensitive) ||
                    row.title().contains("warning", Qt::CaseInsensitive)) {
                    _rows_lshow[row] = false;
                } else {
                    _rows_lshow[row] = true;
                }
            }

            // Map out all the classes
            for (const GSList *ll = ann_row->ann_classes; ll; ll = ll->next){
                _class_rows[make_pair(decc, GPOINTER_TO_INT(ll->data))] = Row(row);
            }

            order++;
        }
    }
}

int64_t DecoderStack::samples_decoded()
{
    std::lock_guard<std::mutex> decode_lock(_output_mutex);
	return _samples_decoded;
}

void DecoderStack::get_annotation_subset(
	std::vector<pv::data::decode::Annotation*> &dest,
	const Row &row, uint64_t start_sample,
	uint64_t end_sample)
{  
    auto iter = _rows.find(row);
    if (iter != _rows.end())
        (*iter).second->get_annotation_subset(dest,
			start_sample, end_sample);
}


uint64_t DecoderStack::get_annotation_index(
    const Row &row, uint64_t start_sample)
{  
    uint64_t index = 0;
    auto iter = _rows.find(row);
    if (iter != _rows.end())
        index = (*iter).second->get_annotation_index(start_sample);

    return index;
}

uint64_t DecoderStack::get_max_annotation(const Row &row)
{ 
    auto iter =  _rows.find(row);
    if (iter != _rows.end())
        return (*iter).second->get_max_annotation();

    return 0;
}

uint64_t DecoderStack::get_min_annotation(const Row &row)
{  
    auto iter = _rows.find(row);
    if (iter != _rows.end())
        return (*iter).second->get_min_annotation();

    return 0;
}

std::map<const decode::Row, bool> DecoderStack::get_rows_gshow()
{
    std::map<const decode::Row, bool> rows_gshow;
    for (std::map<const decode::Row, bool>::const_iterator i = _rows_gshow.begin();
        i != _rows_gshow.end(); i++) {
        rows_gshow[(*i).first] = (*i).second;
    }
    return rows_gshow;
}

std::map<const decode::Row, bool> DecoderStack::get_rows_lshow()
{
    std::map<const decode::Row, bool> rows_lshow;
    for (std::map<const decode::Row, bool>::const_iterator i = _rows_lshow.begin();
        i != _rows_lshow.end(); i++) {
        rows_lshow[(*i).first] = (*i).second;
    }
    return rows_lshow;
}

void DecoderStack::set_rows_gshow(const decode::Row row, bool show)
{
    std::map<const decode::Row, bool>::const_iterator iter = _rows_gshow.find(row);
    if (iter != _rows_gshow.end()) {
        _rows_gshow[row] = show;
    }
}

void DecoderStack::set_rows_lshow(const decode::Row row, bool show)
{
    std::map<const decode::Row, bool>::const_iterator iter = _rows_lshow.find(row);
    if (iter != _rows_lshow.end()) {
        _rows_lshow[row] = show;
    }
}

bool DecoderStack::has_annotations(const Row &row)
{  
    auto iter =
        _rows.find(row);
    if (iter != _rows.end())
        if(0 == (*iter).second->get_max_sample())
            return false;
        else
            return true;
    else
        return false;
}

uint64_t DecoderStack::list_annotation_size()
{
    std::lock_guard<std::mutex> lock(_output_mutex);
    uint64_t max_annotation_size = 0;

    for (auto it = _rows.begin(); it != _rows.end(); it++) {
        auto iter = _rows_lshow.find((*it).first);
        if (iter != _rows_lshow.end() && (*iter).second){
            max_annotation_size = max(max_annotation_size,
                (*it).second->get_annotation_size());
        }
    }

    return max_annotation_size;
}

uint64_t DecoderStack::list_annotation_size(uint16_t row_index)
{ 
    for (auto i = _rows.begin(); i != _rows.end(); i++) {
        auto iter = _rows_lshow.find((*i).first);
        if (iter != _rows_lshow.end() && (*iter).second)
            if (row_index-- == 0) {
                return (*i).second->get_annotation_size();
            }
    }
    return 0;
}

bool DecoderStack::list_annotation(pv::data::decode::Annotation *ann,
                                  uint16_t row_index, uint64_t col_index)
{ 
    for (auto i = _rows.begin(); i != _rows.end(); i++) {
        auto iter = _rows_lshow.find((*i).first);
        if (iter != _rows_lshow.end() && (*iter).second) {
            if (row_index-- == 0) {
                return (*i).second->get_annotation(ann, col_index);
            }
        }
    }

    return false;
}


bool DecoderStack::list_row_title(int row, QString &title)
{ 
    for (auto i = _rows.begin();i != _rows.end(); i++) {
        auto iter = _rows_lshow.find((*i).first);
        if (iter != _rows_lshow.end() && (*iter).second) {
            if (row-- == 0) {
                title = (*i).first.title();
                return 1;
            }
        }
    }
    return 0;
}

void DecoderStack::clear()
{
    init();
}

void DecoderStack::init()
{
    _sample_count = 0; 
    _samples_decoded = 0;
    _error_message = QString();
    _no_memory = false;
    _snapshot = NULL;
    _result_count = 0;

    for (auto i = _rows.begin();i != _rows.end(); i++) { 
        (*i).second->clear();
    }

    set_mark_index(-1);
}
 
void DecoderStack::stop_decode_work()
{  
    //set the flag to exit from task thread 
     if (_stask_stauts){
         _stask_stauts->_bStop = true;
     }
    _decode_state = Stopped; 
}

void DecoderStack::begin_decode_work()
{
     assert(_decode_state == Stopped);

     _error_message = "";
     _decode_state = Running;
      do_decode_work();
     _decode_state = Stopped;
}

bool DecoderStack::check_required_probes()
{
    for(auto dec : _stack){
		if (!dec->have_required_probes()) {
			return false;
		}
    }

    return true;
}

// Log the binding state behind a failed decode. The row message is the same no
// matter whether a probe entry is missing or the capture carried no logic data,
// which is what made "channel not enabled" so hard to read.
void DecoderStack::log_probe_diagnosis(const char *reason)
{
    int logic_total = 0, logic_enabled = 0;
    if (_session) {
        for (auto s : _session->get_signals()) {
            if (s->signal_type() != SR_CHANNEL_LOGIC)
                continue;
            ++logic_total;
            if (s->enabled())
                ++logic_enabled;
        }
    }

    dsv_err("decode aborted (%s): logic signals=%d enabled=%d traces=%d",
            reason, logic_total, logic_enabled, (int)_stack.size());

    for (auto dec : _stack) {
        const srd_decoder *def = dec->get_dec_handel();
        if (!def) {
            dsv_err("  decoder without definition");
            continue;
        }
        dsv_err("  %s required=%d optional=%d bound=%d",
                dec->id(), (int)g_slist_length((GSList *)def->channels),
                (int)g_slist_length((GSList *)def->opt_channels),
                (int)dec->binded_probe_list().size());
    }
}

// Bind the required channels that have no probe entry at all. Only the root
// decoder receives samples, so only its map is completed. This mirrors what
// SigSession::add_decoder() does for a freshly added decoder: a stack that came
// back from the saved session, or one whose required channel list differs from
// the binding that was stored, otherwise dead-locks on
// "One or more required channels have not been specified" and never decodes
// again - the symptom users describe as "the demo device cannot auto decode".
bool DecoderStack::ensure_required_probes()
{
    if (_session == nullptr || _stack.empty())
        return false;

    decode::Decoder *root = _stack.front();
    const srd_decoder *def = root ? root->get_dec_handel() : nullptr;
    if (def == nullptr)
        return false;

    // Channels this device really carries, in the order the session lists them.
    std::vector<int> avail;
    for (auto s : _session->get_signals()) {
        if (s->signal_type() != SR_CHANNEL_LOGIC || !s->enabled())
            continue;
        const int idx = s->get_index();
        if (std::find(avail.begin(), avail.end(), idx) == avail.end())
            avail.push_back(idx);
    }
    if (avail.empty())
        return false;

    std::map<const srd_channel *, int> probes;
    std::vector<int> used;
    for (auto pdch : root->binded_probe_list()) {
        const int idx = root->binded_probe_index(pdch);
        if (idx < 0)
            continue;
        probes[pdch] = idx;
        used.push_back(idx);
    }

    bool added = false;
    QStringList new_bindings;
    for (GSList *l = def->channels; l; l = l->next) {
        const srd_channel *pdch = static_cast<const srd_channel *>(l->data);
        if (probes.find(pdch) != probes.end())
            continue;

        int candidate = -1;
        for (int idx : avail) {
            if (std::find(used.begin(), used.end(), idx) == used.end()) {
                candidate = idx;
                break;
            }
        }
        // Every channel is already taken by another probe of this decoder:
        // sharing one is what srd accepts, so keep the decode running.
        if (candidate < 0)
            candidate = avail[(int)used.size() % (int)avail.size()];

        probes[pdch] = candidate;
        used.push_back(candidate);
        added = true;
        new_bindings.push_back(QString("%1->%2").arg(QString::fromUtf8(pdch->id))
                                   .arg(candidate));
    }

    if (!added)
        return false;

    root->set_probes(probes);
    root->commit();

    dsv_info("Decode: decoder %s was missing a channel binding, bound %s",
            root->id(), qPrintable(new_bindings.join(", ")));
    srd_console_write(qPrintable(
            QString("[decode] 解码器 %1 缺少通道绑定，已自动绑定 %2\n")
                    .arg(QString::fromUtf8(root->id()), new_bindings.join(", "))));
    return true;
}

void DecoderStack::do_decode_work()
{
    //set the flag to exit from task thread 
     if (_stask_stauts){
         _stask_stauts->_bStop = true;
     }
     _stask_stauts = new decode_task_status();
     _stask_stauts->_bStop = false;
     _stask_stauts->_decoder = this;
     _decoder_status->clear(); //clear old items

    if (!_options_changed)
    {  
        // Nothing to do: this task only repeats the run that produced the
        // current result. Drop the error text of that run too - after a new
        // capture the message ("no channel enabled", "no data") describes
        // data that is already gone, and the view keeps painting it because
        // DecodeTrace::paint_mid() reads error_message() unconditionally.
        if (!_error_message.isEmpty()) {
            dsv_info("Clear stale decode error: %s",
                    _error_message.toUtf8().constData());
            _error_message.clear();
            _decoder_status->clear();
        }
        dsv_err("ERROR:Decoder options have not changed.");
        return;
    } 
    _options_changed = false;

    init();

    _snapshot = NULL;

    // A decoder restored with the session (or one whose channel list changed
    // with the device) may be missing the probe entry of a required channel.
    // srd would abort the whole decode with "channel not enabled", so bind the
    // missing ones to channels of the current device first instead of failing.
    ensure_required_probes();

	// Check that all decoders have the required channels
    if (!check_required_probes()) {
        // The row only says "a required channel is not enabled" - log which
        // decoder lost which channel, and how many logic channels the session
        // actually offers, so the cause is visible in the log instead of guessed.
        log_probe_diagnosis("required probes missing");
        _error_message = L_S(STR_PAGE_MSG, S_ID(IDS_MSG_DECODERSTACK_DECODE_WORK_ERROR),
                            "One or more required channels have not been specified");
        dsv_err("ERROR:%s", _error_message.toStdString().c_str());
        return;
	}

	// We get the logic data of the first channel in the list.
	// This works because we are currently assuming all
	// LogicSignals have the same data/snapshot
    for (auto dec : _stack) {
        if (dec->have_probes()) {
            for(auto s :  _session->get_signals()) {
                if(s->get_index() == dec->first_probe_index() && s->signal_type() == SR_CHANNEL_LOGIC)
                { 
                    _snapshot = ((pv::view::LogicSignal*)s)->data();
                    if (_snapshot != NULL)
                        break;
                }
            }
            // The binding may point at a channel this capture no longer carries
            // (narrower channel range or a rate that offers fewer channels). All
            // logic signals of a capture share one snapshot, so take it from any
            // of them and let rebind_stale_probes() repair the binding below.
            if (_snapshot == NULL) {
                for(auto s :  _session->get_signals()) {
                    if (s->signal_type() != SR_CHANNEL_LOGIC)
                        continue;
                    _snapshot = ((pv::view::LogicSignal*)s)->data();
                    if (_snapshot != NULL)
                        break;
                }
            }
            if (_snapshot != NULL)
                break;
        }
    }

	if (_snapshot == NULL)
    {   
        log_probe_diagnosis("no logic snapshot");
        _error_message = L_S(STR_PAGE_MSG, S_ID(IDS_MSG_DECODERSTACK_DECODE_WORK_ERROR),
                             "One or more required channels have not been specified");
        dsv_err("ERROR:%s", _error_message.toStdString().c_str());
        return;
    }		

    if (_session->is_realtime_refresh() == false && _snapshot->empty())
    { 
        // Nothing has been captured yet, or the buffer is still filling up.
        // This is not a decoder failure, so do not paint an error onto the row:
        // a red "No data to decode" line right after adding the decoder (or
        // right after pressing start) was the stale-looking message users
        // reported. The decode is simply deferred to the end of the capture.
        _error_message.clear();
        return;
    }

    // The decoder may have been bound to a channel the previous capture carried
    // and this one does not (narrower channel range, a rate that offers fewer
    // channels, a channel switched off). Decoding would then abort with "At
    // least one of selected channels are not enabled"; rebind those probes
    // first so the automatic decode after a trigger keeps working.
    rebind_stale_probes();

    // Get the samplerate
	_samplerate = _snapshot->samplerate();
    if (_samplerate == 0.0)
    {
        dsv_err("ERROR:Decode data got an invalid sample rate.");
        return;
    }
     
    execute_decode_stack();   
}

uint64_t DecoderStack::get_max_sample_count()
{
	uint64_t max_sample_count = 0;

    for (auto i = _rows.begin(); i != _rows.end(); i++){
        max_sample_count = max(max_sample_count, (*i).second->get_max_sample());
    } 	

	return max_sample_count;
}

void DecoderStack::decode_data(const uint64_t decode_start, const uint64_t decode_end, srd_session *const session)
{
    decode_task_status *status = _stask_stauts;

    //uint8_t *chunk = NULL;
    uint64_t last_cnt = 0;
    uint64_t notify_cnt = (decode_end - decode_start + 1)/100;
    srd_decoder_inst *logic_di = NULL;

    // find the first level decoder instant
    for (GSList *d = session->di_list; d; d = d->next) {
        srd_decoder_inst *di = (srd_decoder_inst *)d->data;
        srd_decoder *decoder = di->decoder;
        const bool have_probes = (decoder->channels || decoder->opt_channels) != 0;
        if (have_probes) {
            logic_di = di;
            break;
        }
    }

    assert(logic_di);

    uint64_t i = decode_start;
    char *error = NULL; 
    bool bError = false;
    bool bEndTime = false;
    int wait_loops = 0;

    if( i >= decode_end){
        dsv_info("decode data index have been to end");
    }

    std::vector<const uint8_t *> chunk;
    std::vector<uint8_t> chunk_const;
    bool bCheckEnd = false;
    uint64_t end_index = decode_end;
    uint64_t decoded_sample_count = 0;

    _progress = 0;
    _is_decoding = true;

    void* lbp_array[35];

    for (int j =0 ; j < logic_di->dec_num_channels; j++){
        lbp_array[j] = NULL;
    }

    while(i <= end_index && !_no_memory && !status->_bStop)
    {
        chunk.clear();
        chunk_const.clear();

        if (_is_capture_end)
        {
            // Samples that are really there, not the ring write position (see
            // execute_decode_stack()).
            uint64_t align_sample_count = _snapshot->get_sample_count();

            if (align_sample_count == 0){
                // A capture in progress has not delivered its first frame yet.
                // Wait for it instead of silently dropping the decode: the task
                // runs once per capture/file and nothing would re-trigger it.
                if (status->_bStop || ++wait_loops > 50) {
                    dsv_info("Have no data to decode (waited %d ms).",
                            wait_loops * 200);
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                continue;
            }

            if (!bCheckEnd){
                bCheckEnd = true;

                if (end_index >= align_sample_count){
                    end_index = align_sample_count - 1;
                    dsv_info("Reset the decode end sample index, new:%llu, old:%llu", 
                        (u64_t)end_index, (u64_t)decode_end);
                }

                if (i >= align_sample_count){
                    dsv_info("ERROR: the decoding sample index is out of range.");
                    break;
                }
            }
        }
        else if (i >= _snapshot->get_sample_count())
        {   
            // Wait the data is ready.
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }
 
        uint64_t chunk_end = end_index;

        for (int j =0 ; j < logic_di->dec_num_channels; j++) {
            int sig_index = logic_di->dec_channelmap[j];
            void *lbp = NULL;

            if (sig_index == -1) {
                chunk.push_back(NULL);
                chunk_const.push_back(0);
            }
            else {
                if (_snapshot->has_data(sig_index)) {
                    const uint8_t *data_ptr = _snapshot->get_samples(i, chunk_end, sig_index, &lbp);
                    bool flag = _snapshot->get_sample(i, sig_index);
                    chunk.push_back(data_ptr);
                    chunk_const.push_back(flag);

                    if (_snapshot->is_able_free() == false)
                    {
                        if (lbp_array[j] != lbp){
                            if (lbp_array[j] != NULL)
                                _snapshot->free_decode_lpb(lbp_array[j]);
                            lbp_array[j] = lbp;
                        }
                    }
                }
                else {
                    _error_message = L_S(STR_PAGE_MSG, S_ID(IDS_MSG_DECODERSTACK_DECODE_DATA_ERROR),
                                     "At least one of selected channels are not enabled.");
                    // Tell the console exactly which channel the decoder wanted
                    // and which ones this capture actually carries: without it
                    // the only visible symptom is the row error.
                    QString msg = QString("[decode] 解码器 %1 需要通道 %2，"
                                          "但本次采集没有这个通道的数据；可用通道：")
                            .arg(QString::fromUtf8(logic_di->decoder->id))
                            .arg(sig_index);
                    bool any = false;
                    for (auto s : _session->get_signals()) {
                        if (s->signal_type() != SR_CHANNEL_LOGIC)
                            continue;
                        const int idx = s->get_index();
                        if (!_snapshot->has_data(idx))
                            continue;
                        msg += (any ? ", " : "") + QString::number(idx);
                        any = true;
                    }
                    srd_console_write(qPrintable(msg + (any ? "\n" : "（无）\n")));
                    dsv_err("%s", qPrintable(msg));
                    return;
                }
            }
        }

        if (i > end_index){
            bEndTime = true;
            dsv_info("Decoding data to end.");
            break;
        }

        if (chunk_end >= end_index)
            chunk_end = end_index + 1;
        if (chunk_end - i > MaxChunkSize)
            chunk_end = i + MaxChunkSize;

        bEndTime = (chunk_end > end_index);

        if (srd_session_send(
                session,
                i,
                chunk_end,
                chunk.data(),
                chunk_const.data(),
                chunk_end - i,
                &error) != SRD_OK){

            if (error){
                _error_message = QString::fromLocal8Bit(error);
                dsv_err("ERROR: Failed to call srd_session_send:%s", error);
                srd_console_write(error);
                srd_console_write("\n");
                g_free(error);
                error = NULL;
            }

            bError = true;
            break;
        }

        decoded_sample_count += chunk_end - i; 
        _progress = end_index ? (int)(decoded_sample_count * 100 / end_index) : 100;
        i = chunk_end;   
 
        //use mutex
        {
            std::lock_guard<std::mutex> lock(_output_mutex);
            _samples_decoded = i - decode_start + 1;
        }

        if ((i - last_cnt) > notify_cnt) {
            last_cnt = i;
            new_decode_data();
        }
    }

    _progress = 100;
    _is_decoding = false;
    
    new_decode_data();

    // the task is normal ends,so all samples was processed;
    if (!bError && bEndTime){
       srd_session_end(session, &error);

        if (error != NULL){
            _error_message = QString::fromLocal8Bit(error);
            dsv_err("Failed to call srd_session_end:%s", error);
        }
    }

    if (error != NULL){
        g_free(error);
    }
  
    if (!_session->is_closed()){
        decode_done();
    }

    dsv_info("Decoded sample count:%llu", decoded_sample_count);
}

void DecoderStack::rebind_stale_probes()
{
    // Only the root decoder receives the sample data; a stacked sub decoder's
    // channels are fed from its parent, so leaving its bindings alone is both
    // harmless and required (a "repair" there would break the stack).
    if (_snapshot == nullptr || _session == nullptr || _stack.empty())
        return;

    decode::Decoder *root = _stack.front();
    if (root == nullptr || !root->have_probes())
        return;

    // Channels that really carry data in this capture.
    std::vector<int> avail;
    for (auto s : _session->get_signals()) {
        if (s->signal_type() != SR_CHANNEL_LOGIC)
            continue;
        const int idx = s->get_index();
        if (_snapshot->has_data(idx) &&
                std::find(avail.begin(), avail.end(), idx) == avail.end())
            avail.push_back(idx);
    }
    if (avail.empty()) {
        dsv_info("Decode: no channel of this capture carries data, probes left "
            "untouched");
        return;
    }

    std::map<const srd_channel *, int> probes;
    bool changed = false;

    for (auto pdch : root->binded_probe_list()) {
        const int idx = root->binded_probe_index(pdch);
        if (idx >= 0 && _snapshot->has_data(idx))
            probes[pdch] = idx;
        else
            changed = true;
    }
    dsv_info("Decode: %s probes=%d available=%d changed=%d",
        root->id(), (int)root->binded_probe_list().size(), (int)avail.size(),
        changed ? 1 : 0);

    // A required channel whose binding went stale gets a channel that exists,
    // so the decode runs instead of aborting with "channel not enabled".
    // Optional channels (def->opt_channels) also get a dec_channelmap entry in
    // srd_decoder_inst_new(), so a stale binding there aborts the decode just
    // the same - repair them too.
    const srd_decoder *def = root->get_dec_handel();
    int next = 0;
    auto repair = [&](GSList *chans) {
        for (GSList *l = chans; l; l = l->next) {
            const srd_channel *pdch = static_cast<const srd_channel *>(l->data);
            if (probes.find(pdch) != probes.end())
                continue;
            probes[pdch] = avail[next % (int)avail.size()];
            ++next;
            changed = true;
        }
    };
    if (def) {
        repair(def->channels);
        repair(def->opt_channels);
    }

    if (!changed)
        return;

    root->set_probes(probes);
    root->commit();
    dsv_info("Decode: decoder %s had a stale channel binding, rebound to the "
        "channels present in this capture", root->id());

    // Make the repair visible in the debug console: without it the decode
    // silently reads a different channel than the one the user configured.
    // Report the bindings that are actually in effect now, not the set of
    // channels this capture happens to carry.
    QStringList bound;
    for (auto pdch : root->binded_probe_list())
        bound.push_back(QString::number(root->binded_probe_index(pdch)));
    QString msg = QString("[decode] 解码器 %1 绑定的通道不在本次采集中，已自动改绑到通道 %2")
            .arg(QString::fromUtf8(root->id()), bound.join(", "));
    srd_console_write(qPrintable(msg + "\n"));
}

void DecoderStack::execute_decode_stack()
{  
	srd_session *session = NULL;
	srd_decoder_inst *prev_di = NULL;
    uint64_t decode_start = 0;
    uint64_t decode_end = 0;

	assert(_snapshot);

	// Create the session
    // one decoderstatck onwer one session
    // all decoderstatck execute in sequence
	srd_session_new(&session);

    if (session == NULL){
        dsv_err("Failed to call srd_session_new()");
        assert(false);
    }
    
    // Get the intial sample count. This must be the number of samples that are
    // actually in the buffer: get_ring_sample_count() is the ring write
    // position and stays 0 until the buffer wraps around, so a capture that
    // fits in the buffer (every demo capture, and any file that was opened)
    // used to look like "no data" and the decode was skipped silently.
    _sample_count = _snapshot->get_sample_count();
 
    // Create the decoders
    for(auto dec : _stack)
	{
        srd_decoder_inst *const di = dec->create_decoder_inst(session);

		if (!di)
		{
			_error_message =L_S(STR_PAGE_MSG, S_ID(IDS_MSG_DECODERSTACK_DECODE_STACK_ERROR), 
                            "Failed to create decoder instance");
			srd_session_destroy(session);
			return;
		}

		if (prev_di)
			srd_inst_stack (session, prev_di, di);

		prev_di = di;
        decode_start = dec->decode_start();

        if (_session->is_realtime_refresh() == false) {
            // Only clamp when the buffer already holds samples; with an empty
            // buffer _sample_count - 1 wraps around to UINT64_MAX.
            decode_end = (_sample_count > 0)
                    ? min(dec->decode_end(), _sample_count - 1)
                    : dec->decode_end();
        }
        else {
            decode_end = max(dec->decode_end(), decode_end);
        }
	}

    // Debug mode: only decode the leading part of the data.
    if (_debug_sample_limit > 0 && decode_end > decode_start + _debug_sample_limit - 1) {
        decode_end = decode_start + _debug_sample_limit - 1;
        dsv_info("Decoder debug limit active, decoding %llu samples.",
                (u64_t)_debug_sample_limit);
    }

    dsv_info("Decode start sample index:%llu, end sample index:%llu, count:%llu"
             " (samples=%llu total=%llu dec_end=%llu)",
            (u64_t)decode_start, (u64_t)decode_end, (u64_t)(decode_end - decode_start + 1),
            (u64_t)_sample_count,
            (u64_t)_snapshot->get_total_sample_count(),
            (u64_t)_stack.empty() ? 0 : (u64_t)_stack.front()->decode_end());

    // Nothing captured yet: decode_data() waits for the first samples (the demo
    // device delivers them a moment after the task starts) and only gives up
    // after a timeout, so an empty buffer is not an error state.
    if (_sample_count == 0) {
        dsv_info("Decode waiting for the first samples (decode %llu..%llu)",
                (u64_t)decode_start, (u64_t)decode_end);
    }

    if (decode_end < decode_start) {
        // A real decode range problem (cursor limit or region collapse) - say
        // so instead of blaming a channel.
        _error_message = L_S(STR_PAGE_MSG, S_ID(IDS_MSG_NO_DATA),
                "No data to decode");
        srd_console_write(qPrintable(
                QString("[decode] 解码区间为空（%1..%2），请重新框选\n")
                        .arg(decode_start).arg(decode_end)));
        dsv_err("Decode range is empty (%llu..%llu)",
                (u64_t)decode_start, (u64_t)decode_end);
        srd_session_destroy(session);
        return;
    }

	// Start the session
	srd_session_metadata_set(session, SRD_CONF_SAMPLERATE,
		g_variant_new_uint64((uint64_t)_samplerate));

	srd_pd_output_callback_add(
                    session, 
                    SRD_OUTPUT_ANN,
		            DecoderStack::annotation_callback,
                    _stask_stauts);

    char *error = NULL;
    if (srd_session_start(session, &error) == SRD_OK){
       //need a lot time
        decode_data(decode_start, decode_end, session);
    }
    else if (error != NULL){
        _error_message = QString::fromLocal8Bit(error);
    }

	// Destroy the session
    if (error != NULL) {
        g_free(error);
    }

	srd_session_destroy(session); 
}

uint64_t DecoderStack::sample_count()
{
    if (_snapshot)
        return _snapshot->get_sample_count();
    else
        return 0;
}

uint64_t DecoderStack::sample_rate()
{
    return _samplerate;
}

//the decode callback, annotation object will be create
void DecoderStack::annotation_callback(srd_proto_data *pdata, void *self)
{
	assert(pdata);
	assert(self);

    struct decode_task_status *st = (decode_task_status*)self;

	DecoderStack *const d = st->_decoder;
	assert(d);

    if (st->_bStop){ 
        return;
    }
    if (d->_decoder_status == NULL){ 
        dsv_err("decode task was deleted.");
        assert(false);
    }
  
    if (d->_no_memory) {
        return;
    }

    Annotation *a = new Annotation(pdata, d->_decoder_status);
    if (a == NULL){
        d->_no_memory = true;
        return;     
    }
    d->_result_count++;

	// Find the row
	assert(pdata->pdo);
	assert(pdata->pdo->di);
	const srd_decoder *const decc = pdata->pdo->di->decoder;
	assert(decc);

    auto row_iter = d->_rows.end();
	
	// Try looking up the sub-row of this class
	const map<pair<const srd_decoder*, int>, Row>::const_iterator r =
        d->_class_rows.find(make_pair(decc, a->format()));
	if (r != d->_class_rows.end())
        row_iter = d->_rows.find((*r).second);
	else
	{
		// Failing that, use the decoder as a key
        row_iter = d->_rows.find(Row(decc));
	}

    assert(row_iter != d->_rows.end());
    if (row_iter == d->_rows.end()) {
        dsv_err("Unexpected annotation: decoder = 0x%x, format = %d", (void*)decc, a->format());
        assert(0);
        return;
    }

	// Add the annotation 
    if (!(*row_iter).second->push_annotation(a))
        d->_no_memory = true; 
}
 
void DecoderStack::frame_ended()
{ 
    _options_changed = true; 
}

int DecoderStack::list_rows_size()
{ 
    int rows_size = 0;
    for (auto i = _rows.begin(); i != _rows.end(); i++) {
        auto iter = _rows_lshow.find((*i).first);
        if (iter != _rows_lshow.end() && (*iter).second)
            rows_size++;
    }
    return rows_size;
}

bool DecoderStack::options_changed()
{
    return _options_changed;
}

void DecoderStack::set_options_changed(bool changed)
{
    _options_changed = changed;
}

bool DecoderStack::out_of_memory()
{
    return _no_memory;
}

void DecoderStack::set_mark_index(int64_t index)
{
    _mark_index = index;
}

int64_t DecoderStack::get_mark_index()
{
    return _mark_index;
}

const char* DecoderStack::get_root_decoder_id()
{
    if (_stack.size() > 0){
        decode::Decoder *dec = _stack.front();
        return dec->decoder()->id;
    }
    return NULL;
}

} // namespace data
} // namespace pv
