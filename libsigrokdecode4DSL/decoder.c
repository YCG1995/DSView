/*
 * This file is part of the libsigrokdecode project.
 *
 * Copyright (C) 2010 Uwe Hermann <uwe@hermann-uwe.de>
 * Copyright (C) 2012 Bert Vermeulen <bert@biot.com>
 * Copyright (C) 2019 DreamSourceLab <support@dreamsourcelab.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"
#include "libsigrokdecode-internal.h" /* First, so we avoid a _POSIX_C_SOURCE warning. */
#include "libsigrokdecode.h"
#include <glib.h>
#include "log.h"

/**
 * @file
 *
 * Listing, loading, unloading, and handling protocol decoders.
 */

/**
 * @defgroup grp_decoder Protocol decoders
 *
 * Handling protocol decoders.
 *
 * @{
 */

/** @cond PRIVATE */

/* 
	The list of loaded protocol decoders. 
	Is srd_decoder* type
*/
static GSList *pd_list = NULL;

/* srd.c */
extern SRD_PRIV GSList *searchpaths;

/* session.c */
extern SRD_PRIV GSList *sessions;
extern SRD_PRIV int max_session_id;

/* module_sigrokdecode.c */
extern SRD_PRIV PyObject *mod_sigrokdecode;

/** @endcond */

static gboolean srd_check_init(void)
{
	if (max_session_id < 0) {
		srd_err("Library is not initialized.");
		return FALSE;
	} else
		return TRUE;
}

/**
 * Returns the list of loaded protocol decoders.
 *
 * This is a GSList of pointers to struct srd_decoder items.
 *
 * @return List of decoders, NULL if none are supported or loaded.
 *
 * @since 0.2.0
 */
SRD_API const GSList *srd_decoder_list(void)
{
	return pd_list;
}

/**
 * Get the decoder with the specified ID.
 *
 * @param id The ID string of the decoder to return.
 *
 * @return The decoder with the specified ID, or NULL if not found.
 *
 * @since 0.1.0
 */
SRD_API struct srd_decoder *srd_decoder_get_by_id(const char *id)
{
	GSList *l;
	struct srd_decoder *dec;

	for (l = pd_list; l; l = l->next) {
		dec = l->data;
		if (!strcmp(dec->id, id))
			return dec;
	}

	return NULL;
}

static void channel_free(void *data)
{
	struct srd_channel *ch = data;

	if (!ch)
		return;

	safe_free(ch->desc);
	safe_free(ch->name);
	safe_free(ch->id);
	safe_free(ch->idn);
	g_free(ch);
}

static void variant_free(void *data)
{
	GVariant *var = data;

	if (!var)
		return;

	g_variant_unref(var);
}

static void annotation_row_free(void *data)
{
	struct srd_decoder_annotation_row *row = data;

	if (!row)
		return;

	g_slist_free(row->ann_classes);
	g_free(row->desc);
	g_free(row->id);
	g_free(row);
}

static void decoder_option_free(void *data)
{
	struct srd_decoder_option *opt = data;

	if (!opt)
		return;

	g_slist_free_full(opt->values, &variant_free);
	variant_free(opt->def);
	safe_free(opt->desc);
	safe_free(opt->id);
	safe_free(opt->idn);
	g_free(opt);
}

static void decoder_free(struct srd_decoder *dec)
{
	PyGILState_STATE gstate;

	if (!dec)
		return;

	gstate = PyGILState_Ensure();
	Py_XDECREF(dec->py_dec);
	Py_XDECREF(dec->py_mod);
	PyGILState_Release(gstate);

	g_slist_free_full(dec->options, &decoder_option_free);
	g_slist_free_full(dec->binary, (GDestroyNotify)&g_strfreev);
	g_slist_free_full(dec->annotation_rows, &annotation_row_free);
	g_slist_free_full(dec->annotations, (GDestroyNotify)&g_strfreev);
	g_slist_free_full(dec->opt_channels, &channel_free);
	g_slist_free_full(dec->channels, &channel_free);

	g_slist_free_full(dec->outputs, g_free);
	g_slist_free_full(dec->inputs, g_free);
	g_slist_free_full(dec->tags, g_free);
	g_free(dec->license);
	g_free(dec->desc);
	g_free(dec->longname);
	g_free(dec->name);
	g_free(dec->id);
	g_free(dec->module_name);

	g_free(dec);
}

