/*
 * This file is part of the PulseView project.
 * DSView is based on PulseView.
 * 
 * Copyright (C) 2014 Joel Holdsworth <joel@airwebreathe.org.uk>
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

#include "row.h"

#include <libsigrokdecode.h>
#include <assert.h>

namespace pv {
namespace data {
namespace decode {

Row::Row() :
	_decoder(NULL),
    _row(NULL),
    _order(-1)
{
}

Row::Row(const srd_decoder *decoder, const srd_decoder_annotation_row *row, const int order) :
	_decoder(decoder),
    _row(row),
    _order(order)
{
    cache_text();
}

Row::Row(const Row &o)
{
	_decoder = o._decoder;
	_row = o._row;
	_order = o._order;
    _title = o._title;
    _title_id = o._title_id;
}

Row& Row::operator=(const Row &o)
{
	_decoder = o._decoder;
	_row = o._row;
	_order = o._order;
    _title = o._title;
    _title_id = o._title_id;
	return (*this);
}

/*
 * Copy the display text while the decoder definition is still alive.
 *
 * The name/id/desc strings belong to the decoder definition, and
 * srd_decoder_reload() frees that definition as soon as the new script has
 * been imported. Row objects outlive it in DecoderStack's _rows, _rows_gshow,
 * _rows_lshow and _class_rows maps, so reading the text out of _decoder / _row
 * on demand means the next repaint (DecoderModel::headerData ->
 * DecoderStack::list_row_title -> Row::title) runs strlen() on freed memory.
 * Caching the text at construction time keeps those maps usable.
 */
void Row::cache_text()
{
    _title.clear();
    _title_id.clear();

    const QString dec_name = (_decoder && _decoder->name)
            ? QString::fromUtf8(_decoder->name) : QString();
    const QString dec_id = (_decoder && _decoder->id)
            ? QString::fromUtf8(_decoder->id) : QString();
    const QString row_desc = (_row && _row->desc)
            ? QString::fromUtf8(_row->desc) : QString();

    if (!dec_name.isEmpty() && !row_desc.isEmpty())
        _title = QString("%1: %2").arg(dec_name).arg(row_desc);
    else if (!dec_name.isEmpty())
        _title = dec_name;
    else if (!row_desc.isEmpty())
        _title = row_desc;

    if (!dec_id.isEmpty() && !row_desc.isEmpty())
        _title_id = QString("%1: %2").arg(dec_id).arg(row_desc);
    else if (!dec_id.isEmpty())
        _title_id = dec_id;
    else if (!row_desc.isEmpty())
        _title_id = row_desc;
}

QString Row::title() const
{
	return _title;
}

QString Row::title_id() const
{
	return _title_id;
}

bool Row::operator<(const Row &other) const
{
    assert(_decoder);

    return (_decoder < other._decoder) ||
        (_decoder == other._decoder && _order < other._order);
}

} // decode
} // data
} // pv
