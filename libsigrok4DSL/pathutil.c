/*
 * This file is part of the DSView project.
 * DSView is based on PulseView.
 *
 * Copyright (C) 2022 DreamSourceLab <support@dreamsourcelab.com>
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

/*
 * Path tolerant file access.
 *
 * The paths handed to this library by the UI are not guaranteed to be in one
 * encoding: resource paths built from the application directory can arrive as
 * UTF-8, while paths that went through the Win32 code page conversion arrive as
 * the system ANSI code page. Windows' narrow fopen()/open()/stat() only ever
 * decode the bytes with the ANSI code page, so a UTF-8 path containing
 * non-ASCII characters silently resolves to "file does not exist" - which is
 * how "Unable to open FPGA bit file ...: No such file or directory" showed up
 * for an existing bit stream under a directory named with Chinese characters.
 *
 * These helpers widen the path and try the existing interpretations in order,
 * so both encodings work and the failure message keeps the original text.
 */

#include "config.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#include <glib.h>

#include "libsigrok-internal.h"

#ifdef _WIN32
#include <windows.h>
#include <io.h>

static wchar_t *to_wide(const char *path, UINT code_page)
{
    int len;
    wchar_t *wide;

    if (path == NULL)
        return NULL;

    len = MultiByteToWideChar(code_page, 0, path, -1, NULL, 0);
    if (len <= 0)
        return NULL;

    wide = (wchar_t *)g_try_malloc0(sizeof(wchar_t) * len);
    if (wide == NULL)
        return NULL;

    if (MultiByteToWideChar(code_page, 0, path, -1, wide, len) <= 0) {
        g_free(wide);
        return NULL;
    }
    return wide;
}

/* An ASCII path needs no conversion at all. */
static gboolean is_ascii(const char *path)
{
    const unsigned char *p = (const unsigned char *)path;
    while (*p) {
        if (*p >= 0x80)
            return FALSE;
        p++;
    }
    return TRUE;
}

static wchar_t *existing_wide(const char *path)
{
    wchar_t *wide_utf8, *wide_ansi;

    wide_utf8 = to_wide(path, CP_UTF8);
    if (wide_utf8 && GetFileAttributesW(wide_utf8) != INVALID_FILE_ATTRIBUTES)
        return wide_utf8;

    wide_ansi = to_wide(path, CP_ACP);
    if (wide_ansi && GetFileAttributesW(wide_ansi) != INVALID_FILE_ATTRIBUTES) {
        g_free(wide_utf8);
        return wide_ansi;
    }
    g_free(wide_ansi);

    /* Nothing exists: keep the UTF-8 reading so the caller reports the path
     * it was given rather than a code page round trip of it. */
    return wide_utf8;
}

static wchar_t *mode_to_wide(const char *mode)
{
    wchar_t *wide;
    size_t i;

    wide = (wchar_t *)g_try_malloc0(sizeof(wchar_t) * (strlen(mode) + 1));
    if (wide == NULL)
        return NULL;
    for (i = 0; mode[i] != '\0'; i++)
        wide[i] = (wchar_t)mode[i];
    return wide;
}
#endif

/**
 * Open a file whose path may be UTF-8 or the system ANSI code page.
 *
 * @param path Path as handed to the library.
 * @param mode Narrow fopen() mode string, e.g. "rb".
 *
 * @return An open FILE*, or NULL with errno set as fopen() would.
 */
SR_PRIV FILE *sr_fopen_path(const char *path, const char *mode)
{
#ifdef _WIN32
    wchar_t *wide_path, *wide_mode;
    FILE *file;

    if (path == NULL || mode == NULL) {
        errno = EINVAL;
        return NULL;
    }
    if (is_ascii(path))
        return fopen(path, mode);

    wide_path = existing_wide(path);
    wide_mode = mode_to_wide(mode);
    if (wide_path == NULL || wide_mode == NULL) {
        g_free(wide_path);
        g_free(wide_mode);
        errno = EINVAL;
        return NULL;
    }

    file = _wfopen(wide_path, wide_mode);
    g_free(wide_path);
    g_free(wide_mode);
    return file;
#else
    return fopen(path, mode);
#endif
}

/**
 * Read the size of an existing file, tolerating both path encodings.
 *
 * @return SR_OK on success, SR_ERR when the file cannot be inspected.
 */
SR_PRIV int sr_file_size(const char *path, uint64_t *size)
{
#ifdef _WIN32
    struct _stat64 st;
    wchar_t *wide_path;
    int ret;

    if (path == NULL || size == NULL)
        return SR_ERR;
    if (is_ascii(path)) {
        if (_stat64(path, &st) != 0)
            return SR_ERR;
        *size = (uint64_t)st.st_size;
        return SR_OK;
    }

    wide_path = existing_wide(path);
    if (wide_path == NULL)
        return SR_ERR;
    ret = _wstat64(wide_path, &st);
    g_free(wide_path);
    if (ret != 0)
        return SR_ERR;

    *size = (uint64_t)st.st_size;
    return SR_OK;
#else
    struct stat st;

    if (path == NULL || size == NULL || stat(path, &st) == -1)
        return SR_ERR;
    *size = (uint64_t)st.st_size;
    return SR_OK;
#endif
}

/**
 * Open a file for reading only, for callers that use the fd interface.
 *
 * @return A file descriptor, or -1 with errno set.
 */
SR_PRIV int sr_open_path_ro(const char *path)
{
#ifdef _WIN32
    wchar_t *wide_path;
    int fd;

    if (path == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (is_ascii(path))
        return _open(path, _O_RDONLY | _O_BINARY);

    wide_path = existing_wide(path);
    if (wide_path == NULL) {
        errno = EINVAL;
        return -1;
    }
    fd = _wopen(wide_path, _O_RDONLY | _O_BINARY);
    g_free(wide_path);
    return fd;
#else
    return open(path, O_RDONLY);
#endif
}
