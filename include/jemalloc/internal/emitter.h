#ifndef JEMALLOC_INTERNAL_EMITTER_H
#define JEMALLOC_INTERNAL_EMITTER_H

typedef enum emitter_output_e emitter_output_t;
enum emitter_output_e {
	emitter_output_json,
	emitter_output_table
};

typedef enum emitter_justify_e emitter_justify_t;
enum emitter_justify_e {
	emitter_justify_left,
	emitter_justify_right,
	/* Not for users; just to pass to internal functions. */
	emitter_justify_none
};

typedef enum emitter_type_e emitter_type_t;
enum emitter_type_e {
	emitter_type_bool,
	emitter_type_int,
	emitter_type_unsigned,
	emitter_type_uint32,
	emitter_type_uint64,
	emitter_type_size,
	emitter_type_ssize,
	emitter_type_string,
};

typedef struct emitter_s emitter_t;
struct emitter_s {
	emitter_output_t output;
	/* Just for debugging.  Vdicts and hdicts aren't allowed to nest. */
	bool in_vdict;
	bool in_hdict;
	/* The output information. */
	void (*write_cb)(void *, const char *);
	void *cbopaque;
	union {
		/* State for tabular output. */
		struct {
			const char *vdict_name;
			bool hdict_first_key;
		};
		/* State for json output. */
		struct {
			/*
			 * ((1ULL << depth) & emitter->key_at_depth) != 0 means
			 * that we have output at least one key at the given
			 * depth (which we need to track to get the trailing
			 * commas right).  This limits the maximum nesting depth
			 * to 64, but we don't ever nest that far.
			 */
			unsigned long long key_at_depth;
			int nesting_depth;
		};
	};
};

static inline void
emitter_init(emitter_t *emitter, emitter_output_t emitter_output,
    void (*write_cb)(void *, const char *), void *cbopaque) {
	emitter->output = emitter_output;
	emitter->in_vdict = false;
	emitter->in_hdict = false;
	emitter->write_cb = write_cb;
	emitter->cbopaque = cbopaque;
	if (emitter_output == emitter_output_json) {
		emitter->key_at_depth = 0;
		emitter->nesting_depth = 0;
	} else {
		// tabular init
	}
}

/* Internal convenience function.  Write to the emitter the given string. */
JEMALLOC_FORMAT_PRINTF(2, 3)
static inline void
emitter_printf(emitter_t *emitter, const char *format, ...) {
	va_list ap;

	va_start(ap, format);
	malloc_vcprintf(emitter->write_cb, emitter->cbopaque, format, ap);
	va_end(ap);
}

/* Internal. Do a variety of sanity checks on the emitter. */
static inline void
emitter_json_assert_state(emitter_t *emitter) {
	assert(emitter->output == emitter_output_json);
	assert(emitter->nesting_depth >= 0);
	assert(emitter->nesting_depth < 64);
	unsigned long long mask = ~((1ULL << (emitter->nesting_depth + 1)) - 1);
	assert((mask & emitter->key_at_depth) == 0);
}

/*
 * Internal.  Returns true if no key has been emitted already in the most nested
 * currently-open json dict.
 */
static inline bool
emitter_json_first_key_in_dict(emitter_t *emitter) {
	emitter_json_assert_state(emitter);
	unsigned long long mask = (1ULL << emitter->nesting_depth);
	return (mask & emitter->key_at_depth) == 0;
}

/*
 * Internal.  Marks a key as having been emitted at the current nesting depth.
 */
static inline void
emitter_json_set_first_key(emitter_t *emitter) {
	emitter_json_assert_state(emitter);
	assert(emitter_json_first_key_in_dict(emitter));
	unsigned long long bit = (1ULL << emitter->nesting_depth);
	emitter->key_at_depth |= bit;
}

/*
 * Internal.  Clears the "first key written" bit at the current nesting depth.
 */
static inline void
emitter_json_clear_first_key(emitter_t *emitter) {
	emitter_json_assert_state(emitter);
	unsigned long long mask = ~(1ULL << emitter->nesting_depth);
	emitter->key_at_depth &= mask;
}