static int get_channels(const struct srd_decoder *d, const char *attr,
		GSList **out_pdchl, int offset)
{
	PyObject *py_channellist, *py_entry;
	struct srd_channel *pdch;
	GSList *pdchl;
	ssize_t i;
	PyGILState_STATE gstate;

	gstate = PyGILState_Ensure();

	if (!PyObject_HasAttrString(d->py_dec, attr)) {
		/* No channels of this type specified. */
		PyGILState_Release(gstate);
		return SRD_OK;
	}

	pdchl = NULL;

	py_channellist = PyObject_GetAttrString(d->py_dec, attr);
	if (!py_channellist)
		goto except_out;

	if (!PyTuple_Check(py_channellist)) {
		srd_err("Protocol decoder %s %s attribute is not a tuple.",
			d->name, attr);
		goto err_out;
	}

	for (i = PyTuple_Size(py_channellist) - 1; i >= 0; i--) {
		py_entry = PyTuple_GetItem(py_channellist, i);
		if (!py_entry)
			goto except_out;

		if (!PyDict_Check(py_entry)) {
			srd_err("Protocol decoder %s %s attribute is not "
				"a list of dict elements.", d->name, attr);
			goto err_out;
		}
		pdch = g_try_malloc0(sizeof(struct srd_channel));
		if (pdch == NULL){
			srd_err("%s,ERROR:failed to alloc memory.", __func__);
			goto err_out;
		}
		memset(pdch, 0, sizeof(struct srd_channel));

		/* Add to list right away so it doesn't get lost. */
		pdchl = g_slist_prepend(pdchl, pdch);

		if (py_dictitem_as_str(py_entry, "id", &pdch->id) != SRD_OK)
			goto err_out;
		if (py_dictitem_as_str(py_entry, "name", &pdch->name) != SRD_OK)
			goto err_out;
		if (py_dictitem_as_str(py_entry, "desc", &pdch->desc) != SRD_OK)
			goto err_out;

		py_dictitem_as_str(py_entry, "idn", &pdch->idn);

		pdch->type = py_dictitem_to_int(py_entry, "type");
		if (pdch->type < 0)
			pdch->type = SRD_CHANNEL_COMMON;
		pdch->order = offset + i;
	}

	Py_DECREF(py_channellist);
	*out_pdchl = pdchl;

	PyGILState_Release(gstate);

	return SRD_OK;

except_out:
    srd_exception_catch(NULL, "Failed to get %s list of %s decoder",
			attr, d->name);

err_out:
	g_slist_free_full(pdchl, &channel_free);
	Py_XDECREF(py_channellist);
	PyGILState_Release(gstate);

	return SRD_ERR_PYTHON;
}

static int get_options(struct srd_decoder *d)
{
	PyObject *py_opts, *py_opt, *py_str, *py_values, *py_default, *py_item;
	GSList *options;
	struct srd_decoder_option *o;
	GVariant *gvar;
	ssize_t opt, i;
	PyGILState_STATE gstate;

	gstate = PyGILState_Ensure();

	if (!PyObject_HasAttrString(d->py_dec, "options")) {
		/* No options, that's fine. */
		PyGILState_Release(gstate);
		return SRD_OK;
	}

	options = NULL;

	/* If present, options must be a tuple. */
	py_opts = PyObject_GetAttrString(d->py_dec, "options");
	if (!py_opts)
		goto except_out;

	if (!PyTuple_Check(py_opts)) {
		srd_err("Protocol decoder %s: options attribute is not "
				"a tuple.", d->id);
		goto err_out;
	}

	for (opt = PyTuple_Size(py_opts) - 1; opt >= 0; opt--) {
		py_opt = PyTuple_GetItem(py_opts, opt);
		if (!py_opt)
			goto except_out;

		if (!PyDict_Check(py_opt)) {
			srd_err("Protocol decoder %s options: each option "
					"must consist of a dictionary.", d->name);
			goto err_out;
		}

		o = g_try_malloc0(sizeof(struct srd_decoder_option));
		if (o == NULL){
			srd_err("%s,ERROR:failed to alloc memory.", __func__);
			goto err_out;
		}
		memset(o, 0, sizeof(struct srd_decoder_option));

		/* Add to list right away so it doesn't get lost. */
		options = g_slist_prepend(options, o);

		py_str = PyDict_GetItemString(py_opt, "id");
		if (!py_str) {
			srd_err("Protocol decoder %s option %zd has no ID.",
				d->name, opt);
			goto err_out;
		}
		if (py_str_as_str(py_str, &o->id) != SRD_OK)
			goto err_out;

		py_str = PyDict_GetItemString(py_opt, "desc");
		if (py_str) {
			if (py_str_as_str(py_str, &o->desc) != SRD_OK)
				goto err_out;
		}

		py_str = PyDict_GetItemString(py_opt, "idn");
		if (py_str){
			py_str_as_str(py_str, &o->idn);
		}

		py_default = PyDict_GetItemString(py_opt, "default");
		if (py_default) {
			gvar = py_obj_to_variant(py_default);
			if (!gvar) {
				srd_err("Protocol decoder %s option 'default' has "
					"invalid default value.", d->name);
				goto err_out;
			}
			o->def = g_variant_ref_sink(gvar);
		}

		py_values = PyDict_GetItemString(py_opt, "values");
		if (py_values) {
			/*
			 * A default is required if a list of values is
			 * given, since it's used to verify their type.
			 */
			if (!o->def) {
				srd_err("No default for option '%s'.", o->id);
				goto err_out;
			}
			if (!PyTuple_Check(py_values)) {
				srd_err("Option '%s' values should be a tuple.", o->id);
				goto err_out;
			}

			for (i = PyTuple_Size(py_values) - 1; i >= 0; i--) {
				py_item = PyTuple_GetItem(py_values, i);
				if (!py_item)
					goto except_out;

				if (py_default && (Py_TYPE(py_default) != Py_TYPE(py_item))) {
					srd_err("All values for option '%s' must be "
						"of the same type as the default.",
						o->id);
					goto err_out;
				}
				gvar = py_obj_to_variant(py_item);
				if (!gvar) {
					srd_err("Protocol decoder %s option 'values' "
						"contains invalid value.", d->name);
					goto err_out;
				}
				o->values = g_slist_prepend(o->values,
						g_variant_ref_sink(gvar));
			}
		}
	}
	d->options = options;
	Py_DECREF(py_opts);
	PyGILState_Release(gstate);

	return SRD_OK;

except_out:
    srd_exception_catch(NULL, "Failed to get %s decoder options", d->name);

err_out:
	g_slist_free_full(options, &decoder_option_free);
	Py_XDECREF(py_opts);
	PyGILState_Release(gstate);

	return SRD_ERR_PYTHON;
}

