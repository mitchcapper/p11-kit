/*
 * Copyright (C) 2012-2013 Red Hat Inc.
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
 * Author: Stef Walter <stefw@redhat.com>
 */

#include "config.h"

#include "asn1.h"
#include "attrs.h"
#include "builder.h"
#include "compat.h"
#define P11_DEBUG_FLAG P11_DEBUG_TRUST
#include "debug.h"
#include "errno.h"
#include "message.h"
#include "module.h"
#include "parser.h"
#include "path.h"
#include "pkcs11.h"
#include "pkcs11x.h"
#include "token.h"

#include <sys/stat.h>
#include <sys/types.h>

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct _p11_token {
	p11_parser *parser;       /* Parser we use to load files */
	p11_index *index;         /* Index we load objects into */
	p11_builder *builder;     /* Expands objects and applies policy */
	p11_dict *loaded;         /* stat structs for loaded files, track reloads */

	char *path;               /* Main path to load from */
	char *anchors;            /* Path to load anchors from */
	char *blacklist;          /* Path to load blacklist from */
	char *label;              /* The token label */
	CK_SLOT_ID slot;          /* The slot id */

	bool checked_writable;
	bool is_writable;
};

static bool
loader_is_necessary (p11_token *token,
                     const char *filename,
                     struct stat *sb)
{
	struct stat *last;

	last = p11_dict_get (token->loaded, filename);

	/* Never seen this before, load it */
	if (last == NULL)
		return true;

	/*
	 * If any of these are different assume that the file
	 * needs to be reloaded
	 */
	return (sb->st_mode != last->st_mode ||
	        sb->st_mtime != last->st_mtime ||
	        sb->st_size != last->st_size);
}

static void
loader_was_loaded (p11_token *token,
                   const char *filename,
                   struct stat *sb)
{
	char *key;

	key = strdup (filename);
	return_if_fail (key != NULL);

	sb = memdup (sb, sizeof (struct stat));
	return_if_fail (sb != NULL);

	/* Track the info about this file, so we don't reload unnecessarily */
	if (!p11_dict_set (token->loaded, key, sb))
		return_if_reached ();
}

static void
loader_not_loaded (p11_token *token,
                   const char *filename)
{
	/* No longer track info about this file */
	p11_dict_remove (token->loaded, filename);
}

static void
loader_gone_file (p11_token *token,
                  const char *filename)
{
	CK_ATTRIBUTE origin[] = {
		{ CKA_X_ORIGIN, (void *)filename, strlen (filename) },
		{ CKA_INVALID },
	};

	CK_RV rv;

	/* Remove everything at this origin */
	rv = p11_index_replace_all (token->index, origin, CKA_INVALID, NULL);
	return_if_fail (rv == CKR_OK);

	/* No longer track info about this file */
	loader_not_loaded (token, filename);
}

static int
loader_load_file (p11_token *token,
                  const char *filename,
                  struct stat *sb)
{
	CK_ATTRIBUTE origin[] = {
		{ CKA_X_ORIGIN, (void *)filename, strlen (filename) },
		{ CKA_INVALID },
	};

	CK_BBOOL modifiablev;

	CK_ATTRIBUTE modifiable = {
		CKA_MODIFIABLE,
		&modifiablev,
		sizeof (modifiablev)
	};

	p11_array *parsed;
	CK_RV rv;
	int flags;
	int ret;
	int i;

	/* Check if this file is already loaded */
	if (!loader_is_necessary (token, filename, sb))
		return 0;

	flags = P11_PARSE_FLAG_NONE;

	/* If it's in the anchors subdirectory, treat as an anchor */
	if (p11_path_prefix (filename, token->anchors))
		flags = P11_PARSE_FLAG_ANCHOR;

	/* If it's in the blacklist subdirectory, treat as a blacklist */
	else if (p11_path_prefix (filename, token->blacklist))
		flags = P11_PARSE_FLAG_BLACKLIST;

	/* If the token is just one path, then assume they are anchors */
	else if (strcmp (filename, token->path) == 0 && !S_ISDIR (sb->st_mode))
		flags = P11_PARSE_FLAG_ANCHOR;

	ret = p11_parse_file (token->parser, filename, flags);

	switch (ret) {
	case P11_PARSE_SUCCESS:
		p11_debug ("loaded: %s", filename);
		break;
	case P11_PARSE_UNRECOGNIZED:
		p11_debug ("skipped: %s", filename);
		loader_gone_file (token, filename);
		return 0;
	default:
		p11_debug ("failed to parse: %s", filename);
		loader_gone_file (token, filename);
		return 0;
	}

	/* TODO: We should check if in the right format */
	modifiablev = CK_FALSE;

