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

static const struct attr_template global_keys[] = {
#define ATTR(dir, key, field, type, ctype, ...)				\
	[GKI_ ## field] = {						\
		key,							\
		&global_keys[GKI_dir_ ## dir],				\
		kdump_ ## type,						\
		##__VA_ARGS__						\
	},
#include "static-attr.def"
#include "global-attr.def"
#undef ATTR
};

static const size_t static_offsets[] = {
#define ATTR(dir, key, field, type, ctype, ...)				\
	[GKI_ ## field - GKI_static_first] = offsetof(kdump_ctx, field),
#include "static-attr.def"
#undef ATTR
};

/**  Get a pointer to the static value with a given index.
 * @param ctx  Dump file object.
 * @param idx  Static index.
 * @returns    Pointer to the static attribute value.
 */
static inline union kdump_attr_value *
static_attr_value(kdump_ctx *ctx, enum global_keyidx idx)
{
	return (union kdump_attr_value*)
		((char*)ctx + static_offsets[idx - GKI_static_first]);
}

/**  Calculate the hash index of a key path.
 * @param key  Key path.
 * @returns    Desired index in the hash table.
 */
static unsigned
key_hash_index(const char *key)
{
	return fold_hash(string_hash(key), ATTR_HASH_BITS);
}

/**  Calculate the hash index of a partial key path.
 * @param key     Key path.
 * @param keylen  Initial portion of @c key to be considered.
 * @returns       Desired index in the hash table.
 */
static unsigned
part_hash_index(const char *key, size_t keylen)
{
	return fold_hash(mem_hash(key, keylen), ATTR_HASH_BITS);
}

/**  Get the length of an attribute path
 * @param attr  Attribute data.
 * @returns     Length of the full path string.
 *
 * The returned length does not include the terminating NUL character.
 */
static size_t
attr_pathlen(const struct attr_data *attr)
{
	const struct attr_data *d;
	size_t len = 0;

	for (d = attr; d->parent != d; d = d->parent) {
		len += strlen(d->template->key);
		if (d != attr)
			++len;	/* for the separating dot ('.') */
	}
	return len;
}

/**  Construct an attribute's key path.
 * @param attr  Attribute data.
 * @param endp  Pointer to the __end__ of the path buffer.
 * @returns     Beginning of the path buffer.
 *
 * The output buffer must be big enough to hold the full path. You can
 * use @c attr_pathlen to calculate the required length.
 * Note that the resulting path is a NUL-terminated string, and the buffer
 * must also contain space for this terminating NUL character.
 */
static char *
make_attr_path(const struct attr_data *attr, char *endp)
{
	const struct attr_data *d;

	*endp = '\0';
	for (d = attr; d->parent != d; d = d->parent) {
		size_t len = strlen(d->template->key);
		if (d != attr)
			*(--endp) = '.';
		endp -= len;
		memcpy(endp, d->template->key, len);
	}
	return endp;
}

/**  Compare if attribute data correponds to a given key.
 * @param attr  Attribute data.
 * @param key   Key path.
 * @param len   Initial portion of @c key to be considered.
 * @returns     Zero if the data is stored under the given key,
 *              non-zero otherwise.
 */
static int
keycmp(const struct attr_data *attr, const char *key, size_t len)
{
	const char *p;

	while ( (p = memrchr(key, '.', len)) ) {
		size_t partlen = key + len - p - 1;
		int res = strncmp(attr->template->key, p + 1, partlen);
		if (res)
			return res;
		if (attr->template->key[partlen] != '\0')
			return 1;
		attr = attr->parent;
		len = p - key;
	}

	return memcmp(attr->template->key, key, len);
}

/**  Look up attribute value by name.
 * @param ctx     Dump file object.
 * @param key     Key name.
 * @param keylen  Initial portion of @c key to be considered.
 * @returns       Stored attribute or @c NULL if not found.
 */
static struct attr_data*
lookup_attr_part(const kdump_ctx *ctx, const char *key, size_t keylen)
{
	unsigned ehash, i;
	const struct attr_hash *tbl;

	i = part_hash_index(key, keylen);
	ehash = (i + ATTR_HASH_FUZZ) % ATTR_HASH_SIZE;
	do {
		tbl = &ctx->attr;
		do {
			if (!tbl->table[i].parent)
				break;
			if (!keycmp(&tbl->table[i], key, keylen))
				return (struct attr_data*) &tbl->table[i];
			tbl = tbl->next;
		} while (tbl);
		i = (i + 1) % ATTR_HASH_SIZE;
	} while (i != ehash);

	return NULL;
}

/**  Look up raw attribute data by name.
 * @param ctx   Dump file object.
 * @param key   Key name.
 * @returns     Stored attribute or @c NULL if not found.
 *
 * This function does not check whether an attribute is set, or not.
 */
static struct attr_data *
lookup_attr_raw(const kdump_ctx *ctx, const char *key)
{
	if (!key || key > GATTR(NR_GLOBAL_ATTRS))
		return ctx->global_attrs[-(intptr_t)key];

	return lookup_attr_part(ctx, key, strlen(key));
}

/**  Look up attribute data by name.
 * @param ctx   Dump file object.
 * @param key   Key name.
 * @returns     Stored attribute or @c NULL if not found.
 */
const struct attr_data *
lookup_attr(const kdump_ctx *ctx, const char *key)
{
	const struct attr_data *d = lookup_attr_raw(ctx, key);
	return d && attr_isset(d) ? d : NULL;
}

/**  Look up attribute parent by name.
 * @param ctx   Dump file object.
 * @param pkey  Pointer to key name. This pointer is updated to the last
 *              path component on return.
 * @returns     Stored attribute or @c NULL if not found.
 *
 * This function does not check whether an attribute is set, or not.
 */
static struct attr_data *
lookup_attr_parent(const kdump_ctx *ctx, const char **pkey)
{
	const char *key, *p;

	p = strrchr(*pkey, '.');
	if (!p)
		return ctx->global_attrs[GKI_dir_root];

	key = *pkey;
	*pkey = p + 1;
	return lookup_attr_part(ctx, key, p - key);
}

/**  Link a new attribute to its parent.
 * @param dir   Parent attribute.
 * @param attr  Complete initialized attribute.
 *
 * This function merely adds the attribute to the dump file object.
 * It does not check for duplicates.
 */
static void
link_attr(struct attr_data *dir, struct attr_data *attr)
{
	/* Link the new node */
	attr->next = dir->dir;
	if (attr != dir)
		dir->dir = attr;
	attr->parent = dir;
}

/**  Allocate an attribute from the hash table.
 * @param ctx     Dump file object.
 * @param parent  Parent directory, or @c NULL.
 * @param tmpl    Attribute template.
 * @returns       Attribute data, or @c NULL on allocation failure.
 */
static struct attr_data *
alloc_attr(kdump_ctx *ctx, struct attr_data *parent,
	   const struct attr_template *tmpl)
{
	struct attr_data tmp;
	size_t pathlen;
	char *path;
	unsigned hash, ehash, i;
	struct attr_hash *tbl, *newtbl;

	tmp.parent = parent ?: &tmp;
	tmp.template = tmpl;
	pathlen = attr_pathlen(&tmp);
	path = alloca(pathlen + 1);
	make_attr_path(&tmp, path + pathlen);

	i = hash = key_hash_index(path);
	ehash = (i + ATTR_HASH_FUZZ) % ATTR_HASH_SIZE;
	do {
		newtbl = &ctx->attr;
		do {
			tbl = newtbl;
			if (!tbl->table[i].parent)
				return &tbl->table[i];
			newtbl = tbl->next;
		} while (newtbl);
		i = (i + 1) % ATTR_HASH_SIZE;
	} while (i != ehash);

	newtbl = calloc(1, sizeof(struct attr_hash));
	if (!newtbl)
		return NULL;
	tbl->next = newtbl;

	return &newtbl->table[hash];
}

/**  Clear (unset) an attribute.
 */
static void
clear_attr(struct attr_data *attr)
{
	if (attr->template->type == kdump_directory) {
		struct attr_data *child = attr->dir;
		while (child) {
			struct attr_data *next = child->next;
			clear_attr(child);
			child = next;
		}
	}

	attr->isset = 0;
	if (attr->dynstr) {
		attr->dynstr = 0;
		free((void*) attr_value(attr)->string);
	}
}

/**  Allocate a new attribute in any directory.
 * @param ctx     Dump file object.
 * @param parent  Parent directory. If @c NULL, create a self-owned
 *                attribute (root directory).
 * @param tmpl    Attribute template.
 * @returns       Attribute data, or @c NULL on allocation failure.
 */
static struct attr_data *
new_attr(kdump_ctx *ctx, struct attr_data *parent,
	 const struct attr_template *tmpl)
{
	struct attr_data *attr;

	attr = alloc_attr(ctx, parent, tmpl);
	if (!attr)
		return attr;

	attr->template = tmpl;
	if (!parent)
		parent = attr;
	link_attr(parent, attr);
	return attr;
}

/**  Add an attribute template.
 * @param ctx   Dump file object.
 */
kdump_status
add_attr_template(kdump_ctx *ctx, const char *path,
		  enum kdump_attr_type type)
{
	struct dyn_attr_template *dt;
	struct attr_data *attr, *parent;
	char *keyname;

	attr = lookup_attr_raw(ctx, path);
	if (attr) {
		if (attr->template->type == type)
			return kdump_ok;

		return set_error(ctx, kdump_invalid,
				 "Type conflict with existing template");
	}

	parent = lookup_attr_parent(ctx, &path);
	if (!parent)
		return set_error(ctx, kdump_nokey, "No such path");

	if (parent->template->type != kdump_directory)
		return set_error(ctx, kdump_invalid,
				 "Path is a leaf attribute");

	dt = malloc(sizeof *dt + strlen(path) + 1);
	if (!dt)
		return set_error(ctx, kdump_syserr,
				 "Cannot allocate attribute template");

	keyname = (char*) (dt + 1);
	strcpy(keyname, path);
	dt->template.key = keyname;
	dt->template.parent = parent->template;
	dt->template.type = type;
	dt->template.ops = NULL;

	attr = new_attr(ctx, parent, &dt->template);
	if (!attr) {
		free(dt);
		return set_error(ctx, kdump_syserr,
				 "Cannot allocate attribute");
	}

	dt->next = ctx->tmpl;
	ctx->tmpl = dt;

	return kdump_ok;
}

/**  Instantiate a directory path.
 * @param attr  Leaf attribute.
 * @returns     The newly instantiated attribute,
 *              or @c NULL on allocation failure.
 *
 * Inititalize all paths up the hierarchy for the (leaf) directory
 * denoted by @c tmpl.
 */
static void
instantiate_path(struct attr_data *attr)
{
	while (!attr_isset(attr)) {
		attr->isset = 1;
		if (attr == attr->parent)
			break;
		attr = attr->parent;
	}
}

/**  Clear (unset) all attributes.
 * @param ctx   Dump file object.
 */
void
clear_attrs(kdump_ctx *ctx)
{
	clear_attr(ctx->global_attrs[GKI_dir_root]);
}

/**  Free all memory used by attributes.
 * @param ctx  Dump file object.
 */
void
cleanup_attr(kdump_ctx *ctx)
{
	struct attr_hash *tbl, *tblnext;
	struct dyn_attr_template *dt, *dtnext;

	clear_attrs(ctx);

	tblnext = ctx->attr.next;
	while(tblnext) {
		tbl = tblnext;
		tblnext = tbl->next;
		free(tbl);
	}
	ctx->attr.next = NULL;

	dtnext = ctx->tmpl;
	while(dtnext) {
		dt = dtnext;
		dtnext = dt->next;
		free(dt);
	}
	ctx->tmpl = NULL;
}

/**  Initialize statically allocated attributes
 */
kdump_status
init_attrs(kdump_ctx *ctx)
{
	enum global_keyidx i;

	for (i = 0; i < NR_GLOBAL_ATTRS; ++i) {
		const struct attr_template *tmpl = &global_keys[i];
		struct attr_data *attr, *parent;

		parent = ctx->global_attrs[tmpl->parent - global_keys];
		attr = new_attr(ctx, parent, tmpl);
		if (!attr)
			return set_error(ctx, kdump_syserr,
					 "Cannot initialize attribute %s",
					 tmpl->key);
		ctx->global_attrs[i] = attr;

		if (i >= GKI_static_first && i <= GKI_static_last) {
			attr->indirect = 1;
			attr->pval = static_attr_value(ctx, i);
		}
	}

	return kdump_ok;
}

/**  Set an attribute of a dump file object.
 * @param ctx   Dump file object.
 * @param attr  Attribute (detached).
 * @param val   New value for the object.
 * @returns     Error status (see below).
 *
 * This function works both for statically allocated and dynamically
 * allocated attributes.
 */
kdump_status
set_attr(kdump_ctx *ctx, struct attr_data *attr, union kdump_attr_value val)
{
	kdump_status res;
	const struct attr_ops *ops;

	if (!attr->acthook) {
		ops = attr->template->ops;
		if (ops && ops->pre_set) {
			attr->acthook = 1;
			res = ops->pre_set(ctx, attr, &val);
			attr->acthook = 0;
			if (res != kdump_ok)
				return res;
		}
	}

	if (attr->indirect)
		*attr->pval = val;
	else
		attr->val = val;

	instantiate_path(attr->parent);
	attr->isset = 1;

	if (!attr->acthook) {
		ops = attr->template->ops;
		if (ops && ops->post_set) {
			attr->acthook = 1;
			res = ops->post_set(ctx, attr);
			attr->acthook = 0;
			return res;
		}
	}

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
	union kdump_attr_value val;

	attr = lookup_attr_raw(ctx, key);
	if (!attr)
		return set_error(ctx, kdump_nokey, "No such key");

	clear_attr(attr);
	val.number = num;
	return set_attr(ctx, attr, val);
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
	union kdump_attr_value val;

	attr = lookup_attr_raw(ctx, key);
	if (!attr)
		return set_error(ctx, kdump_nokey, "No such key");

	clear_attr(attr);
	val.address = addr;
	return set_attr(ctx, attr, val);
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
	char *dynstr;
	union kdump_attr_value val;

	attr = lookup_attr_raw(ctx, key);
	if (!attr)
		return set_error(ctx, kdump_nokey, "No such key");

	dynstr = strdup(str);
	if (!dynstr)
		return set_error(ctx, kdump_syserr,
				 "Cannot allocate string");

	clear_attr(attr);
	attr->dynstr = 1;
	val.string = dynstr;
	return set_attr(ctx, attr, val);
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
	union kdump_attr_value val;

	attr = lookup_attr_raw(ctx, key);
	if (!attr)
		return set_error(ctx, kdump_nokey, "No such key");

	clear_attr(attr);
	val.string = str;
	return set_attr(ctx, attr, val);
}

/**  Allocate a new attribute in any directory.
 * @param ctx    Dump file object.
 * @param dir    Directory key path.
 * @param tmpl   Attribute template.
 * @param pattr  Pointer to attribute data, filled out on success.
 * @returns      Error status.
 */
static kdump_status
add_attr(kdump_ctx *ctx, const char *dir, const struct attr_template *tmpl,
	 struct attr_data **pattr)
{
	struct attr_data *parent, *attr;

	parent = (struct attr_data*) lookup_attr_raw(ctx, dir);
	if (!parent)
		return set_error(ctx, kdump_nokey,
				 "No such path");
	if (parent->template->type != kdump_directory)
		return set_error(ctx, kdump_invalid,
				 "Path is a leaf attribute");

	instantiate_path(parent);
	attr = new_attr(ctx, parent, tmpl);
	if (!attr)
		return set_error(ctx, kdump_syserr,
				 "Cannot allocate attribute");

	*pattr = attr;
	return kdump_ok;
}

/**  Add a numeric attribute to a directory.
 * @param ctx   Dump file object.
 * @param path  Key name.
 * @param tmpl  Attribute template.
 * @param num   Key value (numeric).
 * @returns     Newly allocated attr_data, or @c NULL on failure.
 *
 * This is a wrapper around @c add_attr. It also generates a good enough
 * error message, so callers don't have to provide their own.
 */
kdump_status
add_attr_number(kdump_ctx *ctx, const char *path,
		const struct attr_template *tmpl, kdump_num_t num)
{
	struct attr_data *attr;
	union kdump_attr_value val;
	kdump_status res;

	res = add_attr(ctx, path, tmpl, &attr);
	if (res != kdump_ok)
		return set_error(ctx, res,
				 "Cannot set '%s.%s'", path, tmpl->key);

	val.number = num;
	return set_attr(ctx, attr, val);
}

/**  Add a string attribute to a directory.
 * @param ctx   Dump file object.
 * @param path  Key name.
 * @param tmpl  Attribute template.
 * @param str   Key value (string).
 * @returns     Newly allocated attr_data, or @c NULL on failure.
 *
 * This is a wrapper around @c add_attr. It also generates a good enough
 * error message, so callers don't have to provide their own.
 */
kdump_status
add_attr_string(kdump_ctx *ctx, const char *path,
		const struct attr_template *tmpl, const char *str)
{
	struct attr_data *attr;
	char *dynstr;
	union kdump_attr_value val;
	kdump_status res;

	dynstr = strdup(str);
	if (!dynstr)
		return set_error(ctx, kdump_syserr,
				 "Cannot allocate string");

	res = add_attr(ctx, path, tmpl, &attr);
	if (res != kdump_ok) {
		free(dynstr);
		return set_error(ctx, res,
				 "Cannot set '%s.%s'", path, tmpl->key);
	}

	attr->dynstr = 1;
	val.string = dynstr;
	return set_attr(ctx, attr, val);
}

/**  Add a static string attribute to a directory.
 * @param ctx   Dump file object.
 * @param path  Key name.
 * @param tmpl  Attribute template.
 * @param str   Key value (static string).
 * @returns     Newly allocated attr_data, or @c NULL on failure.
 *
 * This is a wrapper around @c add_attr. It also generates a good enough
 * error message, so callers don't have to provide their own.
 */
kdump_status add_attr_static_string(kdump_ctx *ctx, const char *path,
				    const struct attr_template *tmpl,
				    const char *str)
{
	struct attr_data *attr;
	union kdump_attr_value val;
	kdump_status res;

	res = add_attr(ctx, path, tmpl, &attr);
	if (res != kdump_ok)
		return set_error(ctx, res,
				 "Cannot set '%s.%s'", path, tmpl->key);

	val.string = str;
	return set_attr(ctx, attr, val);
}