/* Convert annotation class attribute to GSList of char **. */
static int get_annotations(struct srd_decoder *dec)
{
	PyObject *py_annlist, *py_ann;
	GSList *annotations;
	char **annpair;
	ssize_t i;
	int ann_type = 7;
	unsigned int j;
	PyGILState_STATE gstate;

	assert(dec);

	gstate = PyGILState_Ensure();

	if (!PyObject_HasAttrString(dec->py_dec, "annotations")) {
		PyGILState_Release(gstate);
		return SRD_OK;
	}

	annotations = NULL;

	py_annlist = PyObject_GetAttrString(dec->py_dec, "annotations");
	if (!py_annlist)
		goto except_out;

	if (!PyTuple_Check(py_annlist)) {
		srd_err("Protocol decoder %s annotations should "
			"be a tuple.", dec->name);
		goto err_out;
	}

	for (i = 0; i < PyTuple_Size(py_annlist); i++) {
		py_ann = PyTuple_GetItem(py_annlist, i);
		if (!py_ann)
			goto except_out;

		if (!PyTuple_Check(py_ann) || (PyTuple_Size(py_ann) != 3 && PyTuple_Size(py_ann) != 2)) {
			srd_err("Protocol decoder %s annotation %zd should "
				"be a tuple with two or three elements.",
				dec->name, i + 1);
			goto err_out;
		}
		if (py_strseq_to_char(py_ann, &annpair) != SRD_OK)
			goto err_out;

		annotations = g_slist_prepend(annotations, annpair);

		if (PyTuple_Size(py_ann) == 3) {
			ann_type = 0;
            for (j = 0; j < strlen(annpair[0]); j++)
                ann_type = ann_type * 10 + (annpair[0][j] - '0');
            dec->ann_types = g_slist_append(dec->ann_types, GINT_TO_POINTER(ann_type));
		} else if (PyTuple_Size(py_ann) == 2) {
			dec->ann_types = g_slist_append(dec->ann_types, GINT_TO_POINTER(ann_type));
			ann_type++;
		}
	}
	dec->annotations = annotations;
	Py_DECREF(py_annlist);
	PyGILState_Release(gstate);

	return SRD_OK;

except_out:
    srd_exception_catch(NULL, "Failed to get %s decoder annotations", dec->name);

err_out:
	g_slist_free_full(annotations, (GDestroyNotify)&g_strfreev);
	Py_XDECREF(py_annlist);
	PyGILState_Release(gstate);

	return SRD_ERR_PYTHON;
}

/* Convert annotation_rows to GSList of 'struct srd_decoder_annotation_row'. */
static int get_annotation_rows(struct srd_decoder *dec)
{
	PyObject *py_ann_rows, *py_ann_row, *py_ann_classes, *py_item;
	GSList *annotation_rows;
	struct srd_decoder_annotation_row *ann_row;
	ssize_t i, k;
	size_t class_idx;
	PyGILState_STATE gstate;

	gstate = PyGILState_Ensure();

	if (!PyObject_HasAttrString(dec->py_dec, "annotation_rows")) {
		PyGILState_Release(gstate);
		return SRD_OK;
	}

	annotation_rows = NULL;

	py_ann_rows = PyObject_GetAttrString(dec->py_dec, "annotation_rows");
	if (!py_ann_rows)
		goto except_out;

	if (!PyTuple_Check(py_ann_rows)) {
		srd_err("Protocol decoder %s annotation_rows "
			"must be a tuple.", dec->name);
		goto err_out;
	}

	for (i = PyTuple_Size(py_ann_rows) - 1; i >= 0; i--) {
		py_ann_row = PyTuple_GetItem(py_ann_rows, i);
		if (!py_ann_row)
			goto except_out;

		if (!PyTuple_Check(py_ann_row) || PyTuple_Size(py_ann_row) != 3) {
			srd_err("Protocol decoder %s annotation_rows "
				"must contain only tuples of 3 elements.",
				dec->name);
			goto err_out;
		}
		ann_row = g_try_malloc0(sizeof(struct srd_decoder_annotation_row));
		if (ann_row == NULL){
			srd_err("%s,ERROR:failed to alloc memory.", __func__);
			goto err_out;
		}
		memset(ann_row, 0, sizeof(struct srd_decoder_annotation_row));

		/* Add to list right away so it doesn't get lost. */
		annotation_rows = g_slist_prepend(annotation_rows, ann_row);

		py_item = PyTuple_GetItem(py_ann_row, 0);
		if (!py_item)
			goto except_out;
		if (py_str_as_str(py_item, &ann_row->id) != SRD_OK)
			goto err_out;

		py_item = PyTuple_GetItem(py_ann_row, 1);
		if (!py_item)
			goto except_out;
		if (py_str_as_str(py_item, &ann_row->desc) != SRD_OK)
			goto err_out;

		py_ann_classes = PyTuple_GetItem(py_ann_row, 2);
		if (!py_ann_classes)
			goto except_out;

		if (!PyTuple_Check(py_ann_classes)) {
			srd_err("Protocol decoder %s annotation_rows tuples "
				"must have a tuple of numbers as 3rd element.",
				dec->name);
			goto err_out;
		}

		for (k = PyTuple_Size(py_ann_classes) - 1; k >= 0; k--) {
			py_item = PyTuple_GetItem(py_ann_classes, k);
			if (!py_item)
				goto except_out;

			if (!PyLong_Check(py_item)) {
				srd_err("Protocol decoder %s annotation row "
					"class tuple must only contain numbers.",
					dec->name);
				goto err_out;
			}
			class_idx = PyLong_AsSize_t(py_item);
			if (PyErr_Occurred())
				goto except_out;

			ann_row->ann_classes = g_slist_prepend(ann_row->ann_classes,
					GSIZE_TO_POINTER(class_idx));
		}
	}
	dec->annotation_rows = annotation_rows;
	Py_DECREF(py_ann_rows);
	PyGILState_Release(gstate);

	return SRD_OK;

except_out:
    srd_exception_catch(NULL, "Failed to get %s decoder annotation rows",
			dec->name);

err_out:
	g_slist_free_full(annotation_rows, &annotation_row_free);
	Py_XDECREF(py_ann_rows);
	PyGILState_Release(gstate);

	return SRD_ERR_PYTHON;
}