	/* Update each parsed object with the origin */
	parsed = p11_parser_parsed (token->parser);
	for (i = 0; i < parsed->num; i++) {
		parsed->elem[i] = p11_attrs_build (parsed->elem[i], origin, &modifiable, NULL);
		return_val_if_fail (parsed->elem[i] != NULL, 0);
	}

	p11_index_batch (token->index);

	/* Now place all of these in the index */
	rv = p11_index_replace_all (token->index, origin, CKA_CLASS, parsed);

	p11_index_finish (token->index);

	if (rv != CKR_OK) {
		p11_message ("couldn't load file into objects: %s", filename);
		return 0;
	}

	loader_was_loaded (token, filename, sb);
	return 1;
}

static int
loader_load_if_file (p11_token *token,
                     const char *path)
{
	struct stat sb;

	if (stat (path, &sb) < 0) {
		if (errno == ENOENT) {
			p11_message ("couldn't stat path: %s: %s",
			             path, strerror (errno));
		}

	} else if (!S_ISDIR (sb.st_mode)) {
		return loader_load_file (token, path, &sb);
	}

	/* Perhaps the file became unloadable, so track properly */
	loader_gone_file (token, path);
	return 0;
}

static int
loader_load_directory (p11_token *token,
                       const char *directory,
                       p11_dict *present)
{
	p11_dictiter iter;
	struct dirent *dp;
	char *path;
	int total = 0;
	int ret;
	DIR *dir;

	/* First we load all the modules */
	dir = opendir (directory);
	if (!dir) {
		p11_message ("couldn't list directory: %s: %s",
		             directory, strerror (errno));
		loader_not_loaded (token, directory);
		return 0;
	}

	/* We're within a global mutex, so readdir is safe */
	while ((dp = readdir (dir)) != NULL) {
		path = p11_path_build (directory, dp->d_name, NULL);
		return_val_if_fail (path != NULL, -1);

		ret = loader_load_if_file (token, path);
		return_val_if_fail (ret >=0, -1);
		total += ret;

		/* Make note that this file was seen */
		p11_dict_remove (present, path);

		free (path);
	}

	closedir (dir);

	/* All other files that were present, not here now */
	p11_dict_iterate (present, &iter);
	while (p11_dict_next (&iter, (void **)&path, NULL))
		loader_gone_file (token, path);

	return total;
}

static int
loader_load_path (p11_token *token,
                  const char *path)
{
	p11_dictiter iter;
	p11_dict *present;
	char *filename;
	struct stat sb;
	int total;
	int ret;

	if (stat (path, &sb) < 0) {
		if (errno != ENOENT) {
			p11_message ("cannot access trust certificate path: %s: %s",
			             path, strerror (errno));
		}
		loader_gone_file (token, path);
		return 0;
	}

	if (S_ISDIR (sb.st_mode)) {

		/* All the files we know about at this path */
		present = p11_dict_new (p11_dict_str_hash, p11_dict_str_equal, NULL, NULL);
		p11_dict_iterate (token->loaded, &iter);
		while (p11_dict_next (&iter, (void **)&filename, NULL)) {
			if (p11_path_prefix (filename, path)) {
				if (!p11_dict_set (present, filename, filename))
					return_val_if_reached (-1);
			}
		}

		/* If the directory has changed, reload it */
		if (loader_is_necessary (token, path, &sb)) {
			ret = loader_load_directory (token, path, present);

		/* Directory didn't change, but maybe files changed? */
		} else {
			total = 0;
			p11_dict_iterate (present, &iter);
			while (p11_dict_next (&iter, (void **)&filename, NULL)) {
				ret = loader_load_if_file (token, filename);
				return_val_if_fail (ret >= 0, ret);
				total += ret;
			}
		}

		p11_dict_free (present);
		loader_was_loaded (token, path, &sb);

	} else {
		ret = loader_load_file (token, path, &sb);
	}

	return ret;
}

static int
load_builtin_objects (p11_token *token)
{
	CK_OBJECT_CLASS builtin = CKO_NSS_BUILTIN_ROOT_LIST;
	CK_BBOOL vtrue = CK_TRUE;
	CK_BBOOL vfalse = CK_FALSE;
	CK_RV rv;

	const char *trust_anchor_roots = "Trust Anchor Roots";
	CK_ATTRIBUTE builtin_root_list[] = {
		{ CKA_CLASS, &builtin, sizeof (builtin) },
		{ CKA_TOKEN, &vtrue, sizeof (vtrue) },
		{ CKA_PRIVATE, &vfalse, sizeof (vfalse) },
		{ CKA_MODIFIABLE, &vfalse, sizeof (vfalse) },
		{ CKA_LABEL, (void *)trust_anchor_roots, strlen (trust_anchor_roots) },
		{ CKA_INVALID },
	};

	p11_index_batch (token->index);
	rv = p11_index_take (token->index, p11_attrs_dup (builtin_root_list), NULL);
	return_val_if_fail (rv == CKR_OK, 0);
	p11_index_finish (token->index);
	return 1;
}

