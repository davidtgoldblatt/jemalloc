#ifndef JEMALLOC_INTERNAL_EMITTER_H
#define JEMALLOC_INTERNAL_EMITTER_H

typedef enum emitter_type_e emitter_type_t;
enum emitter_type_e {
	emitter_type_json,
	emitter_type_table
};

typedef struct emitter_s emitter_t;
struct emitter_s {
	emitter_type_t type;
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
emitter_init(emitter_t *emitter, emitter_type_t emitter_type,
    void (*write_cb)(void *, const char *), void *cbopaque) {
	emitter->type = emitter_type;
	emitter->in_vdict = false;
	emitter->write_cb = write_cb;
	emitter->cbopaque = cbopaque;
	if (emitter_type == emitter_type_json) {
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
	assert(emitter->type == emitter_type_json);
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
 * Internal.  Terminate the current json dict.
 */
static inline void
emitter_json_dict_finish(emitter_t *emitter) {
	emitter_json_assert_state(emitter);
	assert(emitter->nesting_depth > 0);
	emitter_json_clear_first_key(emitter);
	--emitter->nesting_depth;
	emitter_printf(emitter, "\n");
	for (int i = 0; i < emitter->nesting_depth; i++) {
		emitter_printf(emitter, "\t");
	}
	emitter_printf(emitter, "}");
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

/* Begin actually writing to the output. */
static inline void
emitter_begin(emitter_t *emitter) {
	if (emitter->type == emitter_type_json) {
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
	if (emitter->type == emitter_type_json) {
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

	if (emitter->type == emitter_type_json) {
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

	if (emitter->type == emitter_type_json) {
		emitter_json_dict_finish(emitter);
	}
}

static inline void
emitter_vdict_kv(emitter_t *emitter, const char *key, const char* value) {
	assert(emitter->in_vdict);
	if (emitter->type == emitter_type_json) {
		emitter_json_key_prefix(emitter);
		emitter_printf(emitter, "\"%s\": %s", key, value);
	} else {
		emitter_printf(emitter, "  %s.%s: %s\n", emitter->vdict_name,
		    key, value);
	}
}

/* Begin a dict that appears only in the json output. */
static inline void
emitter_json_dict_begin(emitter_t *emitter, const char *dict_name) {
	if (emitter->type != emitter_type_json) {
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
	if (emitter->type != emitter_type_json) {
		return;
	}
	emitter_json_assert_state(emitter);
	emitter_json_dict_finish(emitter);
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
    const char *table_key, const char *value) {
	if (emitter->type == emitter_type_json) {
		emitter_json_key_prefix(emitter);
		emitter_printf(emitter, "\"%s\": %s", json_key, value);
	} else {
		emitter_printf(emitter, "%s: %s\n", table_key, value);
	}
}

/* Print a row that appears only in tabular mode. */
static inline void
emitter_table_note(emitter_t *emitter, const char *format, ...) {
	va_list ap;

	va_start(ap, format);
	if (emitter->type == emitter_type_table) {
		malloc_vcprintf(emitter->write_cb, emitter->cbopaque, format, ap);
		emitter_printf(emitter, "\n");
	}
	va_end(ap);
}

#endif /* JEMALLOC_INTERNAL_EMITTER_H */