/*
 * Internal.  Terminate the current json dict (if braces is true) or array (if
 * braces is false).
 */
static inline void
emitter_json_nest_finish(emitter_t *emitter, bool braces) {
	emitter_json_assert_state(emitter);
	assert(emitter->nesting_depth > 0);
	emitter_json_clear_first_key(emitter);
	--emitter->nesting_depth;
	emitter_printf(emitter, "\n");
	for (int i = 0; i < emitter->nesting_depth; i++) {
		emitter_printf(emitter, "\t");
	}
	if (braces) {
		emitter_printf(emitter, "}");
	} else {
		emitter_printf(emitter, "]");
	}
}

/* Internal.  Finish the current dict. */
static inline void
emitter_json_dict_finish(emitter_t *emitter) {
	emitter_json_nest_finish(emitter, true);
}

/* Internal.  Finish the current arr. */
static inline void
emitter_json_arr_finish(emitter_t *emitter) {
	emitter_json_nest_finish(emitter, false);
}


/*
 * Internal.  Insert any necessary commas, newlines, and tabs needed for the
 * next key at this level.
 */
static inline void
emitter_json_key_prefix(emitter_t *emitter) {
	emitter_json_assert_state(emitter);
	if (emitter_json_first_key_in_dict(emitter)) {
		emitter_printf(emitter, "\n");
		emitter_json_set_first_key(emitter);
	} else {
		emitter_printf(emitter, ",\n");
	}
	for (int i = 0; i < emitter->nesting_depth; i++) {
		emitter_printf(emitter, "\t");
	}
}

static inline void
emitter_gen_fmt(char *out_fmt, size_t out_size, const char *fmt_specifier,
    emitter_justify_t justify, int width) {
	size_t written;
	if (justify == emitter_justify_none) {
		written = malloc_snprintf(out_fmt, out_size,
		    "%%%s", fmt_specifier);
	} else if (justify == emitter_justify_left) {
		written = malloc_snprintf(out_fmt, out_size,
		    "%%-%d%s", width, fmt_specifier);
	} else {
		written = malloc_snprintf(out_fmt, out_size,
		    "%%%d%s", width, fmt_specifier);
	}
	/* Only happens in case of bad format string, which *we* choose. */
	assert(written <  out_size);
	/* In catastrophic error, fail safe. */
	if (written == out_size) {
		out_fmt[0] = '\0';
	}
}

/*
 * Internal.  Emit the given given value type in the relevant encoding (so that
 * the bool true gets mapped to json "true", but the string "true" gets mapped
 * to json "\"true\"", for instance.
 *
 * Width is ignored if justify is emitter_justify_none.
 */