/* Convert binary classes to GSList of char **. */
static int get_binary_classes(struct srd_decoder *dec)
{
	PyObject *py_bin_classes, *py_bin_class;
	GSList *bin_classes;
	char **bin;
	ssize_t i;
	PyGILState_STATE gstate;

	gstate = PyGILState_Ensure();

	if (!PyObject_HasAttrString(dec->py_dec, "binary")) {
		PyGILState_Release(gstate);
		return SRD_OK;
	}

	bin_classes = NULL;

	py_bin_classes = PyObject_GetAttrString(dec->py_dec, "binary");
	if (!py_bin_classes)
		goto except_out;

	if (!PyTuple_Check(py_bin_classes)) {
		srd_err("Protocol decoder %s binary classes should "
			"be a tuple.", dec->name);
		goto err_out;
	}

	for (i = PyTuple_Size(py_bin_classes) - 1; i >= 0; i--) {
		py_bin_class = PyTuple_GetItem(py_bin_classes, i);
		if (!py_bin_class)
			goto except_out;

		if (!PyTuple_Check(py_bin_class)
				|| PyTuple_Size(py_bin_class) != 2) {
			srd_err("Protocol decoder %s binary classes should "
				"consist only of tuples of 2 elements.",
				dec->name);
			goto err_out;
		}
		if (py_strseq_to_char(py_bin_class, &bin) != SRD_OK)
			goto err_out;

		bin_classes = g_slist_prepend(bin_classes, bin);
	}
	dec->binary = bin_classes;
	Py_DECREF(py_bin_classes);
	PyGILState_Release(gstate);

	return SRD_OK;

except_out:
    srd_exception_catch(NULL, "Failed to get %s decoder binary classes",
			dec->name);

err_out:
	g_slist_free_full(bin_classes, (GDestroyNotify)&g_strfreev);
	Py_XDECREF(py_bin_classes);
	PyGILState_Release(gstate);

	return SRD_ERR_PYTHON;
}

/* Check whether the Decoder class defines the named method. */
static int check_method(PyObject *py_dec, const char *mod_name,
		const char *method_name)
{
	PyObject *py_method;
	int is_callable;
	PyGILState_STATE gstate;

	gstate = PyGILState_Ensure();

	py_method = PyObject_GetAttrString(py_dec, method_name);
	if (!py_method) {
        srd_exception_catch(NULL, "Protocol decoder %s Decoder class "
				"has no %s() method", mod_name, method_name);
		PyGILState_Release(gstate);
		return SRD_ERR_PYTHON;
	}

	is_callable = PyCallable_Check(py_method);
	Py_DECREF(py_method);

	PyGILState_Release(gstate);

	if (!is_callable) {
		srd_err("Protocol decoder %s Decoder class attribute '%s' "
			"is not a method.", mod_name, method_name);
		return SRD_ERR_PYTHON;
	}

	return SRD_OK;
}

/**
 * Get the API version of the specified decoder.
 *
 * @param d The decoder to use. Must not be NULL.
 *
 * @return The API version of the decoder, or 0 upon errors.
 *
 * @private
 */
SRD_PRIV long srd_decoder_apiver(const struct srd_decoder *d)
{
	PyObject *py_apiver;
	long apiver;
	PyGILState_STATE gstate;

	if (!d)
		return 0;

	gstate = PyGILState_Ensure();

	py_apiver = PyObject_GetAttrString(d->py_dec, "api_version");
	apiver = (py_apiver && PyLong_Check(py_apiver))
			? PyLong_AsLong(py_apiver) : 0;
	Py_XDECREF(py_apiver);

	PyGILState_Release(gstate);

	return apiver;
}

/**
 * Load a protocol decoder module into the embedded Python interpreter.
 *
 * @param module_name The module name to be loaded.
 *
 * @return SRD_OK upon success, a (negative) error code otherwise.
 *
 * @since 0.1.0
 */
