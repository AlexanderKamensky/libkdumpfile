/* Attribute handling.
   Copyright (C) 2015 Petr Tesarik <ptesarik@suse.cz>

   This file is free software; you can redistribute it and/or modify
   it under the terms of either

     * the GNU Lesser General Public License as published by the Free
       Software Foundation; either version 3 of the License, or (at
       your option) any later version

   or

     * the GNU General Public License as published by the Free
       Software Foundation; either version 2 of the License, or (at
       your option) any later version

   or both in parallel, as here.

   libkdumpfile is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received copies of the GNU General Public License and
   the GNU Lesser General Public License along with this program.  If
   not, see <http://www.gnu.org/licenses/>.
*/

#define _GNU_SOURCE

#include "kdumpfile-priv.h"

#include <stdlib.h>
#include <string.h>
#include <errno.h>

static const struct attr_template global_keys[] = {
#define ATTR(dir, key, field, type, ctype)				\
	[GKI_ ## field] = {						\
		key,							\
		&global_keys[GKI_dir_ ## dir],				\
		kdump_ ## type						\
	},
#include "static-attr.def"
#include "global-attr.def"
#undef ATTR
};

static const size_t static_offsets[] = {
#define ATTR(dir, key, field, type, ctype)		\
	[GKI_ ## field] = offsetof(kdump_ctx, field),
#include "static-attr.def"
#undef ATTR
};

#define NR_GLOBAL	ARRAY_SIZE(global_keys)
#define NR_STATIC	ARRAY_SIZE(static_offsets)

/**  Get a modifiable pointer to the static data with a given index.
 * @param ctx  Dump file object.
 * @param idx  Static index.
 * @returns    Pointer to the actual static attribute data.
 * @sa static_attr_data_const
 */
static inline struct attr_data *
static_attr_data(kdump_ctx *ctx, enum global_keyidx idx)
{
	return (struct attr_data*)((char*)ctx + static_offsets[idx]);
}

/**  Get a const pointer to the static data with a given index.
 * @sa static_attr_data
 */
static inline const struct attr_data *
static_attr_data_const(const kdump_ctx *ctx, enum global_keyidx idx)
{
	return (const struct attr_data*)
		((const char*)ctx + static_offsets[idx]);
}

/**  Check if a template denotes statically allocated attribute
 * @param tmpl  Template.
 * @returns     Non-zero if the template's attribute is static.
 */
static inline int
template_static(const struct attr_template *tmpl)
{
	return tmpl >= &global_keys[0] &&
		tmpl < &global_keys[NR_STATIC];
}

/**  Look up a template by its name in one directory.
 * @param dir     Attribute directory.
 * @param key     Key name.
 * @param keylen  Key length.
 * @returns       Template with the given key, or @c NULL if not found.
 *
 * By specifying the key length, it is possible to search for a
 * component of an attribute path without copying the path string.
 */
static const struct attr_template*
lookup_template_dir(const struct attr_template *dir,
		    const char *key, size_t keylen)
{
	const struct attr_template *t;

	for (t = global_keys; t < &global_keys[NR_GLOBAL]; ++t)
		if (t->parent == dir &&
		    !strncmp(key, t->key, keylen) &&
		    t->key[keylen] == '\0')
			return t;
	return NULL;
}

/**  Look up a template by name.
 * @param key  Key name.
 * @returns    Attribute template, or @c NULL if not found.
 */
static const struct attr_template*
lookup_template(const char *key)
{
	const struct attr_template *dir, *t;
	const char *p;

	if (key > GATTR(NR_GLOBAL))
		return &global_keys[-(intptr_t)key];

	dir = &global_keys[GKI_dir_root];
	while ( (p = strchr(key, '.')) ) {
		size_t keylen = p - key;
		++p;

		t = lookup_template_dir(dir, key, keylen);
		if (t && t->type == kdump_directory) {
			dir = t;
			key = p;
		} else
			return NULL; /* directory not found */
	}

	return lookup_template_dir(dir, key, strlen(key));
}

/**  Lookup attribute value by template.
 * @param ctx   Dump file object.
 * @param tmpl  Attribute template.
 * @returns     Stored attribute or @c NULL if not found.
 *
 */
static const struct attr_data*
lookup_data_const(const kdump_ctx *ctx, const struct attr_template *tmpl)
{
	const struct attr_data *parent;
	const struct attr_data *d;

	if (template_static(tmpl)) {
		d = static_attr_data_const(ctx, tmpl - global_keys);
		return static_attr_isset(d) ? d : NULL;
	}

	parent = lookup_data_const(ctx, tmpl->parent);
	if (!parent)
		return NULL;

	for (d = parent->val.directory; d; d = d->next)
		if (d->template == tmpl)
			return d;
	return NULL;
}

/**  Lookup modifiable attribute value by template.
 *
 * This is a wrapper around @c lookup_data_const with a typecast to
 * a non-const pointers, which is necessary to modify attributes.
 *
 * @sa lookup_data_const
 */
static inline struct attr_data*
lookup_data(kdump_ctx *ctx, const struct attr_template *tmpl)
{
	return (struct attr_data*) lookup_data_const(ctx, tmpl);
}

/**  Check if a given attribute is set.
 * @param ctx  Dump file object.
 * @param key  Key name.
 * @returns    Non-zero if the key is known and has a value.
 */
int
attr_isset(const kdump_ctx *ctx, const char *key)
{
	const struct attr_template *t = lookup_template(key);
	return t && !!lookup_data_const(ctx, t);
}

kdump_status
kdump_get_attr(kdump_ctx *ctx, const char *key,
	       struct kdump_attr *valp)
{
	const struct attr_template *t;
	const struct attr_data *d;

	clear_error(ctx);
	t = lookup_template(key);
	if (!t)
		return set_error(ctx, kdump_unsupported, "No such key");

	d = lookup_data_const(ctx, t);
	if (d) {
		valp->type = t->type;
		valp->val = d->val;
		return kdump_ok;
	}

	return set_error(ctx, kdump_nodata, "Key has no value");
}

kdump_status
kdump_enum_attr(kdump_ctx *ctx, const char *path,
		kdump_enum_attr_fn *cb, void *cb_data)
{
	const struct attr_template *t;
	const struct attr_data *parent, *d;

	clear_error(ctx);
	t = lookup_template(path);
	if (!t)
		return set_error(ctx, kdump_unsupported, "No such path");

	parent = lookup_data_const(ctx, t);
	if (!parent)
		return set_error(ctx, kdump_nodata, "Path not instantiated");
	if (parent->template->type != kdump_directory)
		return set_error(ctx, kdump_unsupported,
				 "Path is a leaf attribute");

	for (d = (struct attr_data*)parent->val.directory; d; d = d->next) {
		struct kdump_attr attr;

		attr.type = d->template->type;
		attr.val = d->val;
		if (cb(cb_data, d->template->key, &attr))
			break;
	}
	return kdump_ok;
}

/**  Allocate a new attribute.
 * @param tmpl   Attribute template.
 * @param extra  Extra size to be allocated.
 */
static struct attr_data*
alloc_attr(const struct attr_template *tmpl, size_t extra)
{
	struct attr_data *ret;

	ret = malloc(sizeof(struct attr_data) + extra);
	if (!ret)
		return NULL;

	ret->template = tmpl;
	return ret;
}

/**  Allocate new attribute object by key name.
 * @param ctx         Dump file object.
 * @param[out] pattr  To be filled with the allocated attribute.
 * @param key         Key name.
 * @param extra       Extra size to be allocated.
 * @returns           Error status.
 */
static kdump_status
alloc_attr_by_key(kdump_ctx *ctx, struct attr_data **pattr,
		  const char *key, size_t extra)
{
	const struct attr_template *t;
	struct attr_data *attr;

	t = lookup_template(key);
	if (!t)
		return set_error(ctx, kdump_unsupported, "No such key");

	attr = alloc_attr(t, extra);
	if (!attr)
		return set_error(ctx, kdump_syserr,
				 "Cannot allocate attribute: %s",
				 strerror(errno));

	*pattr = attr;
	return kdump_ok;
}

/**  Free all memory associated with an attribute.
 * @param attr  The attribute to be freed (detached).
 */
static void
free_attr(struct attr_data *attr)
{
	if (attr->template->type == kdump_directory) {
		struct attr_data *node = attr->val.directory;
		while (node) {
			struct attr_data *next = node->next;
			free_attr(node);
			node = next;
		}
	}

	if (template_static(attr->template))
		attr->pprev = NULL;
	else
		free(attr);
}

/**  Add new attribute to a dump file object.
 * @param dir   Parent attribute.
 * @param attr  Complete initialized attribute.
 *
 * This function merely adds the attribute to the dump file object.
 * It does not check for duplicates.
 */
static void
add_attr(struct attr_data *dir, struct attr_data *attr)
{
	/* Link the new node */
	attr->next = dir->val.directory;
	if (attr->next)
		attr->next->pprev = &attr->next;
	dir->val.directory = attr;
	attr->pprev = (struct attr_data**)&dir->val.directory;
}

/**  Instantiate a directory template path.
 * @param ctx   Dump file object.
 * @param tmpl  Directory template.
 * @returns     The newly instantiated attribute,
 *              or @c NULL on allocation failure.
 *
 * Inititalize all paths up the hierarchy for the (leaf) directory
 * denoted by @c tmpl.
 */
static struct attr_data *
instantiate_path(kdump_ctx *ctx, const struct attr_template *tmpl)
{
	struct attr_data *d, *parent;

	d = lookup_data(ctx, tmpl);
	if (d != NULL)
		return d;

	if (tmpl->parent == tmpl) {
		d = &ctx->dir_root;
		d->next = NULL;
		d->pprev = &d->next;
		return d;
	}

	parent = instantiate_path(ctx, tmpl->parent);
	d = template_static(tmpl)
		? static_attr_data(ctx, tmpl - global_keys)
		: alloc_attr(tmpl, 0);
	if (d)
		add_attr(parent, d);
	return d;
}

/**  Delete an attribute.
 * @param attr  Attribute to be deleted.
 *
 * Remove an attribute from its dump file object and free it.
 */
static void
delete_attr(struct attr_data *attr)
{
	*attr->pprev = attr->next;
	if (attr->next)
		attr->next->pprev = attr->pprev;
	free_attr(attr);
}

/**  Cleanup all attributes from a dump file object.
 * @param ctx   Dump file object.
 */
void
cleanup_attr(kdump_ctx *ctx)
{
	free_attr(&ctx->dir_root);
}

/**  Initialize statically allocated attributes
 */
void
init_static_attrs(kdump_ctx *ctx)
{
	enum global_keyidx i;
	for (i = 0; i < NR_STATIC; ++i) {
		struct attr_data *attr = static_attr_data(ctx, i);
		attr->template = &global_keys[i];
	}
}

/**  Set an attribute of a dump file object.
 * @param ctx   Dump file object.
 * @param attr  Attribute (detached).
 * @returns     Error status (see below).
 *
 * This function works both for statically allocated and dynamically
 * allocated attributes.
 */
kdump_status
set_attr(kdump_ctx *ctx, struct attr_data *attr)
{
	struct attr_data *parent, *old;

	parent = instantiate_path(ctx, attr->template->parent);
	if (!parent)
		return set_error(ctx, kdump_syserr,
				 "Cannot instantiate attribute '%s': %s",
				 attr->template->parent->key,
				 strerror(errno));

	for (old = parent->val.directory; old; old = old->next)
		if (old->template == attr->template) {
			delete_attr(old);
			break;
		}

	add_attr(parent, attr);
	return kdump_ok;
}

/**  Set a numeric attribute of a dump file object.
 * @param ctx  Dump file object.
 * @param key  Key name.
 * @param num  Key value (numeric).
 * @returns    Error status.
 */
kdump_status
set_attr_number(kdump_ctx *ctx, const char *key, kdump_num_t num)
{
	struct attr_data *attr;
	kdump_status res;

	res = alloc_attr_by_key(ctx, &attr, key, 0);
	if (res != kdump_ok)
		return res;

	attr->val.number = num;
	return set_attr(ctx, attr);
}

/**  Set an address attribute of a dump file object.
 * @param ctx   Dump file object.
 * @param key   Key name.
 * @param addr  Key value (address).
 * @returns     Error status.
 */
kdump_status
set_attr_address(kdump_ctx *ctx, const char *key, kdump_addr_t addr)
{
	struct attr_data *attr;
	kdump_status res;

	res = alloc_attr_by_key(ctx, &attr, key, 0);
	if (res != kdump_ok)
		return res;

	attr->val.address = addr;
	return set_attr(ctx, attr);
}

/**  Set a string attribute of a dump file object.
 * @param ctx  Dump file object.
 * @param key  Key name.
 * @param str  Key value (string).
 * @returns    Error status.
 */
kdump_status
set_attr_string(kdump_ctx *ctx, const char *key, const char *str)
{
	struct attr_data *attr;
	size_t len = strlen(str);
	char *dynstr;
	kdump_status res;

	res = alloc_attr_by_key(ctx, &attr, key, len + 1);
	if (res != kdump_ok)
		return res;

	dynstr = (char*)(attr + 1);
	memcpy(dynstr, str, len + 1);
	attr->val.string = dynstr;
	return set_attr(ctx, attr);
}

/**  Set a static string attribute of a dump file object.
 * @param ctx  Dump file object.
 * @param key  Key name.
 * @param str  Key value (static string).
 * @returns    Error status.
 */
kdump_status
set_attr_static_string(kdump_ctx *ctx, const char *key, const char *str)
{
	struct attr_data *attr;
	kdump_status res;

	res = alloc_attr_by_key(ctx, &attr, key, 0);
	if (res != kdump_ok)
		return res;

	attr->val.string = str;
	return set_attr(ctx, attr);
}