static inline void
emitter_print_value(emitter_t *emitter, emitter_justify_t justify, int width,
    emitter_type_t value_type, const void *value) {
	const bool *boolp;
	const char *const *charpp;
	const int *intp;
	const unsigned *unsignedp;
	const ssize_t *ssizep;
	const size_t *sizep;
	const uint32_t *uint32p;
	const uint64_t *uint64p;

	size_t str_written;
#define BUF_SIZE 1000
#define FMT_SIZE 10
	/*
	 * We dynamically generate a format string to emit, to let us use the
	 * snprintf machinery.  This is kinda hacky, but gets the job done
	 * quickly without having to think about the various snprintf edge
	 * cases.
	 */
	char fmt[FMT_SIZE];
	char buf[BUF_SIZE];

	switch (value_type) {
	case emitter_type_bool:
		boolp = (const bool *)value;
		emitter_gen_fmt(fmt, FMT_SIZE, "s", justify, width);
		emitter_printf(emitter, fmt, *boolp ? "true" : "false");
		break;
	case emitter_type_int:
		intp = (const int *)value;
		emitter_gen_fmt(fmt, FMT_SIZE, "d", justify, width);
		emitter_printf(emitter, fmt, *intp);
		break;
	case emitter_type_unsigned:
		unsignedp = (const unsigned *)value;
		emitter_gen_fmt(fmt, FMT_SIZE, "u", justify, width);
		emitter_printf(emitter, fmt, *unsignedp);
		break;
	case emitter_type_ssize:
		ssizep = (const ssize_t *)value;
		emitter_gen_fmt(fmt, FMT_SIZE, "zd", justify, width);
		emitter_printf(emitter, fmt, *ssizep);
		break;
	case emitter_type_size:
		sizep = (const size_t *)value;
		emitter_gen_fmt(fmt, FMT_SIZE, "zu", justify, width);
		emitter_printf(emitter, fmt, *sizep);
		break;
	case emitter_type_string:
		charpp = (const char *const *)value;

		str_written = malloc_snprintf(buf, BUF_SIZE, "\"%s\"", *charpp);
		/*
		 * We control the strings we output; we shouldn't get anything
		 * anywhere near the fmt size.
		 */
		assert(str_written < BUF_SIZE);

		/*
		 * We don't support justified quoted string primitive values for
		 * now. Fortunately, we don't want to emit them.
		 */

		emitter_gen_fmt(fmt, FMT_SIZE, "s", justify, width);
		emitter_printf(emitter, fmt, buf);
		break;
	case emitter_type_uint32:
		uint32p = (const uint32_t *)value;
		emitter_gen_fmt(fmt, FMT_SIZE, FMTu32, justify, width);
		emitter_printf(emitter, fmt, *uint32p);
		break;
	case emitter_type_uint64:
		uint64p = (const uint64_t *)value;
		emitter_gen_fmt(fmt, FMT_SIZE, FMTu64, justify, width);
		emitter_printf(emitter, fmt, *uint64p);
		break;
	default:
		unreachable();
	}
#undef BUF_SIZE
#undef FMT_SIZE
}

/* Begin actually writing to the output. */
static inline void
emitter_begin(emitter_t *emitter) {
	if (emitter->output == emitter_output_json) {
		assert(emitter->nesting_depth == 0);
		emitter_printf(emitter, "{");
		++emitter->nesting_depth;
	} else {
		// tabular init
		emitter_printf(emitter, "");
	}
}

static inline void
emitter_end(emitter_t *emitter) {
	if (emitter->output == emitter_output_json) {
		assert(emitter->nesting_depth == 1);
		emitter_json_dict_finish(emitter);
		emitter_printf(emitter, "\n");
	}
}

/*
 * Begin a dict (in json mode) or vertical table (in table mode).
 * If "dict_name" is "foo", table_header is "This is foo:", and
 * subsequently emitted keys are "bar", "baz", and "bat", then in json mode we
 * will emit:
 *   "foo": {
 *     "bar": ...,
 *     "baz": ...,
 *     "bat": ...
 *   }
 * And in table mode, we will emit:
 *   This is foo:
 *     foo.bar: ...
 *     foo.baz: ...
 *     foo.bat: ...
 * Nested vdicts aren't supported.
 */
static inline void
emitter_vdict_begin(emitter_t *emitter, const char *vdict_name,
    const char *table_header) {
	assert(!emitter->in_vdict && !emitter->in_hdict);
	emitter->in_vdict = true;

	if (emitter->output == emitter_output_json) {
		emitter_json_key_prefix(emitter);
		emitter_printf(emitter, "\"%s\": {", vdict_name);
		++emitter->nesting_depth;
	} else {
		emitter->vdict_name = vdict_name;
		emitter_printf(emitter, "%s\n", table_header);
	}
}

static inline void
emitter_vdict_end(emitter_t *emitter) {
	assert(emitter->in_vdict);
	emitter->in_vdict = false;

	if (emitter->output == emitter_output_json) {
		emitter_json_dict_finish(emitter);
	}
}

/*
 * Emits the given key and value pair.  If note_key is non-null, then in table
 * note, we emit a trailing table note in the row, so that it looks like:
 *   key: value (note_key: note_value)
 * (The note key is arbitrary; it's not namespaced, as the key is).
 */