SRD_API int srd_decoder_load(const char *module_name)
{
	PyObject *py_basedec;
	struct srd_decoder *d;
	long apiver;
	int is_subclass;
	const char *fail_txt = NULL;
	PyGILState_STATE gstate;
  
	if (!srd_check_init())
		return SRD_ERR;

	if (!module_name)
		return SRD_ERR_ARG;

	gstate = PyGILState_Ensure();

	if (PyDict_GetItemString(PyImport_GetModuleDict(), module_name)) {
		/* Module was already imported. */
		PyGILState_Release(gstate);
		return SRD_OK;
	}

	d = g_try_malloc0(sizeof(struct srd_decoder));
	if (d == NULL){
		srd_err("%s,ERROR:failed to alloc memory.", __func__);
		goto err_out;
	}
	memset(d, 0, sizeof(struct srd_decoder));

	fail_txt = NULL;

	// Remember the package / directory name for later selective reload.
	d->module_name = g_strdup(module_name);

	//Load module from python script file,module_name is a sub directory
	d->py_mod = py_import_by_name(module_name);
	if (!d->py_mod) {
		fail_txt = "import by name failed";
		goto except_out;
	}

	if (!mod_sigrokdecode) {
		srd_err("sigrokdecode module not loaded.");
		fail_txt = "sigrokdecode(3) not loaded";
		goto err_out;
	}

	/* 
		Get the 'Decoder' class as Python object. 
		Here, Decoder is python class type
	*/
	d->py_dec = PyObject_GetAttrString(d->py_mod, "Decoder");
	if (!d->py_dec) {
		fail_txt = "no 'Decoder' attribute in imported module";
		goto except_out;
	}

	/*
	   Here, Decoder is c class type
	*/
	py_basedec = PyObject_GetAttrString(mod_sigrokdecode, "Decoder");
	if (!py_basedec) {
		fail_txt = "no 'Decoder' attribute in sigrokdecode(3)";
		goto except_out;
	}

	is_subclass = PyObject_IsSubclass(d->py_dec, py_basedec);
	Py_DECREF(py_basedec);

	if (!is_subclass) {
		srd_err("Decoder class in protocol decoder module %s is not "
			"a subclass of sigrokdecode.Decoder.", module_name);
		fail_txt = "not a subclass of sigrokdecode.Decoder";
		goto err_out;
	}

	/*
	 * Check that this decoder has the correct PD API version.
	 * PDs of different API versions are incompatible and cannot work.
	 */
	apiver = srd_decoder_apiver(d);
	if (apiver != 3) {
        srd_exception_catch(NULL, "Only PD API version 3 is supported, "
			"decoder %s has version %ld", module_name, apiver);
		fail_txt = "API version mismatch";
		goto err_out;
	}

	/* Check Decoder class for required methods. */

	if (check_method(d->py_dec, module_name, "reset") != SRD_OK) {
		fail_txt = "no 'reset()' method";
		goto err_out;
	}

	if (check_method(d->py_dec, module_name, "start") != SRD_OK) {
		fail_txt = "no 'start()' method";
		goto err_out;
	}

	if (check_method(d->py_dec, module_name, "decode") != SRD_OK) {
		fail_txt = "no 'decode()' method";
		goto err_out;
	}

	/* Store required fields in newly allocated strings. */
	if (py_attr_as_str(d->py_dec, "id", &(d->id)) != SRD_OK) {
		fail_txt = "no 'id' attribute";
		goto err_out;
	}
 
	if (py_attr_as_str(d->py_dec, "name", &(d->name)) != SRD_OK) {
		fail_txt = "no 'name' attribute";
		goto err_out;
	}

	if (py_attr_as_str(d->py_dec, "longname", &(d->longname)) != SRD_OK) {
		fail_txt = "no 'longname' attribute";
		goto err_out;
	}

	if (py_attr_as_str(d->py_dec, "desc", &(d->desc)) != SRD_OK) {
		fail_txt = "no 'desc' attribute";
		goto err_out;
	}

	if (py_attr_as_str(d->py_dec, "license", &(d->license)) != SRD_OK) {
		fail_txt = "no 'license' attribute";
		goto err_out;
	}

	if (py_attr_as_strlist(d->py_dec, "inputs", &(d->inputs)) != SRD_OK) {
		fail_txt = "missing or malformed 'inputs' attribute";
		goto err_out;
	}

	if (py_attr_as_strlist(d->py_dec, "outputs", &(d->outputs)) != SRD_OK) {
		fail_txt = "missing or malformed 'outputs' attribute";
		goto err_out;
	}

	if (py_attr_as_strlist(d->py_dec, "tags", &(d->tags)) != SRD_OK) {
		fail_txt = "missing or malformed 'tags' attribute";
		goto err_out;
	}

	/* All options and their default values. */
	if (get_options(d) != SRD_OK) {
		fail_txt = "cannot get options";
		goto err_out;
	}

	/* Check and import required channels. */
	if (get_channels(d, "channels", &d->channels, 0) != SRD_OK) {
		fail_txt = "cannot get channels";
		goto err_out;
	}

	/* Check and import optional channels. */
	if (get_channels(d, "optional_channels", &d->opt_channels,
				g_slist_length(d->channels)) != SRD_OK) {
		fail_txt = "cannot get optional channels";
		goto err_out;
	}

	if (get_annotations(d) != SRD_OK) {
		fail_txt = "cannot get annotations";
		goto err_out;
	}

	if (get_annotation_rows(d) != SRD_OK) {
		fail_txt = "cannot get annotation rows";
		goto err_out;
	}

	if (get_binary_classes(d) != SRD_OK) {
		fail_txt = "cannot get binary classes";
		goto err_out;
	}

	PyGILState_Release(gstate);

	/* Append it to the list of loaded decoders. */
	pd_list = g_slist_append(pd_list, d);

	return SRD_OK;

except_out:
	/* Don't show a message for the "common" directory, it's not a PD. */
	if (strcmp(module_name, "common")) {
        srd_exception_catch(NULL, "Failed to load decoder %s: %s",
				    module_name, fail_txt);
	}
	fail_txt = NULL;

err_out:
	if (fail_txt != NULL){
		srd_err("Failed to load decoder %s: %s", module_name, fail_txt);
	}

	decoder_free(d);
	PyGILState_Release(gstate);

	return SRD_ERR_PYTHON;
}

/**
 * Return a protocol decoder's docstring.
 *
 * @param dec The loaded protocol decoder.
 *
 * @return A newly allocated buffer containing the protocol decoder's
 *         documentation. The caller is responsible for free'ing the buffer.
 *
 * @since 0.1.0
 */
