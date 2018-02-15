#include "test/jemalloc_test.h"
#include "jemalloc/internal/emitter.h"

/*
 * This is so useful for debugging and feature work, we'll leave printing
 * functionality committed but disabled by default.
 */
/* Print the text as it will appear. */
static bool print_raw = false;
/* Print the text escaped, so it can be copied back into the test case. */
static bool print_escaped = false;

typedef struct buf_descriptor_s buf_descriptor_t;
struct buf_descriptor_s {
	char *buf;
	size_t len;
	bool mid_quote;
};

/*
 * Forwards all writes to the passed-in buf_v (which should be cast from a
 * buf_descriptor_t *).
 */
static void
forwarding_cb(void *buf_descriptor_v, const char *str) {
	buf_descriptor_t *buf_descriptor = (buf_descriptor_t *)buf_descriptor_v;

	if (print_raw) {
		malloc_printf("%s", str);
	}
	if (print_escaped) {
		const char *it = str;
		while (*it != '\0') {
			if (!buf_descriptor->mid_quote) {
				malloc_printf("\"");
				buf_descriptor->mid_quote = true;
			}
			switch (*it) {
			case '\\':
				malloc_printf("\\");
				break;
			case '\"':
				malloc_printf("\\\"");
				break;
			case '\t':
				malloc_printf("\\t");
				break;
			case '\n':
				malloc_printf("\\n\"\n");
				buf_descriptor->mid_quote = false;
				break;
			default:
				malloc_printf("%c", *it);
			}
			it++;
		}
	}

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
	buf_descriptor.mid_quote = false;

	emitter_init(&emitter, emitter_output_json, &forwarding_cb,
	    &buf_descriptor);
	(*emit_fn)(&emitter);
	assert_str_eq(expected_json_output, buf, "json output failure");

	buf_descriptor.buf = buf;
	buf_descriptor.len = MALLOC_PRINTF_BUFSIZE;
	buf_descriptor.mid_quote = false;

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
emit_json_simple_kv(emitter_t *emitter) {
	int ival = 123;

	emitter_begin(emitter);
	emitter_simple_kv(emitter, "j1", "J1", emitter_type_int, &ival);
	emitter_json_simple_kv(emitter, "j2", emitter_type_int, &ival);
	emitter_simple_kv(emitter, "j3", "J3", emitter_type_int, &ival);
	emitter_end(emitter);
}

static const char *json_simple_kv_json =
"{\n"
"\t\"j1\": 123,\n"
"\t\"j2\": 123,\n"
"\t\"j3\": 123\n"
"}\n";

static const char *json_simple_kv_table =
"J1: 123\n"
"J3: 123\n";

TEST_BEGIN(test_json_simple_kv) {
	assert_emit_output(&emit_json_simple_kv, json_simple_kv_json,
	    json_simple_kv_table);
}
TEST_END

static void
emit_types(emitter_t *emitter) {
	bool b = false;
	int i = -123;
	unsigned u = 123;
	ssize_t zd = -456;
	size_t zu = 456;
	const char *str = "string";
	uint32_t u32 = 789;
	uint64_t u64 = 10000000000ULL;

	emitter_begin(emitter);
	emitter_simple_kv(emitter, "k1", "K1", emitter_type_bool, &b);
	emitter_simple_kv(emitter, "k2", "K2", emitter_type_int, &i);
	emitter_simple_kv(emitter, "k3", "K3", emitter_type_unsigned, &u);
	emitter_simple_kv(emitter, "k4", "K4", emitter_type_ssize, &zd);
	emitter_simple_kv(emitter, "k5", "K5", emitter_type_size, &zu);
	emitter_simple_kv(emitter, "k6", "K6", emitter_type_string, &str);
	emitter_simple_kv(emitter, "k7", "K7", emitter_type_uint32, &u32);
	emitter_simple_kv(emitter, "k8", "K8", emitter_type_uint64, &u64);
	emitter_end(emitter);
}

static const char *types_json =
"{\n"
"\t\"k1\": false,\n"
"\t\"k2\": -123,\n"
"\t\"k3\": 123,\n"
"\t\"k4\": -456,\n"
"\t\"k5\": 456,\n"
"\t\"k6\": \"string\",\n"
"\t\"k7\": 789,\n"
"\t\"k8\": 10000000000\n"
"}\n";

static const char *types_table =
"K1: false\n"
"K2: -123\n"
"K3: 123\n"
"K4: -456\n"
"K5: 456\n"
"K6: \"string\"\n"
"K7: 789\n"
"K8: 10000000000\n";

TEST_BEGIN(test_types) {
	assert_emit_output(&emit_types, types_json, types_table);
}
TEST_END

static void
emit_json_arr(emitter_t *emitter) {
	int ival = 123;

	emitter_begin(emitter);
	emitter_json_dict_begin(emitter, "dict");
	emitter_json_arr_begin(emitter, "arr");
	emitter_json_arr_dict_begin(emitter);
	emitter_json_simple_kv(emitter, "foo", emitter_type_int, &ival);
	emitter_json_arr_dict_end(emitter); /* Close arr[0] */
	emitter_json_arr_dict_begin(emitter);
	emitter_json_simple_kv(emitter, "bar", emitter_type_int, &ival);
	emitter_json_simple_kv(emitter, "baz", emitter_type_int, &ival);
	emitter_json_arr_dict_end(emitter); /* Close arr[1]. */
	emitter_json_arr_end(emitter); /* Close arr. */
	emitter_json_dict_end(emitter); /* Close dict. */
	emitter_end(emitter);
}

static const char *json_arr_json =
"{\n"
"\t\"dict\": {\n"
"\t\t\"arr\": [\n"
"\t\t\t{\n"
"\t\t\t\t\"foo\": 123\n"
"\t\t\t},\n"
"\t\t\t{\n"
"\t\t\t\t\"bar\": 123,\n"
"\t\t\t\t\"baz\": 123\n"
"\t\t\t}\n"
"\t\t]\n"
"\t}\n"
"}\n";

static const char *json_arr_table = "";

TEST_BEGIN(test_json_arr) {
	assert_emit_output(&emit_json_arr, json_arr_json, json_arr_table);
}
TEST_END

static void
emit_hdict(emitter_t *emitter) {
	int val = 123;

	emitter_begin(emitter);
	emitter_hdict_begin(emitter, "foo");
	emitter_hdict_kv(emitter, "abc", "Abc", emitter_type_int, &val);
	emitter_hdict_kv(emitter, "def", "def", emitter_type_int, &val);
	emitter_hdict_kv(emitter, "ghi", "GHI", emitter_type_int, &val);
	emitter_hdict_end(emitter);
	emitter_end(emitter);
}
static const char *hdict_json =
"{\n"
"\t\"foo\": {\n"
"\t\t\"abc\": 123,\n"
"\t\t\"def\": 123,\n"
"\t\t\"ghi\": 123\n"
"\t}\n"
"}\n";
static const char *hdict_table =
"Abc: 123, def: 123, GHI: 123\n";

TEST_BEGIN(test_hdict) {
	assert_emit_output(&emit_hdict, hdict_json, hdict_table);
}
TEST_END

static void
emit_hdict_split_end(emitter_t *emitter) {
	int val = 123;

	emitter_begin(emitter);
	emitter_hdict_begin(emitter, "foo");
	emitter_hdict_kv(emitter, "abc", "Abc", emitter_type_int, &val);
	emitter_hdict_kv(emitter, "def", "def", emitter_type_int, &val);
	emitter_table_hdict_end(emitter);
	emitter_table_hdict_begin(emitter);
	emitter_hdict_kv(emitter, "ghi", "Ghi", emitter_type_int, &val);
	emitter_table_hdict_end(emitter);
	emitter_json_simple_kv(emitter, "jkl", emitter_type_int, &val);
	emitter_json_hdict_end(emitter);
	emitter_end(emitter);
}
static const char *hdict_split_end_json =
"{\n"
"\t\"foo\": {\n"
"\t\t\"abc\": 123,\n"
"\t\t\"def\": 123,\n"
"\t\t\"ghi\": 123,\n"
"\t\t\"jkl\": 123\n"
"\t}\n"
"}\n";
static const char *hdict_split_end_table =
"Abc: 123, def: 123\n"
"Ghi: 123\n";

TEST_BEGIN(test_hdict_split_end) {
	assert_emit_output(&emit_hdict_split_end, hdict_split_end_json,
	    hdict_split_end_table);
}
TEST_END

static void
emit_tabarr(emitter_t *emitter) {
	emitter_shape_t root;
	emitter_shape_init_root(&root, "root", "Root", emitter_justify_right,
	    10);
	    emitter_shape_t dict1;
	    emitter_shape_init_dict(&dict1, &root, "dict1", "",
		emitter_justify_left, 0);

	        emitter_shape_t nest1;
		emitter_shape_init_dict(&nest1, &dict1, "nest1", "",
		    emitter_justify_left, 0);

		    emitter_shape_t ival;
		    emitter_shape_init_primitive(&ival, &nest1, "int_key",
			"Int header", emitter_justify_right, 15, emitter_type_int);

		    emitter_shape_t strval;
		    emitter_shape_init_primitive(&strval, &nest1, "str_key",
			"Str header", emitter_justify_right, 25,
			emitter_type_string);

		emitter_shape_t nest2;
		emitter_shape_init_dict(&nest2, &dict1, "nest2", "",
		    emitter_justify_right, 20);
		    /*
		     * There's a right-justified field next to a left-justified
		     * one.  Let's add some padding.
		     */
		    emitter_shape_t padding;
		    emitter_shape_init_padding(&padding, &nest2, 1);

		    emitter_shape_t sizeval;
		    emitter_shape_init_primitive(&sizeval, &nest2,
			"size_header", "Size header", emitter_justify_left, 10,
			emitter_type_size);

	emitter_begin(emitter);
	emitter_tabarr_begin(emitter, &root);

	ival.int_value = 123;
	strval.string_value = "strval 1";
	sizeval.size_value = 456;
	emitter_tabarr_obj(emitter, &root);

	ival.int_value = 789;
	strval.string_value = "different strval";
	sizeval.size_value = 101112;
	emitter_tabarr_obj(emitter, &root);

	nest2.table_key = "labelled dict";
	ival.int_value = 131415;
	strval.string_value = "a third strval";
	sizeval.size_value = 161718;
	emitter_tabarr_obj(emitter, &root);


	emitter_tabarr_end(emitter);

	emitter_end(emitter);
}

static const char *tabarr_json =
"{\n"
"\t\"root\": [\n"
"\t\t{\n"
"\t\t\t\"dict1\": {\n"
"\t\t\t\t\"nest1\": {\n"
"\t\t\t\t\t\"int_key\": 123,\n"
"\t\t\t\t\t\"str_key\": \"strval 1\"\n"
"\t\t\t\t},\n"
"\t\t\t\t\"nest2\": {\n"
"\t\t\t\t\t\"size_header\": 456\n"
"\t\t\t\t}\n"
"\t\t\t}\n"
"\t\t},\n"
"\t\t{\n"
"\t\t\t\"dict1\": {\n"
"\t\t\t\t\"nest1\": {\n"
"\t\t\t\t\t\"int_key\": 789,\n"
"\t\t\t\t\t\"str_key\": \"different strval\"\n"
"\t\t\t\t},\n"
"\t\t\t\t\"nest2\": {\n"
"\t\t\t\t\t\"size_header\": 101112\n"
"\t\t\t\t}\n"
"\t\t\t}\n"
"\t\t},\n"
"\t\t{\n"
"\t\t\t\"dict1\": {\n"
"\t\t\t\t\"nest1\": {\n"
"\t\t\t\t\t\"int_key\": 131415,\n"
"\t\t\t\t\t\"str_key\": \"a third strval\"\n"
"\t\t\t\t},\n"
"\t\t\t\t\"nest2\": {\n"
"\t\t\t\t\t\"size_header\": 161718\n"
"\t\t\t\t}\n"
"\t\t\t}\n"
"\t\t}\n"
"\t]\n"
"}\n";

static const char *tabarr_table =
"      Root     Int header               Str header                     Size header\n"
"                      123               \"strval 1\"                     456       \n"
"                      789       \"different strval\"                     101112    \n"
"                   131415         \"a third strval\"      labelled dict: 161718    \n"
"\n";

TEST_BEGIN(test_tabarr) {
	assert_emit_output(&emit_tabarr, tabarr_json, tabarr_table);
}
TEST_END

static void
emit_tabdict(emitter_t *emitter) {
	emitter_shape_t root;
	emitter_shape_init_root(&root, "root", "Root", emitter_justify_left,
	    8);

	emitter_shape_t dict;
	emitter_shape_init_dict(&dict, &root, "", "",
	    emitter_justify_left, 10);

	    emitter_shape_t ival;
	    emitter_shape_init_primitive(&ival, &dict, "valkey", "Value Key",
		emitter_justify_right, 10, emitter_type_int);

	emitter_begin(emitter);
	emitter_tabdict_begin(emitter, &root);

	dict.json_key = "dict1";
	dict.table_key = "Dict 1";
	ival.int_value = 123;
	emitter_tabdict_kv(emitter, &root);

	dict.json_key = "dict2";
	dict.table_key = "Dict 2";
	ival.int_value = 456;
	emitter_tabdict_kv(emitter, &root);

	dict.json_key = "dict3";
	dict.table_key = "Dict 3";
	ival.int_value = 789;
	emitter_tabdict_kv(emitter, &root);

	emitter_tabdict_end(emitter);
	emitter_end(emitter);
}

static const char *tabdict_json =
"{\n"
"\t\"root\": {\n"
"\t\t\"dict1\": {\n"
"\t\t\t\"valkey\": 123\n"
"\t\t},\n"
"\t\t\"dict2\": {\n"
"\t\t\t\"valkey\": 456\n"
"\t\t},\n"
"\t\t\"dict3\": {\n"
"\t\t\t\"valkey\": 789\n"
"\t\t}\n"
"\t}\n"
"}\n";

static const char *tabdict_table =
"Root               Value Key\n"
"        Dict 1:          123\n"
"        Dict 2:          456\n"
"        Dict 3:          789\n"
"\n";

TEST_BEGIN(test_tabdict) {
	assert_emit_output(&emit_tabdict, tabdict_json, tabdict_table);
}
TEST_END

int
main(void) {
	return test_no_reentrancy(
	    test_vdict,
	    test_table_note,
	    test_emit_json_dict,
	    test_simple_kv,
	    test_json_simple_kv,
	    test_types,
	    test_json_arr,
	    test_hdict,
	    test_hdict_split_end,
	    test_tabarr,
	    test_tabdict);
}
