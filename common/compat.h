/*
 * Copyright (c) 2011 Collabora Ltd.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *     * Redistributions of source code must retain the above
 *       copyright notice, this list of conditions and the
 *       following disclaimer.
 *     * Redistributions in binary form must reproduce the
 *       above copyright notice, this list of conditions and
 *       the following disclaimer in the documentation and/or
 *       other materials provided with the distribution.
 *     * The names of contributors to this software may not be
 *       used to endorse or promote products derived from this
 *       software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 *
 * Author: Stef Walter <stefw@collabora.co.uk>
 */

#ifndef __COMPAT_H__
#define __COMPAT_H__

#include "config.h"

#if !defined(__cplusplus) && (__GNUC__ > 2)
#define GNUC_PRINTF(x, y) __attribute__((__format__(__printf__, x, y)))
#else
#define GNUC_PRINTF(x, y)
#endif

#if __GNUC__ >= 4
#define GNUC_NULL_TERMINATED __attribute__((__sentinel__))
#else
#define GNUC_NULL_TERMINATED
#endif

#ifndef HAVE_GETPROGNAME
const char * getprogname (void);
#endif

#ifdef HAVE_ERR_H
#include <err.h>

#else /* !HAVE_ERR_H */

#include <stdarg.h>
void err_set_file (void *fp);
void err_set_exit (void (*ef)(int));
void err (int eval, const char *fmt, ...) GNUC_PRINTF (2, 3);
void verr (int eval, const char *fmt, va_list ap);
void errc (int eval, int code, const char *fmt, ...) GNUC_PRINTF (3, 4);
void verrc (int eval, int code, const char *fmt, va_list ap);
void errx (int eval, const char *fmt, ...) GNUC_PRINTF (2, 3);
void verrx (int eval, const char *fmt, va_list ap);
void warn (const char *fmt, ...) GNUC_PRINTF (1, 2);
void vwarn (const char *fmt, va_list ap);
void warnc (int code, const char *fmt, ...) GNUC_PRINTF (2, 3);
void vwarnc (int code, const char *fmt, va_list ap);
void warnx (const char *fmt, ...) GNUC_PRINTF (1, 2);
void vwarnx (const char *fmt, va_list ap);

#endif /* !HAVE_ERR_H */

#ifdef	HAVE_ERRNO_H
#include <errno.h>
#endif	/* HAVE_ERRNO_H */

#endif /* __COMPAT_H__ */
