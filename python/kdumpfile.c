#include <Python.h>
#include <kdumpfile.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
	PyObject_HEAD
	kdump_ctx *ctx;
	PyObject *file;
	PyObject *cb_get_symbol;
	PyObject *attr;
} kdumpfile_object;

static PyObject *SysErrException;
static PyObject *UnsupportedException;
static PyObject *NoDataException;
static PyObject *DataErrException;
static PyObject *InvalidException;
static PyObject *NoKeyException;
static PyObject *EOFException;

static PyTypeObject attr_dir_object_type;
static PyTypeObject attr_iter_object_type;

static PyObject *attr_dir_new(kdumpfile_object *kdumpfile,
			      const kdump_attr_ref_t *baseref);

static PyObject *
exception_map(kdump_status status)
{
	switch (status) {
	case kdump_syserr:      return SysErrException;
	case kdump_unsupported: return UnsupportedException;
	case kdump_nodata:      return NoDataException;
	case kdump_dataerr:     return DataErrException;
	case kdump_invalid:     return InvalidException;
	case kdump_nokey:       return NoKeyException;
	case kdump_eof:         return EOFException;
	/* If we raise an exception with status == kdump_ok, it's a bug. */
	case kdump_ok:
	default:                return PyExc_RuntimeError;
	};
}

static kdump_status cb_get_symbol(kdump_ctx *ctx, const char *name, kdump_addr_t *addr)
{
	kdumpfile_object *self;
	PyObject *ret;

	self = (kdumpfile_object*)kdump_get_priv(ctx);

	if (! self->cb_get_symbol) {
		PyErr_SetString(PyExc_RuntimeError, "Callback symbol-resolving function not set");
		printf ("aa\n");
		return kdump_nodata;
	}

	ret = PyObject_CallFunction(self->cb_get_symbol, "s", name);

	if (! PyLong_Check(ret)) {
		PyErr_SetString(PyExc_RuntimeError, "Callback of symbol-resolving function returned no long");
		printf ("bb\n");
		return kdump_nodata;
	}

	*addr = PyLong_AsUnsignedLongLong(ret);;

	Py_XDECREF(ret);

	return kdump_ok;
}

static PyObject *
kdumpfile_new (PyTypeObject *type, PyObject *args, PyObject *kw)
{
	kdumpfile_object *self = NULL;
	static char *keywords[] = {"file", NULL};
	kdump_attr_ref_t rootref;
	kdump_status status;
	PyObject *fo = NULL;
	int fd;

	if (!PyArg_ParseTupleAndKeywords (args, kw, "O!", keywords,
				    &PyFile_Type, &fo))
		    return NULL;

	self = (kdumpfile_object*) type->tp_alloc (type, 0);
	if (!self)
		return NULL;

	self->ctx = kdump_alloc_ctx();
	if (!self->ctx) {
		PyErr_SetString(PyExc_MemoryError,
				"Couldn't allocate kdump context");
		goto fail;
	}

	status = kdump_init_ctx(self->ctx);
	if (status != kdump_ok) {
		PyErr_Format(SysErrException,
			     "Couldn't initialize kdump context: %s",
			     kdump_err_str(self->ctx));
		goto fail;
	}

	fd = fileno(PyFile_AsFile(fo));
	status = kdump_set_fd(self->ctx, fd);
	if (status != kdump_ok) {
		PyErr_Format(exception_map(status),
			     "Cannot open dump: %s", kdump_err_str(self->ctx));
		goto fail;
	}

	self->file = fo;
	Py_INCREF(fo);

	self->cb_get_symbol = NULL;
	kdump_cb_get_symbol_val(self->ctx, cb_get_symbol);
	kdump_set_priv(self->ctx, self);

	status = kdump_attr_ref(self->ctx, NULL, &rootref);
	if (status != kdump_ok) {
		PyErr_Format(exception_map(status),
			     "Cannot reference root attribute: %s",
			     kdump_err_str(self->ctx));
		goto fail;
	}

	self->attr = attr_dir_new(self, &rootref);
	if (!self->attr) {
		kdump_attr_unref(self->ctx, &rootref);
		goto fail;
	}

	return (PyObject*)self;

fail:
	Py_XDECREF(self->attr);
	Py_XDECREF(self->file);
	Py_XDECREF(self);
	return NULL;
}