SRD_API char *srd_decoder_doc_get(const struct srd_decoder *dec)
{
	PyObject *py_str;
	char *doc;
	PyGILState_STATE gstate;

	if (!srd_check_init())
		return NULL;

	if (!dec)
		return NULL;

	gstate = PyGILState_Ensure();

	if (!PyObject_HasAttrString(dec->py_mod, "__doc__"))
		goto err;

	if (!(py_str = PyObject_GetAttrString(dec->py_mod, "__doc__"))) {
        srd_exception_catch(NULL, "Failed to get docstring");
		goto err;
	}

	doc = NULL;
	if (py_str != Py_None)
		py_str_as_str(py_str, &doc);
	Py_DECREF(py_str);

	PyGILState_Release(gstate);

	return doc;

err:
	PyGILState_Release(gstate);

	return NULL;
}

/**
 * Unload the specified protocol decoder.
 *
 * @param dec The struct srd_decoder to be unloaded.
 *
 * @return SRD_OK upon success, a (negative) error code otherwise.
 *
 * @since 0.1.0
 */
SRD_API int srd_decoder_unload(struct srd_decoder *dec)
{
	struct srd_session *sess;
	GSList *l;

	if (!srd_check_init())
		return SRD_ERR;

	if (!dec)
		return SRD_ERR_ARG;

	/*
	 * Since any instances of this decoder need to be released as well,
	 * but they could be anywhere in the stack, just free the entire
	 * stack. A frontend reloading a decoder thus has to restart all
	 * instances, and rebuild the stack.
	 */
	for (l = sessions; l; l = l->next) {
		sess = l->data;
		srd_inst_free_all(sess);
	}

	/* Remove the PD from the list of loaded decoders. */
	pd_list = g_slist_remove(pd_list, dec);

	decoder_free(dec);

	return SRD_OK;
}

static void srd_decoder_load_all_zip_path(char *zip_path)
{
	PyObject *zipimport_mod, *zipimporter_class, *zipimporter;
	PyObject *prefix_obj, *files, *key, *value, *set, *modname;
	Py_ssize_t pos = 0;
	char *prefix;
	size_t prefix_len;
	PyGILState_STATE gstate;

	set = files = prefix_obj = zipimporter = zipimporter_class = NULL;

	gstate = PyGILState_Ensure();

	zipimport_mod = py_import_by_name("zipimport");
	if (zipimport_mod == NULL)
		goto err_out;

	zipimporter_class = PyObject_GetAttrString(zipimport_mod, "zipimporter");
	if (zipimporter_class == NULL)
		goto err_out;

	zipimporter = PyObject_CallFunction(zipimporter_class, "s", zip_path);
	if (zipimporter == NULL)
		goto err_out;

	prefix_obj = PyObject_GetAttrString(zipimporter, "prefix");
	if (prefix_obj == NULL)
		goto err_out;

	files = PyObject_GetAttrString(zipimporter, "_files");
	if (files == NULL || !PyDict_Check(files))
		goto err_out;

	set = PySet_New(NULL);
	if (set == NULL)
		goto err_out;

	if (py_str_as_str(prefix_obj, &prefix) != SRD_OK)
		goto err_out;

	prefix_len = strlen(prefix);

	while (PyDict_Next(files, &pos, &key, &value)) {
		char *path, *slash;
		if (py_str_as_str(key, &path) == SRD_OK) {
			if (strlen(path) > prefix_len
					&& memcmp(path, prefix, prefix_len) == 0
					&& (slash = strchr(path + prefix_len, '/'))) {

				modname = PyUnicode_FromStringAndSize(path + prefix_len,
							slash - (path + prefix_len));
				if (modname == NULL) {
					PyErr_Clear();
				} else {
					PySet_Add(set, modname);
					Py_DECREF(modname);
				}
			}
			g_free(path);
		}
	}
	g_free(prefix);

	while ((modname = PySet_Pop(set))) {
		char *modname_str;
		if (py_str_as_str(modname, &modname_str) == SRD_OK) {
			/* The directory name is the module name (e.g. "i2c"). */
			srd_decoder_load(modname_str);
			g_free(modname_str);
		}
		Py_DECREF(modname);
	}

err_out:
	Py_XDECREF(set);
	Py_XDECREF(files);
	Py_XDECREF(prefix_obj);
	Py_XDECREF(zipimporter);
	Py_XDECREF(zipimporter_class);
	Py_XDECREF(zipimport_mod);
	PyErr_Clear();
	PyGILState_Release(gstate);
}

static void srd_decoder_load_all_path(char *path)
{
	GDir *dir;
	const gchar *direntry;

	assert(path);

	if (!(dir = g_dir_open(path, 0, NULL))) {
		/* Not really fatal. Try zipimport method too. */
		srd_decoder_load_all_zip_path(path);
		return;
	}

	/*
	 * This ignores errors returned by srd_decoder_load(). That
	 * function will have logged the cause, but in any case we
	 * want to continue anyway.
	 */
	while ((direntry = g_dir_read_name(dir)) != NULL) {
		/* The directory name is the module name (e.g. "i2c"). */
		srd_decoder_load(direntry);
	}
	g_dir_close(dir);
}

/**
 * Load all installed protocol decoders.
 *
 * @return SRD_OK upon success, a (negative) error code otherwise.
 *
 * @since 0.1.0
 */
SRD_API int srd_decoder_load_all(void)
{
	GSList *l;

	if (!srd_check_init())
		return SRD_ERR;

    for (l = searchpaths; l; l = l->next){
		srd_decoder_load_all_path(l->data);
    }

	return SRD_OK;
}