static inline void
emitter_vdict_kv_note(emitter_t *emitter, const char *key,
    emitter_type_t value_type, const void *value, const char *note_key,
    emitter_type_t note_value_type, const void *note_value) {
	assert(emitter->in_vdict);
	if (emitter->output == emitter_output_json) {
		emitter_json_key_prefix(emitter);
		emitter_printf(emitter, "\"%s\": ", key);
		emitter_print_value(emitter, emitter_justify_none, -1,
		    value_type, value);
	} else {
		emitter_printf(emitter, "  %s.%s: ", emitter->vdict_name,
		    key);
		emitter_print_value(emitter, emitter_justify_none, -1,
		    value_type, value);
		if (note_key != NULL) {
			emitter_printf(emitter, " (%s: ", note_key);
			emitter_print_value(emitter, emitter_justify_none, -1,
			    note_value_type, note_value);
			emitter_printf(emitter, ")");
		}
		emitter_printf(emitter, "\n");
	}
}

/* Same as the _note variant, but with a NULL note. */
static inline void
emitter_vdict_kv(emitter_t *emitter, const char *key,
    emitter_type_t value_type, const void *value) {
	emitter_vdict_kv_note(emitter, key, value_type, value, NULL,
	    emitter_type_bool, NULL);
}

/*
 * An hdict is emitted the same as a vdict in json mode, but as a
 * comma-separated list in table mode (with no table name).
 */
static inline void
emitter_hdict_begin(emitter_t *emitter, const char *json_dict_name) {
	assert(!emitter->in_hdict && !emitter->in_vdict);
	emitter->in_hdict = true;
	if (emitter->output == emitter_output_json) {
		emitter_json_key_prefix(emitter);
		emitter_printf(emitter, "\"%s\": {", json_dict_name);
		++emitter->nesting_depth;
	} else {
		emitter->hdict_first_key = true;
	}
}

static inline void
emitter_table_hdict_begin(emitter_t *emitter) {
	if (emitter->output != emitter_output_table) {
		return;
	}
	emitter_hdict_begin(emitter, "");
}

static inline void
emitter_hdict_kv(emitter_t *emitter, const char *json_key,
    const char *table_key,emitter_type_t value_type, void *value) {
	assert(emitter->in_hdict);
	if (emitter->output == emitter_output_json) {
		emitter_json_key_prefix(emitter);
		emitter_printf(emitter, "\"%s\": ", json_key);
		emitter_print_value(emitter, emitter_justify_none, -1,
		    value_type, value);
	} else {
		if (emitter->hdict_first_key) {
			emitter->hdict_first_key = false;
		} else {
			emitter_printf(emitter, ", ");
		}
		emitter_printf(emitter, "%s: ", table_key);
		emitter_print_value(emitter, emitter_justify_none, -1,
		    value_type, value);
	}
}

static inline void
emitter_hdict_end(emitter_t *emitter) {
	assert(emitter->in_hdict);
	emitter->in_hdict = false;

	if (emitter->output == emitter_output_json) {
		emitter_json_dict_finish(emitter);
	} else {
		emitter_printf(emitter, "\n");
	}
}

/* End the current hdict, but only in json output mode. */
static inline void
emitter_json_hdict_end(emitter_t *emitter) {
	if (emitter->output != emitter_output_json) {
		return;
	}
	emitter_hdict_end(emitter);
}

/* End the current hdict, but only in table output mode. */
static inline void
emitter_table_hdict_end(emitter_t *emitter) {
	if (emitter->output != emitter_output_table) {
		return;
	}
	emitter_hdict_end(emitter);
}

/* Begin a dict that appears only in the json output. */
static inline void
emitter_json_dict_begin(emitter_t *emitter, const char *dict_name) {
	if (emitter->output != emitter_output_json) {
		return;
	}
	emitter_json_assert_state(emitter);
	assert(emitter->nesting_depth > 0);

	emitter_json_key_prefix(emitter);
	emitter_printf(emitter, "\"%s\": {", dict_name);
	++emitter->nesting_depth;
}

