#ifndef ODYSIA_INDEXER_H
#define ODYSIA_INDEXER_H

#include <gio/gio.h>

typedef enum {
    ODYSIA_SYMBOL_FUNCTION,
    ODYSIA_SYMBOL_STRUCT,
    ODYSIA_SYMBOL_UNION,
    ODYSIA_SYMBOL_ENUM,
    ODYSIA_SYMBOL_ENUMERATOR,
    ODYSIA_SYMBOL_TYPEDEF,
    ODYSIA_SYMBOL_GLOBAL_VARIABLE,
    ODYSIA_SYMBOL_FIELD,
    ODYSIA_SYMBOL_PARAMETER,
    ODYSIA_SYMBOL_LOCAL_VARIABLE,
    ODYSIA_SYMBOL_MODULE,
    ODYSIA_SYMBOL_CLASS,
    ODYSIA_SYMBOL_TRAIT,
    ODYSIA_SYMBOL_MACRO,
    ODYSIA_SYMBOL_LABEL,
    ODYSIA_SYMBOL_CONFIG,
    ODYSIA_SYMBOL_BUILD_TARGET,
    ODYSIA_SYMBOL_DEVICE_NODE,
    ODYSIA_SYMBOL_RULE
} OdysiaSymbolKind;

typedef enum {
    ODYSIA_RELATION_CALL,
    ODYSIA_RELATION_TYPE,
    ODYSIA_RELATION_ALIAS,
    ODYSIA_RELATION_KEYWORD
} OdysiaRelationKind;

typedef struct {
    OdysiaRelationKind kind;
    gchar *label;
    gchar *target_name;
    gchar *detail;
} OdysiaRelation;

typedef struct {
    gchar *id;
    gchar *name;
    gchar *display_name;
    gchar *parent_id;
    gchar *file_path;
    gint line;
    OdysiaSymbolKind kind;
    gchar *signature;
    gchar *type_text;
    gchar *documentation;
    gchar *snippet;
    GPtrArray *children;
    GPtrArray *relations;
} OdysiaSymbol;

typedef struct {
    gchar *path;
    gchar *content;
} OdysiaDocFile;

typedef struct {
    gchar *root_path;
    GPtrArray *symbols;
    GHashTable *symbols_by_id;
    GHashTable *symbols_by_name;
    GPtrArray *doc_files;
} OdysiaIndex;

typedef enum {
    ODYSIA_INDEX_STAGE_DISCOVER,
    ODYSIA_INDEX_STAGE_PARSE_SOURCE,
    ODYSIA_INDEX_STAGE_PARSE_DOCS,
    ODYSIA_INDEX_STAGE_BUILD_TREE
} OdysiaIndexStage;

typedef void (*OdysiaIndexProgressFunc)(OdysiaIndexStage stage,
                                        guint current,
                                        guint total,
                                        const gchar *path,
                                        gpointer user_data);

OdysiaIndex *odysia_index_build(const gchar *root_path, GError **error);
OdysiaIndex *odysia_index_build_with_progress(const gchar *root_path,
                                              OdysiaIndexProgressFunc progress_func,
                                              gpointer user_data,
                                              GCancellable *cancellable,
                                              guint source_thread_count,
                                              GError **error);
void odysia_index_free(OdysiaIndex *index);
OdysiaSymbol *odysia_index_get_symbol(const OdysiaIndex *index, const gchar *symbol_id);
OdysiaSymbol *odysia_index_resolve_name(const OdysiaIndex *index, const gchar *symbol_name, OdysiaSymbolKind preferred_kind);
gchar *odysia_index_collect_external_docs(const OdysiaIndex *index, const gchar *symbol_name);
gboolean odysia_index_save_sqlite(const OdysiaIndex *index, const gchar *sqlite_path, GError **error);
OdysiaIndex *odysia_index_load_sqlite(const gchar *sqlite_path, GError **error);
const gchar *odysia_symbol_kind_name(OdysiaSymbolKind kind);
const gchar *odysia_symbol_language_name(const OdysiaSymbol *symbol);
gchar *odysia_extract_type_candidate(const gchar *type_text);
gboolean odysia_symbol_has_name(const OdysiaSymbol *symbol, const gchar *name);

#endif