#ifndef JEMALLOC_INTERNAL_EMITTER_H
#define JEMALLOC_INTERNAL_EMITTER_H

typedef enum emitter_output_e emitter_output_t;
enum emitter_output_e {
	emitter_output_json,
	emitter_output_table
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
	emitter_type_string
};

typedef struct emitter_s emitter_t;
struct emitter_s {
	emitter_output_t output;
	/* Just for debugging.  Vdicts aren't allowed to nest. */
	bool in_vdict;
	void (*write_cb)(void *, const char *);
	void *cbopaque;
	union {
		/* State for tabular output. */
		struct {
			const char *vdict_name;
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

/*
 * Internal.  Emit the given given value type in the relevant encoding (so that
 * the bool true gets mapped to json "true", but the string "true" gets mapped
 * to json "\"true\"", for instance.
 */
static inline void
emitter_print_value(emitter_t *emitter, emitter_type_t value_type,
    const void *value) {
	const bool *boolp;
	const char *const *charpp;
	const int *intp;
	const unsigned *unsignedp;
	const ssize_t *ssizep;
	const size_t *sizep;
	const uint32_t *uint32p;
	const uint64_t *uint64p;

	switch (value_type) {
	case emitter_type_bool:
		boolp = (const bool *)value;
		emitter_printf(emitter, "%s", *boolp ? "true" : "false");
		break;
	case emitter_type_int:
		intp = (const int *)value;
		emitter_printf(emitter, "%d", *intp);
		break;
	case emitter_type_unsigned:
		unsignedp = (const unsigned *)value;
		emitter_printf(emitter, "%u", *unsignedp);
		break;
	case emitter_type_ssize:
		ssizep = (const ssize_t *)value;
		emitter_printf(emitter, "%zd", *ssizep);
		break;
	case emitter_type_size:
		sizep = (const size_t *)value;
		emitter_printf(emitter, "%zu", *sizep);
		break;
	case emitter_type_string:
		charpp = (const char *const *)value;
		emitter_printf(emitter, "\"%s\"", *charpp);
		break;
	case emitter_type_uint32:
		uint32p = (const uint32_t *)value;
		emitter_printf(emitter, "%"FMTu32, *uint32p);
		break;
	case emitter_type_uint64:
		uint64p = (const uint64_t *)value;
		emitter_printf(emitter, "%"FMTu64, *uint64p);
		break;
	default:
		unreachable();
	}
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
	assert(!emitter->in_vdict);
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
		emitter_print_value(emitter, value_type, value);
	} else {
		emitter_printf(emitter, "  %s.%s: ", emitter->vdict_name,
		    key);
		emitter_print_value(emitter, value_type, value);
		if (note_key != NULL) {
			emitter_printf(emitter, " (%s: ", note_key);
			emitter_print_value(emitter, note_value_type,
			    note_value);
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
		emitter_print_value(emitter, value_type, value);
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
		emitter_print_value(emitter, value_type, value);
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

#endif /* JEMALLOC_INTERNAL_EMITTER_H */
