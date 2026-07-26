#include "../src/indexer.h"

#include <glib.h>

static OdysiaSymbol *find_child(const OdysiaIndex *index, const OdysiaSymbol *parent, const gchar *name)
{
    guint index_child;

    for (index_child = 0; index_child < parent->children->len; index_child++) {
        OdysiaSymbol *child;

        child = odysia_index_get_symbol(index, g_ptr_array_index(parent->children, index_child));
        if (odysia_symbol_has_name(child, name)) {
            return child;
        }
    }

    return NULL;
}

static gboolean has_relation(const OdysiaSymbol *symbol, OdysiaRelationKind kind, const gchar *target_name)
{
    guint index_relation;

    for (index_relation = 0; index_relation < symbol->relations->len; index_relation++) {
        OdysiaRelation *relation;

        relation = g_ptr_array_index(symbol->relations, index_relation);
        if (relation->kind == kind && g_strcmp0(relation->target_name, target_name) == 0) {
            return TRUE;
        }
    }

    return FALSE;
}

static void test_index_fixture(void)
{
    GError *error;
    OdysiaIndex *index;
    OdysiaSymbol *function_symbol;
    OdysiaSymbol *struct_symbol;
    OdysiaSymbol *field_symbol;
    OdysiaSymbol *enum_symbol;
    OdysiaSymbol *enumerator_symbol;
    OdysiaSymbol *local_symbol;
    OdysiaSymbol *param_symbol;
    gchar *docs;

    error = NULL;
    index = odysia_index_build("tests/fixtures/linux_sample", &error);
    g_assert_no_error(error);
    g_assert_nonnull(index);
    g_assert_cmpuint(index->symbols->len, >, 6);

    function_symbol = odysia_index_resolve_name(index, "sample_log", ODYSIA_SYMBOL_FUNCTION);
    g_assert_nonnull(function_symbol);
    g_assert_true(has_relation(function_symbol, ODYSIA_RELATION_CALL, "helper"));

    param_symbol = find_child(index, function_symbol, "tag");
    g_assert_nonnull(param_symbol);

    local_symbol = find_child(index, function_symbol, "callback");
    g_assert_nonnull(local_symbol);
    g_assert_nonnull(local_symbol->type_text);
    g_assert_nonnull(strstr(local_symbol->type_text, "(*)"));

    struct_symbol = odysia_index_resolve_name(index, "sample_state", ODYSIA_SYMBOL_STRUCT);
    g_assert_nonnull(struct_symbol);
    field_symbol = find_child(index, struct_symbol, "handler");
    g_assert_nonnull(field_symbol);
    g_assert_nonnull(field_symbol->type_text);
    g_assert_nonnull(strstr(field_symbol->type_text, "(*)"));

    enum_symbol = odysia_index_resolve_name(index, "sample_mode", ODYSIA_SYMBOL_ENUM);
    g_assert_nonnull(enum_symbol);
    enumerator_symbol = find_child(index, enum_symbol, "SAMPLE_MODE_ON");
    g_assert_nonnull(enumerator_symbol);
    g_assert_cmpint(enumerator_symbol->kind, ==, ODYSIA_SYMBOL_ENUMERATOR);

    g_assert_nonnull(odysia_index_resolve_name(index, "rust_helper", ODYSIA_SYMBOL_FUNCTION));
    g_assert_nonnull(odysia_index_resolve_name(index, "RustState", ODYSIA_SYMBOL_STRUCT));
    g_assert_nonnull(odysia_index_resolve_name(index, "KernelThing", ODYSIA_SYMBOL_TRAIT));
    g_assert_nonnull(odysia_index_resolve_name(index, "sample_macro", ODYSIA_SYMBOL_MACRO));
    g_assert_nonnull(odysia_index_resolve_name(index, "sample_asm_entry", ODYSIA_SYMBOL_FUNCTION));
    g_assert_nonnull(odysia_index_resolve_name(index, "python_helper", ODYSIA_SYMBOL_FUNCTION));
    g_assert_nonnull(odysia_index_resolve_name(index, "shell_helper", ODYSIA_SYMBOL_FUNCTION));
    g_assert_nonnull(odysia_index_resolve_name(index, "perl_helper", ODYSIA_SYMBOL_FUNCTION));
    g_assert_nonnull(odysia_index_resolve_name(index, "awk_helper", ODYSIA_SYMBOL_FUNCTION));
    g_assert_nonnull(odysia_index_resolve_name(index, "ODYSIA_SAMPLE", ODYSIA_SYMBOL_CONFIG));
    g_assert_nonnull(odysia_index_resolve_name(index, "sample-target", ODYSIA_SYMBOL_BUILD_TARGET));
    g_assert_nonnull(odysia_index_resolve_name(index, "sample_node", ODYSIA_SYMBOL_DEVICE_NODE));
    g_assert_nonnull(odysia_index_resolve_name(index, "sample_rule", ODYSIA_SYMBOL_RULE));
    g_assert_nonnull(odysia_index_resolve_name(index, "sample_grammar", ODYSIA_SYMBOL_RULE));
    g_assert_nonnull(odysia_index_resolve_name(index, ".sample_section", ODYSIA_SYMBOL_LABEL));
    g_assert_cmpstr(odysia_symbol_language_name(function_symbol), ==, "C");
    g_assert_cmpstr(odysia_symbol_language_name(
                        odysia_index_resolve_name(index, "rust_helper", ODYSIA_SYMBOL_FUNCTION)),
                    ==,
                    "Rust");
    g_assert_cmpstr(odysia_symbol_language_name(
                        odysia_index_resolve_name(index, "python_helper", ODYSIA_SYMBOL_FUNCTION)),
                    ==,
                    "Python");
    g_assert_cmpstr(odysia_symbol_language_name(
                        odysia_index_resolve_name(index, "shell_helper", ODYSIA_SYMBOL_FUNCTION)),
                    ==,
                    "Shell");
    g_assert_cmpstr(odysia_symbol_language_name(
                        odysia_index_resolve_name(index, ".sample_section", ODYSIA_SYMBOL_LABEL)),
                    ==,
                    "Linker Script");

    docs = odysia_index_collect_external_docs(index, "sample_log");
    g_assert_nonnull(docs);
    g_assert_true(docs[0] != '\0');
    g_free(docs);

    odysia_index_free(index);
}

static void test_configurable_source_threads(void)
{
    GError *error;
    OdysiaIndex *single_threaded;
    OdysiaIndex *multi_threaded;

    error = NULL;
    single_threaded = odysia_index_build_with_progress("tests/fixtures/linux_sample",
                                                       NULL,
                                                       NULL,
                                                       NULL,
                                                       1,
                                                       &error);
    g_assert_no_error(error);
    g_assert_nonnull(single_threaded);

    multi_threaded = odysia_index_build_with_progress("tests/fixtures/linux_sample",
                                                      NULL,
                                                      NULL,
                                                      NULL,
                                                      3,
                                                      &error);
    g_assert_no_error(error);
    g_assert_nonnull(multi_threaded);
    g_assert_cmpuint(single_threaded->symbols->len, ==, multi_threaded->symbols->len);
    g_assert_nonnull(odysia_index_resolve_name(multi_threaded, "sample_log", ODYSIA_SYMBOL_FUNCTION));
    g_assert_nonnull(odysia_index_resolve_name(multi_threaded, "sample_state", ODYSIA_SYMBOL_STRUCT));

    odysia_index_free(multi_threaded);
    odysia_index_free(single_threaded);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/indexer/fixture", test_index_fixture);
    g_test_add_func("/indexer/configurable-source-threads", test_configurable_source_threads);
    return g_test_run();
}