static void
kdumpfile_dealloc(PyObject *_self)
{
	kdumpfile_object *self = (kdumpfile_object*)_self;

	if (self->ctx) {
		kdump_free(self->ctx);
		self->ctx = NULL;
	}

	if (self->file) Py_XDECREF(self->file);
	self->ob_type->tp_free((PyObject*)self);
}

static PyObject *kdumpfile_read (PyObject *_self, PyObject *args, PyObject *kw)
{
	kdumpfile_object *self = (kdumpfile_object*)_self;
	PyObject *obj;
	kdump_paddr_t addr;
	kdump_status status;
	int addrspace;
	unsigned long size;
	static char *keywords[] = {"addrspace", "address", "size", NULL};
	size_t r;

	if (!PyArg_ParseTupleAndKeywords(args, kw, "ikk:",
					 keywords, &addrspace, &addr, &size))
		return NULL;

	if (!size) {
		PyErr_SetString(PyExc_ValueError, "Zero size buffer");
		return NULL;
	}

	obj = PyByteArray_FromStringAndSize(0, size);
	if (!obj)
		return NULL;

	r = size;
	status = kdump_readp(self->ctx, addrspace, addr,
			     PyByteArray_AS_STRING(obj), &r);
	if (status != kdump_ok) {
		Py_XDECREF(obj);
		PyErr_Format(exception_map(status), kdump_err_str(self->ctx));
		return NULL;
	}

	return obj;
}

static PyObject *
attr_new(kdumpfile_object *kdumpfile, kdump_attr_ref_t *ref, kdump_attr_t *attr)
{
	if (attr->type != kdump_directory)
		kdump_attr_unref(kdumpfile->ctx, ref);

	switch (attr->type) {
		case kdump_number:
			return PyLong_FromUnsignedLong(attr->val.number);
		case kdump_address:
			return PyLong_FromUnsignedLong(attr->val.address);
		case kdump_string:
			return PyString_FromString(attr->val.string);
		case kdump_directory:
			return attr_dir_new(kdumpfile, ref);
		default:
			PyErr_SetString(PyExc_RuntimeError, "Unhandled attr type");
			return NULL;
	}
}

static PyObject *kdumpfile_vtop_init(PyObject *_self, PyObject *args)
{
	kdumpfile_object *self = (kdumpfile_object*)_self;

	kdump_vtop_init(self->ctx);

	Py_RETURN_NONE;
}

static PyMethodDef kdumpfile_object_methods[] = {
	{"read",      (PyCFunction) kdumpfile_read, METH_VARARGS | METH_KEYWORDS,
		"read (addrtype, address) -> buffer.\n" },
	{"vtop_init", (PyCFunction) kdumpfile_vtop_init, METH_NOARGS,
		"Initialize virtual memory mapping\n"},
	{NULL}  
};

static PyObject *
kdumpfile_getattr(PyObject *_self, void *_data)
{
	kdumpfile_object *self = (kdumpfile_object*)_self;

	if (!self->attr)
		Py_RETURN_NONE;

	Py_INCREF(self->attr);
	return self->attr;
}

static PyObject *kdumpfile_get_symbol_func (PyObject *_self, void *_data)
{
	kdumpfile_object *self = (kdumpfile_object*)_self;

	if (! self->cb_get_symbol)
		Py_RETURN_NONE;
	Py_INCREF(self->cb_get_symbol);

	return self->cb_get_symbol;
}

int kdumpfile_set_symbol_func(PyObject *_self, PyObject *_set, void *_data)
{
	kdumpfile_object *self = (kdumpfile_object*)_self;

	if (! PyCallable_Check(_set)) {
		PyErr_SetString(PyExc_RuntimeError, "Argument must be callable");
		return 1;
	}

	if (self->cb_get_symbol)
		Py_XDECREF(self->cb_get_symbol);
	self->cb_get_symbol = _set;
	Py_INCREF(self->cb_get_symbol);
	return 0;
}

