/*************************************************************************/
/* Copyright (c) 2004                                                    */
/* Daniel Sleator, David Temperley, and John Lafferty                    */
/* Copyright (c) 2013 Linas Vepstas                                      */
/* All rights reserved                                                   */
/*                                                                       */
/* Use of the link grammar parsing system is subject to the terms of the */
/* license set forth in the LICENSE file included with this software.    */
/* This license allows free redistribution and use in source and binary  */
/* forms, with or without modification, subject to certain conditions.   */
/*                                                                       */
/*************************************************************************/

#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <errno.h>

#include "print-util.h"
#include "utilities.h"
#include "wcwidth.h"

/**
 * Return the width, in text-column-widths, of the utf8-encoded
 * string.  This is needed when printing formatted strings.
 * European languages will typically have widths equal to the
 * `mblen` value below (returned by mbsrtowcs); they occupy one
 * column-width per code-point.  The CJK ideographs occupy two
 * column-widths per code-point. No clue about what happens for
 * Arabic, or others.  See wcwidth.c for details.
 */
size_t utf8_strwidth(const char *s)
{
	size_t mblen;

#ifdef _WIN32
	mblen = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0) - 1;
#else
	mblen = mbsrtowcs(NULL, &s, 0, NULL);
#endif
	if ((int)mblen < 0)
	{
		prt_error("Warning: Error in utf8_strwidth(%s)\n", s);
		return 1 /* XXX */;
	}

	wchar_t *ws = alloca((mblen + 1) * sizeof(wchar_t));

#ifdef _WIN32
	MultiByteToWideChar(CP_UTF8, 0, s, -1, ws, mblen);
#else
	mbstate_t mbss;
	memset(&mbss, 0, sizeof(mbss));
	mbsrtowcs(ws, &s, mblen, &mbss);
#endif /* _WIN32 */

	int glyph_width = 0;
	for (size_t i = 0; i < mblen; i++)
	{
		int w = mk_wcwidth(ws[i]);

		// If w<0 then its not vaid UTF8, but garbage.  Many
		// terminals will print this with a weird boxed font
		// that is two columns wide, showing the hex value in it.
		if (w < 0) w = 2;
		glyph_width += w;
	}
	return glyph_width;
}

/**
 * Return the width, in text-column-widths, of the utf8-encoded
 * character.
 *
 * The mbstate_t argument is not used, since we convert only from utf-8.
 * FIXME: This function (along with other places that use mbrtowc()) need
 * to be fixed for Windows (utf-16 wchar_t).
 * Use char32_t with mbrtoc32() instead of mbrtowc().
 */
size_t utf8_charwidth(const char *s)
{
	wchar_t wc;

	int n = mbrtowc(&wc, s, MB_LEN_MAX, NULL);
	if (n == 0) return 0;
	if (n < 0)
	{
		prt_error("Error: charwidth(%s): mbrtowc() returned %d\n", s, n);
		return 1 /* XXX */;
	}

	return mk_wcwidth(wc);
}

/**
 * Return the number of characters in the longest initial substring
 * which has a text-column-width of not greater than max_width.
 */
size_t utf8_num_char(const char *s, size_t max_width)
{
	size_t total_bytes = 0;
	size_t glyph_width = 0;
	int n = 0;
	wchar_t wc;

	do
	{
		total_bytes += n;
		n = mbrtowc(&wc, s+total_bytes, MB_LEN_MAX, NULL);
		if (n == 0) break;
		if (n < 0)
		{
			prt_error("Error: utf8_num_char(%s, %zu): mbrtowc() returned %d\n",
			          s, max_width, n);
			return 1 /* XXX */;
		}

		glyph_width += mk_wcwidth(wc);
		//printf("N %zu G %zu;", total_bytes, glyph_width);
	}
	while (glyph_width <= max_width);
	printf("\n");

	return total_bytes;
}

/* ============================================================= */

/**
 * Append to a dynamic string with vprintf-like formatting.
 * @return The number of appended bytes, or a negative value on error.
 *
 * Note: As in the rest of the LG library, we assume here C99 library
 * compliance (without it, this code would be buggy).
 */
int vappend_string(dyn_str * string, const char *fmt, va_list args)
{
#define TMPLEN 1024 /* Big enough for a possible error message, see below */
	char temp_buffer[TMPLEN];
	char *temp_string = temp_buffer;
	int templen;
	va_list copy_args;

	va_copy(copy_args, args);
	templen = vsnprintf(temp_string, TMPLEN, fmt, copy_args);
	va_end(copy_args);

	if (templen < 0) goto error;
	if (0)
	{
		if (fmt[0] == '(') { errno=2; goto error;} /* Test the error reporting. */
	}

	if (templen >= TMPLEN)
	{
		/* TMPLEN is too small - use a bigger buffer. Couldn't actually
		 * find any example of entering this code with templen>=1024... */
		temp_string = alloca(templen+1);
		templen = vsnprintf(temp_string, templen+1, fmt, args);
		if (templen < 0) goto error;
	}
	va_end(args);

	patch_subscript_marks(temp_string);
	dyn_strcat(string, temp_string);
	return templen;

error:
	{
		/* Some error has occurred */
		const char msg[] = "[vappend_string(): ";
		strcpy(temp_buffer, msg);
		strerror_r(errno, temp_buffer+sizeof(msg)-1, TMPLEN-sizeof(msg));
		strcat(temp_buffer, "]");
		dyn_strcat(string, temp_string);
		return templen;
	}
}

/**
 * Append to a dynamic string with printf-like formatting.
 * @return The number of appended bytes, or a negative value on error.
 */
int append_string(dyn_str * string, const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);

	return vappend_string(string, fmt, args);
}

size_t append_utf8_char(dyn_str * string, const char * mbs)
{
	/* Copy exactly one multi-byte character to buf */
	char buf[10];
	size_t n = utf8_charlen(mbs);

	assert(n<10, "Multi-byte character is too long!");
	strncpy(buf, mbs, n);
	buf[n] = 0;
	dyn_strcat(string, buf);
	return n;
}