static inline void
emitter_json_dict_end(emitter_t *emitter) {
	if (emitter->output != emitter_output_json) {
		return;
	}
	emitter_json_assert_state(emitter);
	emitter_json_dict_finish(emitter);
}

/*
 * Emits
 *   "key": value
 * in json mode, and nothing in table mode.
 */
static inline void
emitter_json_simple_kv(emitter_t *emitter, const char *key,
    emitter_type_t value_type, const void *value) {
	if (emitter->output == emitter_output_json) {
		emitter_json_key_prefix(emitter);
		emitter_printf(emitter, "\"%s\": ", key);
		emitter_print_value(emitter, emitter_justify_none, -1,
		    value_type, value);
	}
}

/*
 * Emits
 *   "json_key": value
 * in json mode, and
 *   Table_key: value
 * in table mode.
 */
static inline void
emitter_simple_kv(emitter_t *emitter, const char *json_key,
    const char *table_key, emitter_type_t value_type, const void *value) {
	if (emitter->output == emitter_output_json) {
		emitter_json_simple_kv(emitter, json_key, value_type, value);
	} else {
		emitter_printf(emitter, "%s: ", table_key);
		emitter_print_value(emitter, emitter_justify_none, -1,
		    value_type, value);
		emitter_printf(emitter, "\n");
	}
}

/* Print a row that appears only in tabular mode. */
static inline void
emitter_table_note(emitter_t *emitter, const char *format, ...) {
	va_list ap;

	va_start(ap, format);
	if (emitter->output == emitter_output_table) {
		malloc_vcprintf(emitter->write_cb, emitter->cbopaque, format, ap);
		emitter_printf(emitter, "\n");
	}
	va_end(ap);
}

/* Begin a json-only array.  It looks like:
 *   "key": [
 *     {
 *       "v1": 1
 *     },
 *     {
 *       "v2": 2
 *     }
 *   ]
 *
 * For now, we only support dicts as array elements.
 */
static inline void
emitter_json_arr_begin(emitter_t *emitter, const char *key) {
	if (emitter->output != emitter_output_json) {
		return;
	}
	emitter_json_assert_state(emitter);
	assert(emitter->nesting_depth > 0);

	emitter_json_key_prefix(emitter);
	emitter_printf(emitter, "\"%s\": [", key);
	++emitter->nesting_depth;
}

static inline void
emitter_json_arr_end(emitter_t *emitter) {
	if (emitter->output != emitter_output_json) {
		return;
	}
	emitter_json_assert_state(emitter);
	emitter_json_arr_finish(emitter);
}

static inline void
emitter_json_arr_dict_begin(emitter_t *emitter) {
	if (emitter->output != emitter_output_json) {
		return;
	}
	emitter_json_assert_state(emitter);
	assert(emitter->nesting_depth > 0);

	emitter_json_key_prefix(emitter);
	emitter_printf(emitter, "{");
	++emitter->nesting_depth;
}

static inline void
emitter_json_arr_dict_end(emitter_t *emitter) {
	if (emitter->output != emitter_output_json) {
		return;
	}
	emitter_json_assert_state(emitter);
	emitter_json_dict_finish(emitter);
}

typedef enum emitter_shape_type_e emitter_shape_type_t;
enum emitter_shape_type_e {
	emitter_shape_type_root,
	emitter_shape_type_dict,
	emitter_shape_type_primitive,
	emitter_shape_type_padding
};

typedef struct emitter_shape_s emitter_shape_t;
typedef ql_head(emitter_shape_t) emitter_shape_list_t;
struct emitter_shape_s {
	emitter_shape_type_t type;
	/* The enclosing dict (or null, for a root). */
	emitter_shape_t *parent;
	/* Used for elements of a dictionary. */
	ql_elm(emitter_shape_t) link;

	/*
	 * In table output mode, we justify everything, giving each element a
	 * maximum number of horizontal characters it's allowed to occupy.
	 * (Though, the dict name is included ony for the root).
	 */
	emitter_justify_t justify;
	int width;