static void srd_decoder_unload_cb(void *arg, void *ignored)
{
	(void)ignored;

	srd_decoder_unload((struct srd_decoder *)arg);
}

/**
 * Unload all loaded protocol decoders.
 *
 * @return SRD_OK upon success, a (negative) error code otherwise.
 *
 * @since 0.1.0
 */
SRD_API int srd_decoder_unload_all(void)
{
	g_slist_foreach(pd_list, srd_decoder_unload_cb, NULL);
	g_slist_free(pd_list);
	pd_list = NULL;

	return SRD_OK;
}

/**
 * Reload a single protocol decoder from its (possibly modified) script files.
 *
 * The decoder module and all of its submodules are purged from sys.modules so
 * the import does not return a cached module, then the decoder is unloaded and
 * freshly imported. Like srd_decoder_unload(), this frees all decoder
 * instances in every session; the frontend must rebuild instances afterwards.
 *
 * @param[in] id The decoder id (directory name), e.g. "1-i2c".
 *
 * @return SRD_OK upon success, a (negative) error code otherwise.
 */
/*
 * Locate a loaded decoder by its Python module (package / directory) name.
 * The decoder's logical id (srd_decoder->id, e.g. "0:i2c") differs from its
 * package name (e.g. "0-i2c"), so it cannot be looked up via get_by_id.
 * Must be called while holding the GIL.
 */
static struct srd_decoder *get_decoder_by_module_name(const char *module_name)
{
	GSList *l;

	for (l = pd_list; l; l = l->next) {
		struct srd_decoder *d = l->data;

		if (d->module_name && strcmp(d->module_name, module_name) == 0)
			return d;
	}

	return NULL;
}

SRD_API struct srd_decoder *srd_decoder_get_by_module_name(
		const char *module_name)
{
	struct srd_decoder *dec;
	PyGILState_STATE gstate;

	if (!srd_check_init() || !module_name)
		return NULL;

	gstate = PyGILState_Ensure();
	dec = get_decoder_by_module_name(module_name);
	PyGILState_Release(gstate);

	return dec;
}

SRD_API int srd_decoder_reload(const char *module_name)
{
	struct srd_decoder *dec;
	PyGILState_STATE gstate;
	PyObject *modules, *keys;
	Py_ssize_t i, n;
	char *prefix;
	size_t namelen;
	int purged = 0, ret;

	if (!srd_check_init())
		return SRD_ERR;
	if (!module_name)
		return SRD_ERR_ARG;

	namelen = strlen(module_name);
	prefix = g_strdup_printf("%s.", module_name);

	gstate = PyGILState_Ensure();

	dec = get_decoder_by_module_name(module_name);
	if (!dec) {
		srd_err("reload: module '%s' not found in loaded list.",
			module_name);
		g_free(prefix);
		PyGILState_Release(gstate);
		return SRD_ERR_ARG;
	}
	srd_err("reload: begin for module '%s', purging sys.modules.",
		module_name);

	/* Remove the package and all its submodules from sys.modules. */
	modules = PyImport_GetModuleDict();
	keys = PyDict_Keys(modules);
	n = PyList_Size(keys);

	for (i = 0; i < n; i++) {
		PyObject *key = PyList_GetItem(keys, i);
		PyObject *k_bytes = PyUnicode_AsUTF8String(key);

		if (!k_bytes) {
			PyErr_Clear();
			continue;
		}

		const char *k = PyBytes_AsString(k_bytes);

		if (!k) {
			Py_DECREF(k_bytes);
			PyErr_Clear();
			continue;
		}

		if (strcmp(k, module_name) == 0 ||
		    strncmp(k, prefix, namelen + 1) == 0) {
			if (PyDict_DelItem(modules, key) == -1)
				PyErr_Clear();
			else
				purged++;
		}

		Py_DECREF(k_bytes);
	}

	Py_DECREF(keys);
	g_free(prefix);
	srd_err("reload: purged %d sys.modules entries.", purged);
	PyGILState_Release(gstate);

	/*
	 * Detach the old definition from the loaded list but do NOT free it yet.
	 *
	 * The frontend still holds this srd_decoder and the srd_channel pointers
	 * inside it until it has attached the freshly imported one. Freeing it
	 * before the re-import has succeeded would leave those pointers dangling
	 * whenever the new script compiles but fails at import time (a missing
	 * dependency, an exception at package level, ...) - and the frontend has
	 * no way to notice. Keeping the old definition alive turns a failed
	 * reload into "the previous version keeps working" instead of a crash.
	 */
	pd_list = g_slist_remove(pd_list, dec);

	ret = srd_decoder_load(module_name);
	if (ret == SRD_OK) {
		/*
		 * srd_decoder_load() returns SRD_OK without doing anything when the
		 * module is still in sys.modules, so success alone does not prove a
		 * new definition is there: check the list for a *different* object.
		 */
		struct srd_decoder *fresh = get_decoder_by_module_name(module_name);

		if (!fresh || fresh == dec) {
			srd_err("reload: re-import produced no new decoder for '%s'.",
				module_name);
			ret = SRD_ERR;
		}
	}

	if (ret != SRD_OK) {
		pd_list = g_slist_prepend(pd_list, dec);
		srd_err("reload: re-import failed (%d), keeping the previous decoder.",
			ret);
		return ret;
	}
	srd_err("reload: re-imported, releasing the previous decoder.");

	/*
	 * The new definition is in place, so the old one can go. Instances may
	 * live in any stack of any session, hence the global sweep.
	 */
	for (GSList *l = sessions; l; l = l->next)
		srd_inst_free_all((struct srd_session *)l->data);

	decoder_free(dec);
	return SRD_OK;
}