static void
cleanup_exceptions(void)
{
	Py_XDECREF(SysErrException);
	Py_XDECREF(UnsupportedException);
	Py_XDECREF(NoDataException);
	Py_XDECREF(DataErrException);
	Py_XDECREF(InvalidException);
	Py_XDECREF(NoKeyException);
	Py_XDECREF(EOFException);
}

static int lookup_exceptions (void)
{
	PyObject *mod = PyImport_ImportModule("kdumpfile.exceptions");
	if (!mod)
		return -1;

#define lookup_exception(name)				\
do {							\
	name = PyObject_GetAttrString(mod, #name);	\
	if (!name)					\
		goto fail;				\
} while(0)

	lookup_exception(SysErrException);
	lookup_exception(UnsupportedException);
	lookup_exception(NoDataException);
	lookup_exception(DataErrException);
	lookup_exception(InvalidException);
	lookup_exception(NoKeyException);
	lookup_exception(EOFException);
#undef lookup_exception

	Py_XDECREF(mod);
	return 0;
fail:
	cleanup_exceptions();
	Py_XDECREF(mod);
	return -1;
}


static PyGetSetDef kdumpfile_object_getset[] = {
	{ "attr", kdumpfile_getattr, NULL,
	  "Access to libkdumpfile attributes",
	  NULL },
	{ "symbol_func", kdumpfile_get_symbol_func, kdumpfile_set_symbol_func,
	  "Callback function called by libkdumpfile for symbol resolving",
	  NULL },
	{ NULL }
};

static PyTypeObject kdumpfile_object_type = 
{
	PyVarObject_HEAD_INIT(NULL, 0)
	"_kdumpfile.kdumpfile",         /* tp_name*/ 
	sizeof (kdumpfile_object),      /* tp_basicsize*/ 
	0,                              /* tp_itemsize*/ 
	kdumpfile_dealloc,              /* tp_dealloc*/ 
	0,                              /* tp_print*/ 
	0,                              /* tp_getattr*/ 
	0,                              /* tp_setattr*/ 
	0,                              /* tp_compare*/ 
	0,                              /* tp_repr*/ 
	0,                              /* tp_as_number*/ 
	0,                              /* tp_as_sequence*/
	0,                              /* tp_as_mapping*/ 
	0,                              /* tp_hash */ 
	0,                              /* tp_call*/ 
	0,                              /* tp_str*/ 
	0,                              /* tp_getattro*/ 
	0,                              /* tp_setattro*/ 
	0,                              /* tp_as_buffer*/ 
	Py_TPFLAGS_DEFAULT,             /* tp_flags*/ 
	"kdumpfile",                    /* tp_doc */ 
	0,                              /* tp_traverse */ 
	0,                              /* tp_clear */ 
	0,                              /* tp_richcompare */ 
	0,                              /* tp_weaklistoffset */ 
	0,                              /* tp_iter */ 
	0,                              /* tp_iternext */ 
	kdumpfile_object_methods,       /* tp_methods */ 
	0,                              /* tp_members */ 
	kdumpfile_object_getset,        /* tp_getset */ 
	0,                              /* tp_base */
	0,                              /* tp_dict */
	0,                              /* tp_descr_get */
	0,                              /* tp_descr_set */
	0, 				  /* tp_dictoffset */
	0,                              /* tp_init */
	0,                              /* tp_alloc */
	kdumpfile_new,                  /* tp_new */
};

/* Attribute dictionary type */

typedef struct {
	PyObject_HEAD
	kdumpfile_object *kdumpfile;
	kdump_attr_ref_t baseref;
} attr_dir_object;

static PyObject *attr_iter_new(attr_dir_object *attr_dir);

static int
lookup_attribute(attr_dir_object *self, PyObject *key, kdump_attr_ref_t *ref)
{
	PyObject *stringkey;
	char *keystr;
	int ret;

	if (!PyString_Check(key)) {
		stringkey = PyObject_Str(key);
		if (!stringkey)
			return -1;
	} else
		stringkey = key;

	ret = -1;

	keystr = PyString_AsString(stringkey);
	if (keystr) {
		kdump_ctx *ctx = self->kdumpfile->ctx;
		kdump_status status;

		status = kdump_sub_attr_ref(ctx, &self->baseref, keystr, ref);
		if (status == kdump_ok)
			ret = 0;
		else if (status == kdump_nokey)
			PyErr_SetObject(PyExc_KeyError, key);
		else
			PyErr_SetString(exception_map(status),
					kdump_err_str(ctx));
	}

	if (stringkey != key)
		Py_DECREF(stringkey);

	return ret;
}

static PyObject *
attr_dir_subscript(PyObject *_self, PyObject *key)
{
	attr_dir_object *self = (attr_dir_object*)_self;
	kdump_ctx *ctx;
	kdump_attr_t attr;
	kdump_attr_ref_t ref;
	kdump_status status;

	if (lookup_attribute(self, key, &ref))
		return NULL;

	ctx = self->kdumpfile->ctx;
	status = kdump_attr_ref_get(ctx, &ref, &attr);
	if (status == kdump_ok)
		return attr_new(self->kdumpfile, &ref, &attr);

	if (status == kdump_nodata)
		PyErr_SetObject(PyExc_KeyError, key);
	else
		PyErr_SetString(exception_map(status), kdump_err_str(ctx));

	kdump_attr_unref(ctx, &ref);
	return NULL;
}

static int
attr_dir_ass_subscript(PyObject *_self, PyObject *key, PyObject *value)
{
	attr_dir_object *self = (attr_dir_object*)_self;
	unsigned PY_LONG_LONG num;
	PyObject *conv;
	kdump_ctx *ctx;
	kdump_attr_t attr;
	kdump_attr_ref_t ref;
	kdump_status status;

	if (lookup_attribute(self, key, &ref))
		return -1;

	attr.type = value
		? kdump_attr_ref_type(&ref)
		: kdump_nil;
	ctx = self->kdumpfile->ctx;

	conv = value;
	switch (attr.type) {
	case kdump_nil:		/* used for deletions */
		break;

	case kdump_number:
	case kdump_address:
		if (PyLong_Check(value)) {
			num = PyLong_AsUnsignedLongLong(value);
			if (PyErr_Occurred())
				goto fail;
		} else if (PyInt_Check(value)) {
			num = PyInt_AsLong(value);
			if (PyErr_Occurred())
				goto fail;
		} else {
			PyErr_Format(PyExc_TypeError,
				     "need an integer, not %.200s",
				     Py_TYPE(value)->tp_name);
			goto fail;
		}

		if (attr.type == kdump_number)
			attr.val.number = num;
		else
			attr.val.address = num;
		break;

	case kdump_string:
		if (!PyString_Check(value)) {
			conv = PyObject_Str(value);
			if (!conv)
				goto fail;
		}
		if (! (attr.val.string = PyString_AsString(conv)) )
			goto fail;
		break;

	default:
		PyErr_Format(PyExc_TypeError, "Cannot assign to %s",
			     (attr.type == kdump_directory
			      ? "a directory"
			      : "an unknown type"));
		goto fail;
	}

	status = kdump_attr_ref_set(ctx, &ref, &attr);
	kdump_attr_unref(ctx, &ref);
	if (conv != value)
		Py_DECREF(conv);

	if (status != kdump_ok) {
		PyErr_SetString(exception_map(status), kdump_err_str(ctx));
		return -1;
	}

	return 0;

 fail:
	kdump_attr_unref(ctx, &ref);
	if (conv != value)
		Py_DECREF(conv);
	return -1;
}

static PyMappingMethods attr_dir_as_mapping = {
	NULL,			/* mp_length */
	attr_dir_subscript,	/* mp_subscript */
	attr_dir_ass_subscript,	/* mp_ass_subscript */
};

static PyObject *
attr_dir_new(kdumpfile_object *kdumpfile, const kdump_attr_ref_t *baseref)
{
	attr_dir_object *self;

	self = PyObject_GC_New(attr_dir_object, &attr_dir_object_type);
	if (self == NULL)
		return NULL;

	Py_INCREF((PyObject*)kdumpfile);
	self->kdumpfile = kdumpfile;
	self->baseref = *baseref;
	PyObject_GC_Track(self);
	return (PyObject*)self;
}

static void
attr_dir_dealloc(PyObject *_self)
{
	attr_dir_object *self = (attr_dir_object*)_self;

	PyObject_GC_UnTrack(self);
	kdump_attr_unref(self->kdumpfile->ctx, &self->baseref);
	Py_XDECREF((PyObject*)self->kdumpfile);
	Py_TYPE(self)->tp_free((PyObject*)self);
}

static int
attr_dir_traverse(PyObject *_self, visitproc visit, void *arg)
{
	attr_dir_object *self = (attr_dir_object*)_self;

	Py_VISIT((PyObject*)self->kdumpfile);
	return 0;
}

static PyObject *
attr_dir_getattro(PyObject *_self, PyObject *name)
{
	PyObject *ret;

	ret = PyObject_GenericGetAttr(_self, name);
	if (ret || !PyErr_ExceptionMatches(PyExc_AttributeError))
		return ret;

	PyErr_Clear();
	ret = attr_dir_subscript(_self, name);
	if (ret || !PyErr_ExceptionMatches(PyExc_KeyError))
		return ret;

	PyErr_Format(PyExc_AttributeError,
		     "'%.50s' object has no attribute '%.400s'",
		     Py_TYPE(_self)->tp_name, PyString_AS_STRING(name));
	return NULL;
}

static int
attr_dir_setattro(PyObject *_self, PyObject *name, PyObject *value)
{
	int ret;

	ret = PyObject_GenericSetAttr(_self, name, value);
	if (!ret || !PyErr_ExceptionMatches(PyExc_AttributeError))
		return ret;

	PyErr_Clear();
	ret = attr_dir_ass_subscript(_self, name, value);
	if (!ret || !PyErr_ExceptionMatches(PyExc_KeyError))
		return ret;

	PyErr_Format(PyExc_AttributeError,
		     "'%.50s' object has no attribute '%.400s'",
		     Py_TYPE(_self)->tp_name, PyString_AS_STRING(name));
	return -1;
}

static PyTypeObject attr_dir_object_type =
{
	PyVarObject_HEAD_INIT (NULL, 0)
	"_kdumpfile.attribute-directory",
	sizeof(attr_dir_object),	/* tp_basicsize*/
	sizeof(char),			/* tp_itemsize*/
	/* methods */
	attr_dir_dealloc,		/* tp_dealloc*/
	0,				/* tp_print*/
	0,				/* tp_getattr*/
	0,				/* tp_setattr*/
	0,				/* tp_compare*/
	0,				/* tp_repr*/
	0,				/* tp_as_number*/
	0,				/* tp_as_sequence*/
	&attr_dir_as_mapping,		/* tp_as_mapping*/
	0,				/* tp_hash */
	0,				/* tp_call*/
	0,				/* tp_str*/
	attr_dir_getattro,		/* tp_getattro*/
	attr_dir_setattro,		/* tp_setattro*/
	0,				/* tp_as_buffer*/
	Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC, /* tp_flags*/
	0,				/* tp_doc */
	attr_dir_traverse,		/* tp_traverse */
	0,				/* tp_clear */
	0,				/* tp_richcompare */
	0,				/* tp_weaklistoffset */
	(getiterfunc)attr_iter_new,	/* tp_iter */
	0,				/* tp_iternext */
};

/* Attribute iterator type */

typedef struct {
	PyObject_HEAD
	kdumpfile_object *kdumpfile;
	kdump_attr_iter_t iter;
} attr_iter_object;

static PyObject *
attr_iter_new(attr_dir_object *attr_dir)
{
	attr_iter_object *self;
	kdump_ctx *ctx = attr_dir->kdumpfile->ctx;
	kdump_status status;

	self = PyObject_GC_New(attr_iter_object, &attr_iter_object_type);
	if (self == NULL)
		return NULL;

	status = kdump_attr_ref_iter_start(ctx, &attr_dir->baseref,
					   &self->iter);
	if (status != kdump_ok) {
		PyErr_Format(exception_map(status), kdump_err_str(ctx));
		Py_DECREF(self);
		return NULL;
	}

	Py_INCREF((PyObject*)attr_dir->kdumpfile);
	self->kdumpfile = attr_dir->kdumpfile;
	PyObject_GC_Track(self);
	return (PyObject*)self;
}

static void
attr_iter_dealloc(PyObject *_self)
{
	attr_iter_object *self = (attr_iter_object*)_self;
	kdump_ctx *ctx = self->kdumpfile->ctx;

	kdump_attr_iter_end(ctx, &self->iter);
	PyObject_GC_UnTrack(self);
	Py_XDECREF((PyObject*)self->kdumpfile);
	Py_TYPE(self)->tp_free((PyObject*)self);
}

static int
attr_iter_traverse(PyObject *_self, visitproc visit, void *arg)
{
	attr_iter_object *self = (attr_iter_object*)_self;

	Py_VISIT((PyObject*)self->kdumpfile);
	return 0;
}

static PyObject *
attr_iter_next(PyObject *_self)
{
	attr_iter_object *self = (attr_iter_object*)_self;
	PyObject *ret;
	kdump_ctx *ctx;
	kdump_status status;

	if (!self->iter.key)
		return NULL;

	ret = PyString_FromString(self->iter.key);

	ctx = self->kdumpfile->ctx;
	status = kdump_attr_iter_next(ctx, &self->iter);
	if (status != kdump_ok) {
		PyErr_Format(exception_map(status), kdump_err_str(ctx));
		Py_XDECREF(ret);
		ret = NULL;
	}

	return ret;
}

static PyTypeObject attr_iter_object_type = {
	PyVarObject_HEAD_INIT(NULL, 0)
	"_libkdumpfile.attribute-iterator",
	sizeof(attr_iter_object),	/* tp_basicsize */
	0,				/* tp_itemsize */
	/* methods */
	attr_iter_dealloc,		/* tp_dealloc */
	0,				/* tp_print */
	0,				/* tp_getattr */
	0,				/* tp_setattr */
	0,				/* tp_compare */
	0,				/* tp_repr */
	0,				/* tp_as_number */
	0,				/* tp_as_sequence */
	0,				/* tp_as_mapping */
	0,				/* tp_hash */
	0,				/* tp_call */
	0,				/* tp_str */
	0,				/* tp_getattro */
	0,				/* tp_setattro */
	0,				/* tp_as_buffer */
	Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC, /* tp_flags */
	0,				/* tp_doc */
	attr_iter_traverse,		/* tp_traverse */
	0,				/* tp_clear */
	0,				/* tp_richcompare */
	0,				/* tp_weaklistoffset */
	PyObject_SelfIter,		/* tp_iter */
	attr_iter_next,			/* tp_iternext */
};

struct constdef {
	const char *name;
	int value;
};

static const struct constdef kdumpfile_constants[] = {
	{ "KDUMP_KPHYSADDR", KDUMP_KPHYSADDR },
        { "KDUMP_MACHPHYSADDR", KDUMP_MACHPHYSADDR },
	{ "KDUMP_KVADDR", KDUMP_KVADDR },
	{ "KDUMP_XENVADDR", KDUMP_XENVADDR },
	{ NULL, 0 }
};

PyMODINIT_FUNC
init_kdumpfile (void)
{
	PyObject *mod;
	const struct constdef *cdef;
	int ret;

	if (PyType_Ready(&kdumpfile_object_type) < 0)
		return;
	if (PyType_Ready(&attr_dir_object_type) < 0)
		return;
	if (PyType_Ready(&attr_iter_object_type) < 0)
		return;

	ret = lookup_exceptions();
	if (ret)
		return;

	mod = Py_InitModule3("_kdumpfile", NULL,
			"kdumpfile - interface to libkdumpfile");
	if (!mod)
		goto fail;

	Py_INCREF((PyObject *)&kdumpfile_object_type);
	ret = PyModule_AddObject(mod, "kdumpfile",
				 (PyObject*)&kdumpfile_object_type);
	if (ret)
		goto fail;

	for (cdef = kdumpfile_constants; cdef->name; ++cdef)
		if (PyModule_AddIntConstant(mod, cdef->name, cdef->value))
			goto fail;

	return;
fail:
	cleanup_exceptions();
	Py_XDECREF(mod);
}
