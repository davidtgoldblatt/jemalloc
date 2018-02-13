#include "test/jemalloc_test.h"

#include "jemalloc/internal/emitter.h"

/*
 * This is so useful for debugging and feature work, we'll leave printing
 * functionality committed but disabled by default.
 */
static bool actually_print = false;

typedef struct buf_descriptor_s buf_descriptor_t;
struct buf_descriptor_s {
	char *buf;
	size_t len;
};

/*
 * Forwards all writes to the passed-in buf_v (which should be cast from a
 * buf_descriptor_t *).
 */
static void
forwarding_cb(void *buf_descriptor_v, const char *str) {
	if (actually_print) {
		malloc_printf("%s", str);
	}
	buf_descriptor_t *buf_descriptor = (buf_descriptor_t *)buf_descriptor_v;
	size_t written = malloc_snprintf(buf_descriptor->buf,
	    buf_descriptor->len, "%s", str);
	assert_zu_eq(written, strlen(str), "Buffer overflow!");
	buf_descriptor->buf += written;
	buf_descriptor->len -= written;
	assert_zu_gt(buf_descriptor->len, 0, "Buffer out of space!");
}

static void
assert_emit_output(void (*emit_fn)(emitter_t *),
    const char *expected_json_output, const char *expected_table_output) {
	emitter_t emitter;
	char buf[MALLOC_PRINTF_BUFSIZE];
	buf_descriptor_t buf_descriptor;

	buf_descriptor.buf = buf;
	buf_descriptor.len = MALLOC_PRINTF_BUFSIZE;
	emitter_init(&emitter, emitter_output_json, &forwarding_cb,
	    &buf_descriptor);
	(*emit_fn)(&emitter);
	assert_str_eq(expected_json_output, buf, "json output failure");

	buf_descriptor.buf = buf;
	buf_descriptor.len = MALLOC_PRINTF_BUFSIZE;
	emitter_init(&emitter, emitter_output_table, &forwarding_cb,
	    &buf_descriptor);
	(*emit_fn)(&emitter);
	assert_str_eq(expected_table_output, buf, "table output failure");
}

static void
emit_vdict(emitter_t *emitter) {
	bool b_false = false;
	bool b_true = true;
	int i_123 = 123;
	const char *str = "a string";

	emitter_begin(emitter);
	emitter_vdict_begin(emitter, "foo", "This is the foo table:");
	emitter_vdict_kv(emitter, "abc", emitter_type_bool, &b_false);
	emitter_vdict_kv(emitter, "def", emitter_type_bool, &b_true);
	emitter_vdict_kv_note(emitter, "ghi", emitter_type_int, &i_123,
	    "note_key1", emitter_type_string, &str);
	emitter_vdict_kv_note(emitter, "jkl", emitter_type_string, &str,
	    "note_key2", emitter_type_bool, &b_false);
	emitter_vdict_end(emitter);
	emitter_end(emitter);
}
static const char *vdict_json =
"{\n"
"\t\"foo\": {\n"
"\t\t\"abc\": false,\n"
"\t\t\"def\": true,\n"
"\t\t\"ghi\": 123,\n"
"\t\t\"jkl\": \"a string\"\n"
"\t}\n"
"}\n";
static const char *vdict_table =
"This is the foo table:\n"
"  foo.abc: false\n"
"  foo.def: true\n"
"  foo.ghi: 123 (note_key1: \"a string\")\n"
"  foo.jkl: \"a string\" (note_key2: false)\n";

TEST_BEGIN(test_vdict) {
	assert_emit_output(&emit_vdict, vdict_json, vdict_table);
}
TEST_END

static void
emit_table_note(emitter_t *emitter) {
	emitter_begin(emitter);
	emitter_table_note(emitter, "Table note 1");
	emitter_table_note(emitter, "Table note 2 %s", "with format string");
	emitter_end(emitter);
}

static const char *table_note_json =
"{\n"
"}\n";

static const char *table_note_table =
"Table note 1\n"
"Table note 2 with format string\n";

TEST_BEGIN(test_table_note) {
	assert_emit_output(&emit_table_note, table_note_json, table_note_table);
}
TEST_END

static void
emit_json_dict(emitter_t *emitter) {
	emitter_begin(emitter);
	emitter_json_dict_begin(emitter, "root1a");
	emitter_json_dict_begin(emitter, "nested1a");
	emitter_json_dict_begin(emitter, "nested2a");
	emitter_json_dict_begin(emitter, "nested3a");
	emitter_json_dict_end(emitter); /* Ends nested3a */
	emitter_json_dict_begin(emitter, "nested3b");
	emitter_json_dict_end(emitter); /* Ends nested3b */
	emitter_json_dict_end(emitter); /* Ends nested2a */
	emitter_json_dict_begin(emitter, "nested2b");
	emitter_json_dict_end(emitter); /* Ends nested2b */
	emitter_json_dict_begin(emitter, "nested2c");
	emitter_json_dict_end(emitter); /* Ends nested2c */
	emitter_json_dict_end(emitter); /* Ends nested1a */
	emitter_json_dict_begin(emitter, "nested1b");
	emitter_json_dict_end(emitter); /* Ends nested1b */
	emitter_json_dict_end(emitter); /* Ends root1a */
	emitter_json_dict_begin(emitter, "root1b");
	emitter_json_dict_end(emitter); /* Ends root1b */
	emitter_json_dict_begin(emitter, "root1c");
	emitter_json_dict_end(emitter); /* Ends root1c */
	emitter_end(emitter);
}