int
p11_token_load (p11_token *token)
{
	int total = 0;
	int ret;

	ret = loader_load_path (token, token->path);
	return_val_if_fail (ret >= 0, -1);
	total += ret;

	ret = loader_load_path (token, token->anchors);
	return_val_if_fail (ret >= 0, -1);
	total += ret;

	ret = loader_load_path (token, token->blacklist);
	return_val_if_fail (ret >= 0, -1);
	total += ret;

	return total;
}

void
p11_token_reload (p11_token *token,
                  CK_ATTRIBUTE *attrs)
{
	CK_ATTRIBUTE *attr;
	struct stat sb;
	char *origin;

	attr = p11_attrs_find (attrs, CKA_X_ORIGIN);
	if (attr == NULL)
		return;

	origin = strndup (attr->pValue, attr->ulValueLen);
	return_if_fail (origin != NULL);

	if (stat (origin, &sb) < 0) {
		if (errno == ENOENT) {
			loader_gone_file (token, origin);
		} else {
			p11_message ("cannot access trust file: %s: %s",
			             origin, strerror (errno));
		}
	} else {
		loader_load_file (token, origin, &sb);
	}
}

void
p11_token_free (p11_token *token)
{
	if (!token)
		return;

	p11_index_free (token->index);
	p11_parser_free (token->parser);
	p11_builder_free (token->builder);
	p11_dict_free (token->loaded);
	free (token->path);
	free (token->anchors);
	free (token->blacklist);
	free (token->label);
	free (token);
}

p11_token *
p11_token_new (CK_SLOT_ID slot,
               const char *path,
               const char *label)
{
	p11_token *token;

	return_val_if_fail (path != NULL, NULL);
	return_val_if_fail (label != NULL, NULL);

	token = calloc (1, sizeof (p11_token));
	return_val_if_fail (token != NULL, NULL);

	token->builder = p11_builder_new (P11_BUILDER_FLAG_TOKEN);
	return_val_if_fail (token->builder != NULL, NULL);

	token->index = p11_index_new (p11_builder_build,
	                              p11_builder_changed,
	                              token->builder);
	return_val_if_fail (token->index != NULL, NULL);

	token->parser = p11_parser_new (p11_builder_get_cache (token->builder));
	return_val_if_fail (token->parser != NULL, NULL);

	token->loaded = p11_dict_new (p11_dict_str_hash, p11_dict_str_equal, free, free);
	return_val_if_fail (token->loaded != NULL, NULL);

	token->path = strdup (path);
	return_val_if_fail (token->path != NULL, NULL);

	token->anchors = p11_path_build (token->path, "anchors", NULL);
	return_val_if_fail (token->anchors != NULL, NULL);

	token->blacklist = p11_path_build (token->path, "blacklist", NULL);
	return_val_if_fail (token->blacklist != NULL, NULL);

	token->label = strdup (label);
	return_val_if_fail (token->label != NULL, NULL);

	token->slot = slot;

	load_builtin_objects (token);

	p11_debug ("token: %s: %s", token->label, token->path);
	return token;
}

const char *
p11_token_get_label (p11_token *token)
{
	return_val_if_fail (token != NULL, NULL);
	return token->label;
}

const char *
p11_token_get_path (p11_token *token)
{
	return_val_if_fail (token != NULL, NULL);
	return token->path;
}

CK_SLOT_ID
p11_token_get_slot (p11_token *token)
{
	return_val_if_fail (token != NULL, 0);
	return token->slot;
}

p11_index *
p11_token_index (p11_token *token)
{
	return_val_if_fail (token != NULL, NULL);
	return token->index;
}

static bool
check_writable_directory (const char *path)
{
	struct stat sb;
	char *parent;
	bool ret;

	if (access (path, W_OK) == 0)
		return stat (path, &sb) == 0 && S_ISDIR (sb.st_mode);

	switch (errno) {
	case EACCES:
		return false;
	case ENOENT:
		parent = p11_path_parent (path);
		if (parent == NULL)
			ret = false;
		else
			ret = check_writable_directory (parent);
		free (parent);
		return ret;
	default:
		p11_message ("couldn't access: %s: %s", path, strerror (errno));
		return false;
	}
}

bool
p11_token_is_writable (p11_token *token)
{
	/*
	 * This function attempts to determine whether a later write
	 * to this token will succeed so we can setup the appropriate
	 * token flags. Yes, it is racy, but that's inherent to the problem.
	 */

	if (!token->checked_writable) {
		token->is_writable = check_writable_directory (token->path);
		token->checked_writable = true;
	}

	return token->is_writable;
}