	/* Not used for the root. */
	const char *json_key;
	const char *table_key;

	union {
		/* Used only for primitive types. */
		struct {
			emitter_type_t value_type;
			union {
				bool bool_value;
				int int_value;
				unsigned unsigned_value;
				uint32_t uint32_value;
				uint64_t uint64_value;
				size_t size_value;
				ssize_t ssize_value;
				const char *string_value;
			};
		};
		/* Used only for dict or root types. */
		struct {
			emitter_shape_list_t children;
		};
		/* No data needed for padding; just width. */
	};
};

static inline void
emitter_shape_init_root(emitter_shape_t *shape, const char *json_key,
    const char *table_key, emitter_justify_t justify, int width) {
	shape->type = emitter_shape_type_root;
	shape->parent = NULL;
	shape->justify = justify;
	shape->width = width;
	shape->json_key = json_key;
	shape->table_key = table_key;
	ql_new(&shape->children);
}

static inline void
emitter_shape_init_dict(emitter_shape_t *shape, emitter_shape_t *parent,
    const char *json_key, const char *table_key, emitter_justify_t justify,
    int width) {
	shape->type = emitter_shape_type_dict;
	shape->parent = parent;

	assert(shape->parent->type == emitter_shape_type_root
	    || shape->parent->type == emitter_shape_type_dict);

	shape->justify = justify;
	shape->width = width;
	ql_new(&shape->children);
	ql_elm_new(shape, link);
	ql_tail_insert(&parent->children, shape, link);
	shape->json_key = json_key;
	shape->table_key = table_key;
}

/*
 * Note that this doesn't actually fill the value; it's up to the user to fill
 * it in directly, before outputting an array element.
 */
static inline void
emitter_shape_init_primitive(emitter_shape_t *shape, emitter_shape_t *parent,
    const char *json_key, const char *table_key, emitter_justify_t justify,
    int width, emitter_type_t value_type) {
	shape->type = emitter_shape_type_primitive;
	shape->parent = parent;

	assert(shape->parent->type == emitter_shape_type_root
	    || shape->parent->type == emitter_shape_type_dict);

	ql_elm_new(shape, link);
	ql_tail_insert(&parent->children, shape, link);

	shape->json_key = json_key;
	shape->table_key = table_key;
	shape->justify = justify;
	shape->width = width;
	shape->value_type = value_type;
}

static inline void
emitter_shape_init_padding(emitter_shape_t *shape, emitter_shape_t *parent,
    int width) {
	shape->type = emitter_shape_type_padding;
	shape->parent = parent;

	assert(shape->parent->type == emitter_shape_type_root
	    || shape->parent->type == emitter_shape_type_dict);

	ql_elm_new(shape, link);
	ql_tail_insert(&parent->children, shape, link);

	shape->width = width;
}

/* Internal.  Recursively visit the shape. */
static inline void
emitter_table_print_shape(emitter_t *emitter, emitter_shape_t *shape,
    bool header) {
	assert(emitter->output == emitter_output_table);

	char fmt[100];
	char buf[100];

	/* For iteration in the switch. */
	emitter_shape_t *child;
	switch (shape->type) {
	case emitter_shape_type_root:
		emitter_gen_fmt(fmt, 100, "s", shape->justify, shape->width);
		emitter_printf(emitter, fmt, header ? shape->table_key : "");

		ql_foreach(child, &shape->children, link) {
			emitter_table_print_shape(emitter, child, header);
		}
		emitter_printf(emitter, "\n");
		break;
	case emitter_shape_type_dict:
		if (strlen(shape->table_key) != 0) {
			malloc_snprintf(buf, 100, "%s:", shape->table_key);
		} else {
			malloc_snprintf(buf, 100, "", shape->table_key);
		}
		emitter_gen_fmt(fmt, 100, "s", shape->justify, shape->width);
		emitter_printf(emitter, fmt, buf);

		ql_foreach(child, &shape->children, link) {
			emitter_table_print_shape(emitter, child, header);
		}
		break;
	case emitter_shape_type_primitive:
		if (header) {
			emitter_gen_fmt(fmt, 100, "s", shape->justify,
			    shape->width);
			emitter_printf(emitter, fmt, shape->table_key);
		} else {
			emitter_print_value(emitter, shape->justify,
			    shape->width, shape->value_type,
			    (void *)&shape->bool_value);
		}
		break;
	case emitter_shape_type_padding:
		emitter_gen_fmt(fmt, 100, "s", emitter_justify_left,
		    shape->width);
		emitter_printf(emitter, fmt, "");
		break;
	default:
		not_reached();
	}
}