/* Locate the main script of a decoder package in the registered search
 * paths. Returns a newly allocated path, or NULL when nothing matches. */
static char *srd_decoder_find_script(const char *module_name)
{
	GSList *l;
	char *pkg, *mod_py, *cand;

	pkg = g_strdup_printf("%s/__init__.py", module_name);
	mod_py = g_strdup_printf("%s.py", module_name);
	for (l = srd_searchpaths_get(); l; l = l->next) {
		const char *dir = (const char *)l->data;

		cand = g_build_filename(dir, pkg, NULL);
		if (g_file_test(cand, G_FILE_TEST_EXISTS)) {
			g_free(pkg);
			g_free(mod_py);
			return cand;
		}
		g_free(cand);

		cand = g_build_filename(dir, module_name, mod_py, NULL);
		if (g_file_test(cand, G_FILE_TEST_EXISTS)) {
			g_free(pkg);
			g_free(mod_py);
			return cand;
		}
		g_free(cand);
	}
	g_free(pkg);
	g_free(mod_py);
	return NULL;
}

/* Returns a newly allocated UTF-8 copy of a Python unicode object. */
static char *py_unicode_dup(PyObject *obj)
{
	PyObject *bytes;
	char *s;

	if (!obj)
		return NULL;
	bytes = PyUnicode_AsUTF8String(obj);
	if (!bytes)
		return NULL;
	s = g_strdup(PyBytes_AsString(bytes));
	Py_DECREF(bytes);
	return s;
}

/* Compile one script file (GIL must be held). Returns SRD_OK when it parses. */
static int srd_check_script_syntax(const char *path, char **error)
{
	char *code = NULL;
	PyObject *co = NULL, *etype = NULL, *evalue = NULL, *etb = NULL;
	int ret = SRD_OK;

	if (!g_file_get_contents(path, &code, NULL, NULL)) {
		if (error)
			*error = g_strdup_printf("Cannot read script: %s", path);
		return SRD_ERR;
	}

	co = Py_CompileString(code, path, Py_file_input);
	if (!co) {
		PyErr_Fetch(&etype, &evalue, &etb);
		PyErr_NormalizeException(&etype, &evalue, &etb);

		if (error) {
			/* SyntaxError carries position details. */
			PyObject *plineno = PyObject_GetAttrString(evalue, "lineno");
			PyObject *ptext = PyObject_GetAttrString(evalue, "text");
			PyObject *pmsg = PyObject_GetAttrString(evalue, "msg");
			char *msg = NULL, *text = NULL;

			long lineno = plineno ? PyLong_AsLong(plineno) : -1;
			msg = (pmsg && PyUnicode_Check(pmsg))
					? py_unicode_dup(pmsg) : NULL;
			text = (ptext && PyUnicode_Check(ptext))
					? py_unicode_dup(ptext) : NULL;

			if (lineno > 0)
				*error = g_strdup_printf(
					"Syntax error in %s line %ld: %s\n%4ld | %s",
					path, lineno, msg ? msg : "invalid syntax",
					lineno, text ? text : "");
			else
				*error = g_strdup_printf("Compile failed in %s: %s",
					path, msg ? msg : "invalid syntax");

			g_free(msg);
			g_free(text);
			Py_XDECREF(plineno);
			Py_XDECREF(ptext);
			Py_XDECREF(pmsg);
		}

		PyErr_Clear();
		ret = SRD_ERR;
	}

	Py_XDECREF(co);
	Py_XDECREF(etb);
	Py_XDECREF(evalue);
	Py_XDECREF(etype);
	g_free(code);

	return ret;
}

/**
 * Compile-check a decoder script without importing it.
 *
 * @param module_name The decoder package directory name (e.g. "0-i2c").
 * @param error       If non-NULL, receives a human readable diagnostic
 *                    containing the script path, line number and source text.
 *
 * @return SRD_OK if the script compiles, SRD_ERR on syntax/IO errors.
 */
SRD_API int srd_decoder_check(const char *module_name, char **error){
	char *path = NULL, *dir = NULL, *code = NULL;
	GDir *gd = NULL;
	const char *fn;
	gboolean checked_any = FALSE;
	PyGILState_STATE gstate;
	int ret = SRD_OK;

	if (error)
		*error = NULL;

	if (!module_name)
		return SRD_ERR_ARG;

	path = srd_decoder_find_script(module_name);
	if (!path) {
		if (error)
			*error = g_strdup_printf("Decoder script not found: %s",
					module_name);
		return SRD_ERR;
	}
	dir = g_path_get_dirname(path);

	/* A package can be split over several files (Decoders declare
	 * __init__ plus one or more implementation modules), so compiling only
	 * the entry file would miss most real syntax errors. */
	gd = g_dir_open(dir, 0, NULL);

	gstate = PyGILState_Ensure();

	if (gd) {
		while ((fn = g_dir_read_name(gd)) != NULL && ret == SRD_OK) {
			char *file;

			if (!g_str_has_suffix(fn, ".py"))
				continue;
			file = g_build_filename(dir, fn, NULL);
			checked_any = TRUE;
			ret = srd_check_script_syntax(file, error);
			g_free(file);
		}
	} else {
		ret = srd_check_script_syntax(path, error);
	}

	PyGILState_Release(gstate);

	if (gd)
		g_dir_close(gd);

	if (ret == SRD_OK && !checked_any) {
		if (error)
			*error = g_strdup_printf("Decoder script not found: %s",
					module_name);
		ret = SRD_ERR;
	}

	g_free(code);
	g_free(dir);
	g_free(path);

	return ret;
}

/** @} */