static const char *emit_json_dict_json =
"{\n"
"\t\"root1a\": {\n"
"\t\t\"nested1a\": {\n"
"\t\t\t\"nested2a\": {\n"
"\t\t\t\t\"nested3a\": {\n"
"\t\t\t\t},\n"
"\t\t\t\t\"nested3b\": {\n"
"\t\t\t\t}\n"
"\t\t\t},\n"
"\t\t\t\"nested2b\": {\n"
"\t\t\t},\n"
"\t\t\t\"nested2c\": {\n"
"\t\t\t}\n"
"\t\t},\n"
"\t\t\"nested1b\": {\n"
"\t\t}\n"
"\t},\n"
"\t\"root1b\": {\n"
"\t},\n"
"\t\"root1c\": {\n"
"\t}\n"
"}\n";
static const char *emit_json_dict_table = "";

TEST_BEGIN(test_emit_json_dict) {
	assert_emit_output(&emit_json_dict, emit_json_dict_json,
	    emit_json_dict_table);
}
TEST_END

static void
emit_simple_kv(emitter_t *emitter) {
	const char *strval = "a string";
	int intval = 123;
	bool boolval = true;

	emitter_begin(emitter);
	emitter_json_dict_begin(emitter, "nest1");
	emitter_json_dict_begin(emitter, "nest2");
	emitter_simple_kv(emitter, "json_key1", "Table key 1",
	    emitter_type_string, &strval);
	emitter_simple_kv(emitter, "json_key2", "Table key 2",
	    emitter_type_string, &strval);
	emitter_json_dict_end(emitter); /* Ends nest2 */
	emitter_simple_kv(emitter, "json_key3", "Table key 3",
	    emitter_type_int, &intval);
	emitter_simple_kv(emitter, "json_key4", "Table key 4",
	    emitter_type_bool, &boolval);
	emitter_json_dict_end(emitter); /* Ends nest1 */
	emitter_simple_kv(emitter, "json_key5", "Table key 5",
	    emitter_type_bool, &boolval);
	emitter_simple_kv(emitter, "json_key6", "Table key 6",
	    emitter_type_string, &strval);
	emitter_end(emitter);
}

static const char *simple_kv_json =
"{\n"
"\t\"nest1\": {\n"
"\t\t\"nest2\": {\n"
"\t\t\t\"json_key1\": \"a string\",\n"
"\t\t\t\"json_key2\": \"a string\"\n"
"\t\t},\n"
"\t\t\"json_key3\": 123,\n"
"\t\t\"json_key4\": true\n"
"\t},\n"
"\t\"json_key5\": true,\n"
"\t\"json_key6\": \"a string\"\n"
"}\n";

static const char *simple_kv_table =
"Table key 1: \"a string\"\n"
"Table key 2: \"a string\"\n"
"Table key 3: 123\n"
"Table key 4: true\n"
"Table key 5: true\n"
"Table key 6: \"a string\"\n";

TEST_BEGIN(test_simple_kv) {
	assert_emit_output(&emit_simple_kv, simple_kv_json, simple_kv_table);
}
TEST_END

static void
emit_types(emitter_t *emitter) {
	bool b = false;
	int i = -123;
	unsigned u = 123;
	ssize_t zd = -456;
	const char *str = "string";

	emitter_begin(emitter);
	emitter_simple_kv(emitter, "k1", "K1", emitter_type_bool, &b);
	emitter_simple_kv(emitter, "k2", "K2", emitter_type_int, &i);
	emitter_simple_kv(emitter, "k3", "K3", emitter_type_unsigned, &u);
	emitter_simple_kv(emitter, "k4", "K4", emitter_type_ssize, &zd);
	emitter_simple_kv(emitter, "k5", "K5", emitter_type_string, &str);
	emitter_end(emitter);
}

static const char *types_json =
"{\n"
"\t\"k1\": false,\n"
"\t\"k2\": -123,\n"
"\t\"k3\": 123,\n"
"\t\"k4\": -456,\n"
"\t\"k5\": \"string\"\n"
"}\n";

static const char *types_table =
"K1: false\n"
"K2: -123\n"
"K3: 123\n"
"K4: -456\n"
"K5: \"string\"\n";

TEST_BEGIN(test_types) {
	assert_emit_output(&emit_types, types_json, types_table);
}
TEST_END

int
main(void) {
	return test_no_reentrancy(
	    test_vdict,
	    test_table_note,
	    test_emit_json_dict,
	    test_simple_kv,
	    test_types);
}