/* Internal.  Same as above, but for json. */
static inline void
emitter_json_print_shape(emitter_t *emitter, emitter_shape_t *shape) {
	assert(emitter->output == emitter_output_json);

	emitter_shape_t *child;
	switch (shape->type) {
	case emitter_shape_type_root:
		emitter_json_arr_dict_begin(emitter);
		ql_foreach(child, &shape->children, link) {
			emitter_json_print_shape(emitter, child);
		}
		emitter_json_arr_dict_end(emitter);
		break;
	case emitter_shape_type_dict:
		emitter_json_dict_begin(emitter, shape->json_key);
		ql_foreach(child, &shape->children, link) {
			emitter_json_print_shape(emitter, child);
		}
		emitter_json_dict_end(emitter);
		break;
	case emitter_shape_type_primitive:
		emitter_json_simple_kv(emitter, shape->json_key,
		    shape->value_type, &shape->bool_value);
		break;
	case emitter_shape_type_padding:
		break;
	default:
		not_reached();
	}
}

/*
 * A table array appears as fairly boring nested json object when emitted
 * in json mode, but as an aligned, multi-row table with a header in table mode.
 * See the test cases for API usage.
 */
static inline void
emitter_tabarr_begin(emitter_t *emitter, emitter_shape_t *shape) {
	assert(shape->type == emitter_shape_type_root);
	assert(shape->parent == NULL);

	if (emitter->output == emitter_output_json) {
		emitter_json_arr_begin(emitter, shape->json_key);
	} else {
		emitter_table_print_shape(emitter, shape, true);
	}
}

static inline void
emitter_tabarr_end(emitter_t *emitter) {
	if (emitter->output == emitter_output_json) {
		emitter_json_arr_end(emitter);
	} else {
		emitter_printf(emitter, "\n");
	}
}

static inline void
emitter_tabarr_obj(emitter_t *emitter, emitter_shape_t *shape) {
	assert(shape->type == emitter_shape_type_root);

	if (emitter->output == emitter_output_json) {
		emitter_json_print_shape(emitter, shape);
	} else {
		emitter_table_print_shape(emitter, shape, false);
	}
}

static inline void
emitter_tabdict_begin(emitter_t *emitter, emitter_shape_t *shape) {
	assert(shape->type == emitter_shape_type_root);
	assert(shape->parent == NULL);
	/*
	 * If this represents a dict, then there should be a single child (which
	 * can be iterated.
	 */
	assert(ql_first(&shape->children) == ql_last(&shape->children, link));

	if (emitter->output == emitter_output_json) {
		emitter_json_dict_begin(emitter, shape->json_key);
	} else {
		emitter_table_print_shape(emitter, shape, true);
	}
}

static inline void
emitter_tabdict_end(emitter_t *emitter) {
	if (emitter->output == emitter_output_json) {
		emitter_json_dict_end(emitter);
	} else {
		emitter_printf(emitter, "\n");
	}
}

static inline void
emitter_tabdict_kv(emitter_t *emitter, emitter_shape_t *shape) {
	assert(shape->type == emitter_shape_type_root);

	if (emitter->output == emitter_output_json) {
		emitter_json_print_shape(emitter, ql_first(&shape->children));
	} else {
		emitter_table_print_shape(emitter, shape, false);
	}
}

#endif /* JEMALLOC_INTERNAL_EMITTER_H */
