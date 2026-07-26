#include "indexer.h"

#include <stdio.h>
#include <string.h>
#include <sqlite3.h>

typedef struct {
    gchar *text;
    gchar *doc;
    gint line;
} SourceUnit;

typedef struct {
    OdysiaIndex *index;
    OdysiaIndexProgressFunc progress_func;
    gpointer progress_user_data;
    GCancellable *cancellable;
    GMutex mutex;
    guint completed_files;
    guint total_files;
} SourceParseContext;

typedef struct {
    SourceParseContext *context;
    const gchar *path;
} SourceParseTask;

static gboolean is_ident_start(gchar ch)
{
    return g_ascii_isalpha(ch) || ch == '_';
}

static gboolean is_ident_char(gchar ch)
{
    return g_ascii_isalnum(ch) || ch == '_';
}

static gboolean has_source_extension(const gchar *name)
{
    return g_str_has_suffix(name, ".c") ||
           g_str_has_suffix(name, ".h") ||
           g_str_has_suffix(name, ".rs") ||
           g_str_has_suffix(name, ".S") ||
           g_str_has_suffix(name, ".s") ||
           g_str_has_suffix(name, ".asm") ||
           g_str_has_suffix(name, ".py") ||
           g_str_has_suffix(name, ".sh") ||
           g_str_has_suffix(name, ".pl") ||
           g_str_has_suffix(name, ".pm") ||
           g_str_has_suffix(name, ".awk") ||
           g_str_has_suffix(name, ".dts") ||
           g_str_has_suffix(name, ".dtsi") ||
           g_str_has_suffix(name, ".cocci") ||
           g_str_has_suffix(name, ".l") ||
           g_str_has_suffix(name, ".y") ||
           g_str_has_suffix(name, ".lds") ||
           g_str_has_suffix(name, ".lds.S") ||
           g_str_has_suffix(name, ".mk") ||
           g_str_has_prefix(name, "Makefile") ||
           g_str_has_prefix(name, "Kbuild") ||
           g_str_has_prefix(name, "Kconfig");
}

static gboolean has_supported_shebang(const gchar *path)
{
    FILE *file;
    gchar line[256];
    gboolean supported;

    file = fopen(path, "r");
    if (file == NULL) {
        return FALSE;
    }
    supported = fgets(line, sizeof(line), file) != NULL &&
                g_str_has_prefix(line, "#!") &&
                (strstr(line, "sh") != NULL ||
                 strstr(line, "python") != NULL ||
                 strstr(line, "perl") != NULL ||
                 strstr(line, "awk") != NULL);
    fclose(file);
    return supported;
}

static gboolean has_doc_extension(const gchar *name)
{
    return g_str_has_suffix(name, ".rst") || g_str_has_suffix(name, ".txt") || g_str_has_suffix(name, ".md");
}

static gboolean is_qualifier_token(const gchar *token)
{
    return g_str_equal(token, "const") ||
           g_str_equal(token, "volatile") ||
           g_str_equal(token, "restrict") ||
           g_str_equal(token, "__restrict") ||
           g_str_equal(token, "__maybe_unused") ||
           g_str_equal(token, "__user") ||
           g_str_equal(token, "__iomem") ||
           g_str_equal(token, "__rcu") ||
           g_str_equal(token, "__percpu") ||
           g_str_equal(token, "__force");
}

static gboolean is_decorator_keyword(const gchar *token)
{
    return g_str_equal(token, "__attribute__") ||
           g_str_equal(token, "__aligned") ||
           g_str_equal(token, "__printf") ||
           g_str_equal(token, "__scanf") ||
           g_str_equal(token, "__malloc") ||
           g_str_equal(token, "__must_check") ||
           g_str_equal(token, "__pure") ||
           g_str_equal(token, "__always_inline") ||
           g_str_equal(token, "__maybe_unused") ||
           g_str_equal(token, "__init") ||
           g_str_equal(token, "__exit") ||
           g_str_equal(token, "__cold") ||
           g_str_equal(token, "__visible") ||
           g_str_equal(token, "__weak") ||
           g_str_equal(token, "__sched") ||
           g_str_equal(token, "__packed") ||
           g_str_equal(token, "__deprecated") ||
           g_str_equal(token, "__section") ||
           g_str_equal(token, "__latent_entropy") ||
           g_str_equal(token, "__nocfi") ||
           g_str_equal(token, "notrace") ||
           g_str_equal(token, "noinline") ||
           g_str_equal(token, "asmlinkage") ||
           g_str_equal(token, "inline") ||
           g_str_equal(token, "__inline__");
}

static const gchar *const c99_keywords[] = {
    "auto", "break", "case", "char", "const", "continue", "default", "do", "double",
    "else", "enum", "extern", "float", "for", "goto", "if", "inline", "int", "long",
    "register", "restrict", "return", "short", "signed", "sizeof", "static", "struct",
    "switch", "typedef", "union", "unsigned", "void", "volatile", "while", "_Bool",
    "_Complex", "_Imaginary"
};

static const gchar *const kernel_keywords[] = {
    "__always_inline", "__attribute__", "__cold", "__deprecated", "__exit", "__force",
    "__init", "__iomem", "__latent_entropy", "__maybe_unused", "__must_check", "__nocfi",
    "__packed", "__percpu", "__printf", "__pure", "__rcu", "__section", "__sched",
    "__user", "__visible", "__weak", "asmlinkage", "barrier", "container_of", "likely",
    "noinline", "notrace", "READ_ONCE", "smp_mb", "spin_lock", "spin_unlock", "unlikely",
    "WRITE_ONCE"
};

static gchar *strip_comments_and_literals(const gchar *text);

static gchar *trim_copy(const gchar *text)
{
    return g_strstrip(g_strdup(text != NULL ? text : ""));
}

static gchar *relative_path(const gchar *root_path, const gchar *path)
{
    if (g_str_has_prefix(path, root_path)) {
        const gchar *suffix;

        suffix = path + strlen(root_path);
        while (*suffix == G_DIR_SEPARATOR) {
            suffix++;
        }
        return g_strdup(*suffix == '\0' ? "." : suffix);
    }

    return g_strdup(path);
}

static void source_unit_free(gpointer data)
{
    SourceUnit *unit;

    unit = data;
    if (unit == NULL) {
        return;
    }

    g_free(unit->text);
    g_free(unit->doc);
    g_free(unit);
}

static void doc_file_free(gpointer data)
{
    OdysiaDocFile *doc_file;

    doc_file = data;
    if (doc_file == NULL) {
        return;
    }

    g_free(doc_file->path);
    g_free(doc_file->content);
    g_free(doc_file);
}

static void relation_free(gpointer data)
{
    OdysiaRelation *relation;

    relation = data;
    if (relation == NULL) {
        return;
    }

    g_free(relation->label);
    g_free(relation->target_name);
    g_free(relation->detail);
    g_free(relation);
}

static void symbol_free(gpointer data)
{
    OdysiaSymbol *symbol;

    symbol = data;
    if (symbol == NULL) {
        return;
    }

    g_free(symbol->id);
    g_free(symbol->name);
    g_free(symbol->display_name);
    g_free(symbol->parent_id);
    g_free(symbol->file_path);
    g_free(symbol->signature);
    g_free(symbol->type_text);
    g_free(symbol->documentation);
    g_free(symbol->snippet);
    if (symbol->children != NULL) {
        g_ptr_array_free(symbol->children, TRUE);
    }
    if (symbol->relations != NULL) {
        g_ptr_array_free(symbol->relations, TRUE);
    }
    g_free(symbol);
}

const gchar *odysia_symbol_kind_name(OdysiaSymbolKind kind)
{
    switch (kind) {
    case ODYSIA_SYMBOL_FUNCTION:
        return "Function";
    case ODYSIA_SYMBOL_STRUCT:
        return "Struct";
    case ODYSIA_SYMBOL_UNION:
        return "Union";
    case ODYSIA_SYMBOL_ENUM:
        return "Enum";
    case ODYSIA_SYMBOL_ENUMERATOR:
        return "Enumerator";
    case ODYSIA_SYMBOL_TYPEDEF:
        return "Typedef";
    case ODYSIA_SYMBOL_GLOBAL_VARIABLE:
        return "Global Variable";
    case ODYSIA_SYMBOL_FIELD:
        return "Field";
    case ODYSIA_SYMBOL_PARAMETER:
        return "Parameter";
    case ODYSIA_SYMBOL_LOCAL_VARIABLE:
        return "Local Variable";
    case ODYSIA_SYMBOL_MODULE:
        return "Module";
    case ODYSIA_SYMBOL_CLASS:
        return "Class";
    case ODYSIA_SYMBOL_TRAIT:
        return "Trait";
    case ODYSIA_SYMBOL_MACRO:
        return "Macro";
    case ODYSIA_SYMBOL_LABEL:
        return "Label";
    case ODYSIA_SYMBOL_CONFIG:
        return "Configuration";
    case ODYSIA_SYMBOL_BUILD_TARGET:
        return "Build Target";
    case ODYSIA_SYMBOL_DEVICE_NODE:
        return "Device Tree Node";
    case ODYSIA_SYMBOL_RULE:
        return "Rule";
    default:
        return "Symbol";
    }
}

const gchar *odysia_symbol_language_name(const OdysiaSymbol *symbol)
{
    const gchar *base_name;
    const gchar *backslash;

    if (symbol == NULL || symbol->file_path == NULL) {
        return "Other";
    }

    base_name = strrchr(symbol->file_path, '/');
    backslash = strrchr(symbol->file_path, '\\');
    if (backslash != NULL && (base_name == NULL || backslash > base_name)) {
        base_name = backslash;
    }
    base_name = base_name != NULL ? base_name + 1 : symbol->file_path;

    if (g_str_has_suffix(base_name, ".lds.S") || g_str_has_suffix(base_name, ".lds")) {
        return "Linker Script";
    }
    if (g_str_has_suffix(base_name, ".c") || g_str_has_suffix(base_name, ".h")) {
        return "C";
    }
    if (g_str_has_suffix(base_name, ".rs")) {
        return "Rust";
    }
    if (g_str_has_suffix(base_name, ".S") || g_str_has_suffix(base_name, ".s") ||
        g_str_has_suffix(base_name, ".asm")) {
        return "Assembly";
    }
    if (g_str_has_suffix(base_name, ".py")) {
        return "Python";
    }
    if (g_str_has_suffix(base_name, ".sh")) {
        return "Shell";
    }
    if (g_str_has_suffix(base_name, ".pl") || g_str_has_suffix(base_name, ".pm")) {
        return "Perl";
    }
    if (g_str_has_suffix(base_name, ".awk")) {
        return "AWK";
    }
    if (g_str_has_suffix(base_name, ".dts") || g_str_has_suffix(base_name, ".dtsi")) {
        return "Device Tree";
    }
    if (g_str_has_suffix(base_name, ".cocci")) {
        return "Coccinelle";
    }
    if (g_str_has_suffix(base_name, ".l") || g_str_has_suffix(base_name, ".y")) {
        return "Lex/Yacc";
    }
    if (g_str_has_suffix(base_name, ".mk") || g_str_has_prefix(base_name, "Makefile") ||
        g_str_has_prefix(base_name, "Kbuild")) {
        return "Make/Kbuild";
    }
    if (g_str_has_prefix(base_name, "Kconfig")) {
        return "Kconfig";
    }
    if (g_strcmp0(symbol->type_text, "Python") == 0 ||
        g_strcmp0(symbol->type_text, "Shell") == 0 ||
        g_strcmp0(symbol->type_text, "Perl") == 0 ||
        g_strcmp0(symbol->type_text, "AWK") == 0) {
        return symbol->type_text;
    }
    return "Other";
}

static const gchar *relation_kind_name(OdysiaRelationKind kind)
{
    switch (kind) {
    case ODYSIA_RELATION_CALL:
        return "call";
    case ODYSIA_RELATION_TYPE:
        return "type";
    case ODYSIA_RELATION_ALIAS:
        return "alias";
    case ODYSIA_RELATION_KEYWORD:
        return "keyword";
    default:
        return "type";
    }
}

static OdysiaRelationKind relation_kind_from_name(const gchar *name)
{
    if (g_strcmp0(name, "call") == 0) {
        return ODYSIA_RELATION_CALL;
    }
    if (g_strcmp0(name, "alias") == 0) {
        return ODYSIA_RELATION_ALIAS;
    }
    if (g_strcmp0(name, "keyword") == 0) {
        return ODYSIA_RELATION_KEYWORD;
    }
    return ODYSIA_RELATION_TYPE;
}

static OdysiaSymbolKind symbol_kind_from_name(const gchar *name)
{
    if (g_strcmp0(name, "Function") == 0) {
        return ODYSIA_SYMBOL_FUNCTION;
    }
    if (g_strcmp0(name, "Struct") == 0) {
        return ODYSIA_SYMBOL_STRUCT;
    }
    if (g_strcmp0(name, "Union") == 0) {
        return ODYSIA_SYMBOL_UNION;
    }
    if (g_strcmp0(name, "Enum") == 0) {
        return ODYSIA_SYMBOL_ENUM;
    }
    if (g_strcmp0(name, "Enumerator") == 0) {
        return ODYSIA_SYMBOL_ENUMERATOR;
    }
    if (g_strcmp0(name, "Typedef") == 0) {
        return ODYSIA_SYMBOL_TYPEDEF;
    }
    if (g_strcmp0(name, "Global Variable") == 0) {
        return ODYSIA_SYMBOL_GLOBAL_VARIABLE;
    }
    if (g_strcmp0(name, "Field") == 0) {
        return ODYSIA_SYMBOL_FIELD;
    }
    if (g_strcmp0(name, "Parameter") == 0) {
        return ODYSIA_SYMBOL_PARAMETER;
    }
    if (g_strcmp0(name, "Local Variable") == 0) {
        return ODYSIA_SYMBOL_LOCAL_VARIABLE;
    }
    if (g_strcmp0(name, "Module") == 0) {
        return ODYSIA_SYMBOL_MODULE;
    }
    if (g_strcmp0(name, "Class") == 0) {
        return ODYSIA_SYMBOL_CLASS;
    }
    if (g_strcmp0(name, "Trait") == 0) {
        return ODYSIA_SYMBOL_TRAIT;
    }
    if (g_strcmp0(name, "Macro") == 0) {
        return ODYSIA_SYMBOL_MACRO;
    }
    if (g_strcmp0(name, "Label") == 0) {
        return ODYSIA_SYMBOL_LABEL;
    }
    if (g_strcmp0(name, "Configuration") == 0) {
        return ODYSIA_SYMBOL_CONFIG;
    }
    if (g_strcmp0(name, "Build Target") == 0) {
        return ODYSIA_SYMBOL_BUILD_TARGET;
    }
    if (g_strcmp0(name, "Device Tree Node") == 0) {
        return ODYSIA_SYMBOL_DEVICE_NODE;
    }
    if (g_strcmp0(name, "Rule") == 0) {
        return ODYSIA_SYMBOL_RULE;
    }
    return ODYSIA_SYMBOL_GLOBAL_VARIABLE;
}

static gchar *normalize_space(const gchar *text)
{
    GString *buffer;
    gboolean seen_space;
    const gchar *cursor;

    buffer = g_string_new(NULL);
    seen_space = FALSE;
    for (cursor = text; *cursor != '\0'; cursor++) {
        if (g_ascii_isspace(*cursor)) {
            if (!seen_space) {
                g_string_append_c(buffer, ' ');
                seen_space = TRUE;
            }
        } else {
            g_string_append_c(buffer, *cursor);
            seen_space = FALSE;
        }
    }

    return g_strstrip(g_string_free(buffer, FALSE));
}

static gsize skip_balanced_parens(const gchar *text, gsize open_index)
{
    gint depth;
    gsize index;

    depth = 0;
    for (index = open_index; text[index] != '\0'; index++) {
        if (text[index] == '(') {
            depth++;
        } else if (text[index] == ')') {
            depth--;
            if (depth == 0) {
                return index;
            }
        }
    }

    return open_index;
}

static void append_separator_if_needed(GString *buffer)
{
    if (buffer->len > 0 && !g_ascii_isspace(buffer->str[buffer->len - 1])) {
        g_string_append_c(buffer, ' ');
    }
}

static gchar *sanitize_declaration_text(const gchar *text)
{
    GString *buffer;
    gsize index;

    buffer = g_string_new(NULL);
    for (index = 0; text[index] != '\0'; index++) {
        if (is_ident_start(text[index])) {
            gsize start;
            gsize end;
            gchar *token;
            gsize lookahead;

            start = index;
            end = index + 1;
            while (is_ident_char(text[end])) {
                end++;
            }
            token = g_strndup(text + start, end - start);
            lookahead = end;
            while (g_ascii_isspace(text[lookahead])) {
                lookahead++;
            }

            if (is_decorator_keyword(token) ||
                (g_str_equal(token, "asm") && text[lookahead] == '(')) {
                append_separator_if_needed(buffer);
                if (text[lookahead] == '(') {
                    index = skip_balanced_parens(text, lookahead);
                } else {
                    index = end - 1;
                }
                g_free(token);
                continue;
            }

            g_string_append_len(buffer, text + start, end - start);
            g_free(token);
            index = end - 1;
            continue;
        }

        g_string_append_c(buffer, text[index]);
    }

    return g_string_free(buffer, FALSE);
}

static gchar *prepare_parsed_text(const gchar *text)
{
    gchar *sanitized;
    gchar *decorated;
    gchar *normalized;

    sanitized = strip_comments_and_literals(text);
    decorated = sanitize_declaration_text(sanitized);
    normalized = normalize_space(decorated);
    g_free(decorated);
    g_free(sanitized);
    return normalized;
}

static gchar *strip_comments_and_literals(const gchar *text)
{
    GString *buffer;
    gboolean in_line_comment;
    gboolean in_block_comment;
    gboolean in_string;
    gboolean in_char;
    gboolean escaped;
    gsize index;

    buffer = g_string_new(NULL);
    in_line_comment = FALSE;
    in_block_comment = FALSE;
    in_string = FALSE;
    in_char = FALSE;
    escaped = FALSE;

    for (index = 0; text[index] != '\0'; index++) {
        gchar current;
        gchar next;

        current = text[index];
        next = text[index + 1];

        if (in_line_comment) {
            g_string_append_c(buffer, current == '\n' ? '\n' : ' ');
            if (current == '\n') {
                in_line_comment = FALSE;
            }
            continue;
        }

        if (in_block_comment) {
            g_string_append_c(buffer, current == '\n' ? '\n' : ' ');
            if (current == '*' && next == '/') {
                g_string_append_c(buffer, ' ');
                in_block_comment = FALSE;
                index++;
            }
            continue;
        }

        if (in_string) {
            g_string_append_c(buffer, current == '\n' ? '\n' : ' ');
            if (!escaped && current == '"') {
                in_string = FALSE;
            }
            escaped = (!escaped && current == '\\');
            if (current != '\\') {
                escaped = FALSE;
            }
            continue;
        }

        if (in_char) {
            g_string_append_c(buffer, current == '\n' ? '\n' : ' ');
            if (!escaped && current == '\'') {
                in_char = FALSE;
            }
            escaped = (!escaped && current == '\\');
            if (current != '\\') {
                escaped = FALSE;
            }
            continue;
        }

        if (current == '/' && next == '/') {
            g_string_append(buffer, "  ");
            in_line_comment = TRUE;
            index++;
            continue;
        }
        if (current == '/' && next == '*') {
            g_string_append(buffer, "  ");
            in_block_comment = TRUE;
            index++;
            continue;
        }
        if (current == '"') {
            g_string_append_c(buffer, ' ');
            in_string = TRUE;
            escaped = FALSE;
            continue;
        }
        if (current == '\'') {
            g_string_append_c(buffer, ' ');
            in_char = TRUE;
            escaped = FALSE;
            continue;
        }

        g_string_append_c(buffer, current);
    }

    return g_string_free(buffer, FALSE);
}

static gchar *clean_doc_comment(const gchar *text)
{
    gchar **lines;
    GString *buffer;
    guint index;

    lines = g_strsplit(text, "\n", -1);
    buffer = g_string_new(NULL);

    for (index = 0; lines[index] != NULL; index++) {
        gchar *line;

        line = trim_copy(lines[index]);
        if (g_str_has_prefix(line, "/**")) {
            memmove(line, line + 3, strlen(line + 3) + 1);
        } else if (g_str_has_prefix(line, "/*")) {
            memmove(line, line + 2, strlen(line + 2) + 1);
        }
        g_strstrip(line);
        if (g_str_has_prefix(line, "*")) {
            memmove(line, line + 1, strlen(line + 1) + 1);
            g_strstrip(line);
        }
        if (g_str_has_suffix(line, "*/")) {
            line[strlen(line) - 2] = '\0';
            g_strstrip(line);
        }
        if (line[0] != '\0') {
            if (buffer->len > 0) {
                g_string_append_c(buffer, '\n');
            }
            g_string_append(buffer, line);
        }
        g_free(line);
    }

    g_strfreev(lines);
    return g_string_free(buffer, FALSE);
}

static void collect_files_recursive(const gchar *directory,
                                    GPtrArray *source_files,
                                    GPtrArray *doc_files,
                                    OdysiaIndexProgressFunc progress_func,
                                    gpointer user_data,
                                    GCancellable *cancellable,
                                    guint *discovered_count,
                                    GError **error)
{
    GDir *dir;
    const gchar *name;

    dir = g_dir_open(directory, 0, error);
    if (dir == NULL) {
        return;
    }

    while ((name = g_dir_read_name(dir)) != NULL) {
        gchar *path;

        if (cancellable != NULL && g_cancellable_is_cancelled(cancellable)) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Indexing was cancelled.");
            break;
        }

        if (g_str_equal(name, ".git") || g_str_equal(name, "build") || g_str_equal(name, "target")) {
            continue;
        }

        path = g_build_filename(directory, name, NULL);
        if (g_file_test(path, G_FILE_TEST_IS_DIR)) {
            collect_files_recursive(path,
                                    source_files,
                                    doc_files,
                                    progress_func,
                                    user_data,
                                    cancellable,
                                    discovered_count,
                                    error);
            g_free(path);
            if (error != NULL && *error != NULL) {
                break;
            }
            continue;
        }

        if (has_source_extension(name) || has_supported_shebang(path)) {
            g_ptr_array_add(source_files, path);
            if (discovered_count != NULL) {
                (*discovered_count)++;
                if (progress_func != NULL) {
                    progress_func(ODYSIA_INDEX_STAGE_DISCOVER, *discovered_count, 0, path, user_data);
                }
            }
            continue;
        }
        if (has_doc_extension(name)) {
            g_ptr_array_add(doc_files, path);
            if (discovered_count != NULL) {
                (*discovered_count)++;
                if (progress_func != NULL) {
                    progress_func(ODYSIA_INDEX_STAGE_DISCOVER, *discovered_count, 0, path, user_data);
                }
            }
            continue;
        }

        g_free(path);
    }

    g_dir_close(dir);
}

static SourceUnit *source_unit_new(const gchar *text, const gchar *doc, gint line)
{
    SourceUnit *unit;

    unit = g_new0(SourceUnit, 1);
    unit->text = g_strdup(text);
    unit->doc = doc != NULL ? g_strdup(doc) : NULL;
    unit->line = line;
    return unit;
}

static gsize skip_preprocessor_directive(const gchar *content, gsize index, gint *line)
{
    gsize cursor;

    cursor = index;
    while (content[cursor] != '\0') {
        if (content[cursor] == '\n') {
            (*line)++;
            if (cursor > index && content[cursor - 1] == '\\') {
                cursor++;
                continue;
            }
            return cursor;
        }
        cursor++;
    }

    return cursor;
}

static GPtrArray *collect_top_level_units(const gchar *content)
{
    GPtrArray *units;
    GString *buffer;
    gchar *pending_doc;
    gboolean collecting;
    gboolean in_line_comment;
    gboolean in_block_comment;
    gboolean in_string;
    gboolean in_char;
    gboolean escaped;
    gint brace_depth;
    gint paren_depth;
    gint line;
    gint unit_line;
    gsize index;

    units = g_ptr_array_new_with_free_func(source_unit_free);
    buffer = g_string_new(NULL);
    pending_doc = NULL;
    collecting = FALSE;
    in_line_comment = FALSE;
    in_block_comment = FALSE;
    in_string = FALSE;
    in_char = FALSE;
    escaped = FALSE;
    brace_depth = 0;
    paren_depth = 0;
    line = 1;
    unit_line = 1;

    for (index = 0; content[index] != '\0'; index++) {
        gchar current;
        gchar next;

        current = content[index];
        next = content[index + 1];

        if (!collecting && current == '/' && next == '*' && content[index + 2] == '*') {
            GString *comment;

            comment = g_string_new("/**");
            index += 3;
            while (content[index] != '\0') {
                if (content[index] == '*' && content[index + 1] == '/') {
                    g_string_append(comment, "*/");
                    index++;
                    break;
                }
                g_string_append_c(comment, content[index]);
                if (content[index] == '\n') {
                    line++;
                }
                index++;
            }
            g_free(pending_doc);
            pending_doc = clean_doc_comment(comment->str);
            g_string_free(comment, TRUE);
            continue;
        }

        if (in_line_comment) {
            if (collecting) {
                g_string_append_c(buffer, current);
            }
            if (current == '\n') {
                in_line_comment = FALSE;
                line++;
            }
            continue;
        }
        if (in_block_comment) {
            if (collecting) {
                g_string_append_c(buffer, current);
            }
            if (current == '*' && next == '/') {
                if (collecting) {
                    g_string_append_c(buffer, next);
                }
                in_block_comment = FALSE;
                index++;
            }
            if (current == '\n') {
                line++;
            }
            continue;
        }

        if (!collecting) {
            if (current == '/' && next == '/') {
                in_line_comment = TRUE;
                index++;
                continue;
            }
            if (current == '/' && next == '*') {
                in_block_comment = TRUE;
                index++;
                continue;
            }
            if (current == '\n') {
                line++;
                continue;
            }
            if (g_ascii_isspace(current)) {
                continue;
            }
            if (current == '#') {
                index = skip_preprocessor_directive(content, index, &line);
                continue;
            }
            collecting = TRUE;
            unit_line = line;
            g_string_truncate(buffer, 0);
        }

        g_string_append_c(buffer, current);

        if (in_string) {
            if (!escaped && current == '"') {
                in_string = FALSE;
            }
            escaped = (!escaped && current == '\\');
            if (current != '\\') {
                escaped = FALSE;
            }
        } else if (in_char) {
            if (!escaped && current == '\'') {
                in_char = FALSE;
            }
            escaped = (!escaped && current == '\\');
            if (current != '\\') {
                escaped = FALSE;
            }
        } else {
            if (current == '"') {
                in_string = TRUE;
            } else if (current == '\'') {
                in_char = TRUE;
            } else if (current == '/' && next == '/') {
                in_line_comment = TRUE;
                g_string_append_c(buffer, next);
                index++;
            } else if (current == '/' && next == '*') {
                in_block_comment = TRUE;
                g_string_append_c(buffer, next);
                index++;
            } else if (current == '(') {
                paren_depth++;
            } else if (current == ')' && paren_depth > 0) {
                paren_depth--;
            } else if (current == '{') {
                brace_depth++;
            } else if (current == '}' && brace_depth > 0) {
                gsize lookahead;

                brace_depth--;
                if (brace_depth == 0) {
                    lookahead = index + 1;
                    while (content[lookahead] != '\0' && g_ascii_isspace(content[lookahead])) {
                        lookahead++;
                    }
                    if (content[lookahead] != ';') {
                        g_ptr_array_add(units, source_unit_new(buffer->str, pending_doc, unit_line));
                        g_free(pending_doc);
                        pending_doc = NULL;
                        collecting = FALSE;
                    }
                }
            } else if (current == ';' && brace_depth == 0 && paren_depth == 0) {
                g_ptr_array_add(units, source_unit_new(buffer->str, pending_doc, unit_line));
                g_free(pending_doc);
                pending_doc = NULL;
                collecting = FALSE;
            }
        }

        if (current == '\n') {
            line++;
        }
    }

    g_string_free(buffer, TRUE);
    g_free(pending_doc);
    return units;
}

static gchar *extract_last_identifier(const gchar *text)
{
    gint end;
    gint start;

    end = (gint) strlen(text) - 1;
    while (end >= 0 && !is_ident_char(text[end])) {
        end--;
    }
    if (end < 0) {
        return NULL;
    }
    start = end;
    while (start >= 0 && is_ident_char(text[start])) {
        start--;
    }
    if (!is_ident_start(text[start + 1])) {
        return NULL;
    }
    return g_strndup(text + start + 1, end - start);
}

static gboolean contains_function_pointer_pattern(const gchar *text)
{
    gsize index;

    for (index = 0; text[index] != '\0'; index++) {
        if (text[index] == '(') {
            gsize cursor;

            cursor = index + 1;
            while (g_ascii_isspace(text[cursor])) {
                cursor++;
            }
            if (text[cursor] == '*') {
                return TRUE;
            }
        }
    }

    return FALSE;
}

static gchar *extract_function_pointer_name(const gchar *text)
{
    gsize index;

    for (index = 0; text[index] != '\0'; index++) {
        if (text[index] == '(') {
            gsize cursor;

            cursor = index + 1;
            while (g_ascii_isspace(text[cursor])) {
                cursor++;
            }
            while (text[cursor] == '*') {
                cursor++;
                while (g_ascii_isspace(text[cursor])) {
                    cursor++;
                }
            }
            while (is_ident_start(text[cursor])) {
                gsize token_start;
                gsize token_end;
                gchar *token;

                token_start = cursor;
                token_end = cursor + 1;
                while (is_ident_char(text[token_end])) {
                    token_end++;
                }
                token = g_strndup(text + token_start, token_end - token_start);
                if (!is_qualifier_token(token) && !is_decorator_keyword(token)) {
                    return token;
                }
                g_free(token);
                cursor = token_end;
                while (g_ascii_isspace(text[cursor])) {
                    cursor++;
                }
            }
        }
    }

    return NULL;
}

static gchar *extract_declared_name(const gchar *text)
{
    gchar *function_pointer_name;

    function_pointer_name = extract_function_pointer_name(text);
    if (function_pointer_name != NULL) {
        return function_pointer_name;
    }
    return extract_last_identifier(text);
}

static gchar *extract_type_text_from_part(const gchar *part, const gchar *name)
{
    gchar *name_pos;
    gchar *before;
    gchar *after;
    gchar *combined;
    gchar *normalized;

    name_pos = strstr(part, name);
    if (name_pos == NULL) {
        return trim_copy(part);
    }

    before = g_strndup(part, name_pos - part);
    after = g_strdup(name_pos + strlen(name));
    combined = g_strdup_printf("%s%s", before, after);
    normalized = normalize_space(combined);
    g_free(combined);
    g_free(after);
    g_free(before);
    return normalized;
}

static gint find_last_top_level_paren(const gchar *text)
{
    gint depth;
    gint last;
    gint index;

    depth = 0;
    last = -1;
    for (index = 0; text[index] != '\0'; index++) {
        if (text[index] == '(') {
            if (depth == 0) {
                last = index;
            }
            depth++;
        } else if (text[index] == ')' && depth > 0) {
            depth--;
        }
    }
    return last;
}

static gchar *extract_function_name(const gchar *signature)
{
    const gchar *header_end;
    gint open_index;
    gchar *prefix;
    gchar *name;
    gchar *header_text;

    header_end = strchr(signature, '{');
    if (header_end != NULL) {
        header_text = g_strndup(signature, header_end - signature);
    } else {
        header_text = g_strdup(signature);
    }

    open_index = find_last_top_level_paren(header_text);
    if (open_index < 0) {
        g_free(header_text);
        return NULL;
    }
    prefix = g_strndup(header_text, open_index);
    name = extract_last_identifier(prefix);
    g_free(prefix);
    g_free(header_text);
    return name;
}

static gchar *extract_paren_contents(const gchar *text, gint open_index)
{
    gint depth;
    gint index;

    depth = 0;
    for (index = open_index; text[index] != '\0'; index++) {
        if (text[index] == '(') {
            depth++;
        } else if (text[index] == ')') {
            depth--;
            if (depth == 0) {
                return g_strndup(text + open_index + 1, index - open_index - 1);
            }
        }
    }
    return g_strdup("");
}

gchar *odysia_extract_type_candidate(const gchar *type_text)
{
    gchar **tokens;
    gchar *candidate;
    gint count;
    gint index;

    if (type_text == NULL) {
        return NULL;
    }

    tokens = g_strsplit_set(type_text, " \t\n*[]()", -1);
    candidate = NULL;
    count = 0;
    while (tokens[count] != NULL) {
        count++;
    }
    for (index = count - 1; index >= 0; index--) {
        if (tokens[index][0] == '\0') {
            continue;
        }
        if (g_str_equal(tokens[index], "const") ||
            g_str_equal(tokens[index], "volatile") ||
            g_str_equal(tokens[index], "static") ||
            g_str_equal(tokens[index], "unsigned") ||
            g_str_equal(tokens[index], "signed") ||
            g_str_equal(tokens[index], "long") ||
            g_str_equal(tokens[index], "short") ||
            g_str_equal(tokens[index], "struct") ||
            g_str_equal(tokens[index], "union") ||
            g_str_equal(tokens[index], "enum")) {
            continue;
        }
        candidate = g_strdup(tokens[index]);
        break;
    }
    g_strfreev(tokens);
    return candidate;
}

gboolean odysia_symbol_has_name(const OdysiaSymbol *symbol, const gchar *name)
{
    return symbol != NULL && name != NULL && g_strcmp0(symbol->name, name) == 0;
}

static gchar *build_symbol_id(const gchar *file_path, gint line, OdysiaSymbolKind kind, const gchar *name)
{
    return g_strdup_printf("%s:%d:%s:%s", file_path, line, odysia_symbol_kind_name(kind), name);
}

static OdysiaSymbol *symbol_new(OdysiaSymbolKind kind, const gchar *id, const gchar *name, const gchar *file_path, gint line)
{
    OdysiaSymbol *symbol;

    symbol = g_new0(OdysiaSymbol, 1);
    symbol->id = g_strdup(id);
    symbol->name = g_strdup(name);
    symbol->display_name = g_strdup(name);
    symbol->file_path = g_strdup(file_path);
    symbol->line = line;
    symbol->kind = kind;
    symbol->children = g_ptr_array_new_with_free_func(g_free);
    symbol->relations = g_ptr_array_new_with_free_func(relation_free);
    return symbol;
}

static void index_add_symbol(OdysiaIndex *index, OdysiaSymbol *symbol)
{
    GPtrArray *matches;

    g_ptr_array_add(index->symbols, symbol);
    g_hash_table_insert(index->symbols_by_id, symbol->id, symbol);
    matches = g_hash_table_lookup(index->symbols_by_name, symbol->name);
    if (matches == NULL) {
        matches = g_ptr_array_new();
        g_hash_table_insert(index->symbols_by_name, g_strdup(symbol->name), matches);
    }
    g_ptr_array_add(matches, symbol);
}

static void add_relation(OdysiaSymbol *symbol, OdysiaRelationKind kind, const gchar *label, const gchar *target_name, const gchar *detail)
{
    OdysiaRelation *relation;

    relation = g_new0(OdysiaRelation, 1);
    relation->kind = kind;
    relation->label = g_strdup(label);
    relation->target_name = g_strdup(target_name);
    relation->detail = g_strdup(detail);
    g_ptr_array_add(symbol->relations, relation);
}

static void scan_symbol_keywords(OdysiaSymbol *symbol)
{
    gchar *sanitized;
    GHashTable *seen;
    gsize index;

    if (symbol == NULL || symbol->snippet == NULL || symbol->snippet[0] == '\0') {
        return;
    }

    sanitized = strip_comments_and_literals(symbol->snippet);
    seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    for (index = 0; sanitized[index] != '\0'; index++) {
        if (!is_ident_start(sanitized[index])) {
            continue;
        }

        {
            gsize start;
            gsize end;
            gchar *token;
            guint keyword_index;

            start = index;
            end = index + 1;
            while (is_ident_char(sanitized[end])) {
                end++;
            }

            token = g_strndup(sanitized + start, end - start);
            for (keyword_index = 0; keyword_index < G_N_ELEMENTS(c99_keywords); keyword_index++) {
                if (g_strcmp0(token, c99_keywords[keyword_index]) == 0 && !g_hash_table_contains(seen, token)) {
                    add_relation(symbol, ODYSIA_RELATION_KEYWORD, "keyword", token, token);
                    g_hash_table_add(seen, g_strdup(token));
                    break;
                }
            }
            for (keyword_index = 0; keyword_index < G_N_ELEMENTS(kernel_keywords); keyword_index++) {
                if (g_strcmp0(token, kernel_keywords[keyword_index]) == 0 && !g_hash_table_contains(seen, token)) {
                    add_relation(symbol, ODYSIA_RELATION_KEYWORD, "kernel keyword", token, token);
                    g_hash_table_add(seen, g_strdup(token));
                    break;
                }
            }

            g_free(token);
            index = end - 1;
        }
    }

    g_hash_table_destroy(seen);
    g_free(sanitized);
}

static OdysiaSymbol *add_child_symbol(OdysiaIndex *index,
                                      OdysiaSymbol *parent,
                                      OdysiaSymbolKind kind,
                                      const gchar *name,
                                      const gchar *type_text,
                                      const gchar *signature)
{
    gchar *symbol_id;
    OdysiaSymbol *child;
    gchar *type_candidate;

    symbol_id = build_symbol_id(parent->file_path, parent->line, kind, name);
    child = symbol_new(kind, symbol_id, name, parent->file_path, parent->line);
    child->parent_id = g_strdup(parent->id);
    child->type_text = trim_copy(type_text);
    child->signature = trim_copy(signature);
    child->documentation = g_strdup(parent->documentation);
    child->snippet = trim_copy(signature);
    index_add_symbol(index, child);
    g_ptr_array_add(parent->children, g_strdup(child->id));

    type_candidate = odysia_extract_type_candidate(child->type_text);
    if (type_candidate != NULL) {
        add_relation(child, ODYSIA_RELATION_TYPE, "type", type_candidate, child->type_text);
    }
    scan_symbol_keywords(child);
    g_free(type_candidate);
    g_free(symbol_id);
    return child;
}

static gboolean unit_is_function_definition(const gchar *unit_text)
{
    gchar *normalized;
    gchar *name;
    gboolean result;

    normalized = prepare_parsed_text(unit_text);
    result = g_str_has_suffix(normalized, "}") && strchr(normalized, '(') != NULL;
    if (result) {
        name = extract_function_name(normalized);
        result = name != NULL &&
                 !g_str_equal(name, "if") &&
                 !g_str_equal(name, "for") &&
                 !g_str_equal(name, "while") &&
                 !g_str_equal(name, "switch");
        g_free(name);
    }
    g_free(normalized);
    return result;
}

static gboolean unit_is_composite_definition(const gchar *unit_text, const gchar *keyword)
{
    gchar *normalized;
    gboolean result;
    gchar *needle;

    normalized = prepare_parsed_text(unit_text);
    needle = g_strdup_printf("%s ", keyword);
    result = strchr(normalized, '{') != NULL &&
             (g_str_has_prefix(normalized, needle) ||
              (g_str_has_prefix(normalized, "typedef ") && strstr(normalized, needle) != NULL));
    g_free(needle);
    g_free(normalized);
    return result;
}

static gchar *extract_braced_body(const gchar *text)
{
    const gchar *open_brace;
    const gchar *cursor;
    gint depth;

    open_brace = strchr(text, '{');
    if (open_brace == NULL) {
        return g_strdup("");
    }
    depth = 0;
    for (cursor = open_brace; *cursor != '\0'; cursor++) {
        if (*cursor == '{') {
            depth++;
        } else if (*cursor == '}') {
            depth--;
            if (depth == 0) {
                return g_strndup(open_brace + 1, cursor - open_brace - 1);
            }
        }
    }
    return g_strdup("");
}

static GPtrArray *split_top_level_commas(const gchar *text)
{
    GPtrArray *parts;
    GString *current;
    gint paren_depth;
    gint bracket_depth;
    gint brace_depth;
    gsize index;

    parts = g_ptr_array_new_with_free_func(g_free);
    current = g_string_new(NULL);
    paren_depth = 0;
    bracket_depth = 0;
    brace_depth = 0;

    for (index = 0; text[index] != '\0'; index++) {
        gchar ch;

        ch = text[index];
        if (ch == '(') {
            paren_depth++;
        } else if (ch == ')' && paren_depth > 0) {
            paren_depth--;
        } else if (ch == '[') {
            bracket_depth++;
        } else if (ch == ']' && bracket_depth > 0) {
            bracket_depth--;
        } else if (ch == '{') {
            brace_depth++;
        } else if (ch == '}' && brace_depth > 0) {
            brace_depth--;
        }

        if (ch == ',' && paren_depth == 0 && bracket_depth == 0 && brace_depth == 0) {
            g_ptr_array_add(parts, trim_copy(current->str));
            g_string_truncate(current, 0);
        } else {
            g_string_append_c(current, ch);
        }
    }

    if (current->len > 0) {
        g_ptr_array_add(parts, trim_copy(current->str));
    }
    g_string_free(current, TRUE);
    return parts;
}

static gchar *statement_without_initializer(const gchar *text)
{
    gint depth;
    gsize index;

    depth = 0;
    for (index = 0; text[index] != '\0'; index++) {
        if (text[index] == '(' || text[index] == '[' || text[index] == '{') {
            depth++;
        } else if ((text[index] == ')' || text[index] == ']' || text[index] == '}') && depth > 0) {
            depth--;
        } else if (text[index] == '=' && depth == 0) {
            return g_strndup(text, index);
        }
    }
    return g_strdup(text);
}

static void add_declaration_symbols(OdysiaIndex *index,
                                    OdysiaSymbol *parent,
                                    const gchar *declaration,
                                    OdysiaSymbolKind kind)
{
    GPtrArray *parts;
    gchar *first;
    gchar *first_name;
    gchar *first_name_pos;
    gchar *base_type;
    guint index_part;

    parts = split_top_level_commas(declaration);
    if (parts->len == 0) {
        g_ptr_array_free(parts, TRUE);
        return;
    }

    first = g_ptr_array_index(parts, 0);
    first_name = extract_declared_name(first);
    if (first_name == NULL) {
        g_ptr_array_free(parts, TRUE);
        return;
    }
    first_name_pos = strstr(first, first_name);
    if (first_name_pos == NULL) {
        g_free(first_name);
        g_ptr_array_free(parts, TRUE);
        return;
    }
    base_type = extract_type_text_from_part(first, first_name);

    for (index_part = 0; index_part < parts->len; index_part++) {
        gchar *part;
        gchar *name;
        gchar *type_text;

        part = g_ptr_array_index(parts, index_part);
        name = extract_declared_name(part);
        if (name == NULL) {
            continue;
        }
        type_text = index_part == 0 ? extract_type_text_from_part(part, name)
                                    : g_strdup_printf("%s %s", base_type, extract_type_text_from_part(part, name));
        add_child_symbol(index, parent, kind, name, type_text, part);
        g_free(type_text);
        g_free(name);
    }

    g_free(base_type);
    g_free(first_name);
    g_ptr_array_free(parts, TRUE);
}

static gboolean looks_like_declaration(const gchar *statement)
{
    gchar **tokens;
    gboolean result;

    if (statement[0] == '\0') {
        return FALSE;
    }
    if (strchr(statement, '(') != NULL && !contains_function_pointer_pattern(statement)) {
        return FALSE;
    }
    if (g_str_has_prefix(statement, "return") ||
        g_str_has_prefix(statement, "goto") ||
        g_str_has_prefix(statement, "break") ||
        g_str_has_prefix(statement, "continue") ||
        g_str_has_prefix(statement, "if ") ||
        g_str_has_prefix(statement, "for ") ||
        g_str_has_prefix(statement, "while ") ||
        g_str_has_prefix(statement, "switch ")) {
        return FALSE;
    }

    tokens = g_strsplit_set(statement, " \t", 3);
    result = tokens[0] != NULL && tokens[1] != NULL &&
             (is_ident_start(tokens[0][0]) ||
              g_str_equal(tokens[0], "struct") ||
              g_str_equal(tokens[0], "union") ||
              g_str_equal(tokens[0], "enum") ||
              g_str_equal(tokens[0], "const") ||
              g_str_equal(tokens[0], "static") ||
              g_str_equal(tokens[0], "unsigned") ||
              g_str_equal(tokens[0], "signed") ||
              g_str_equal(tokens[0], "long") ||
              g_str_equal(tokens[0], "short"));
    g_strfreev(tokens);
    return result;
}

static void parse_parameters(OdysiaIndex *index, OdysiaSymbol *function_symbol, const gchar *params_text)
{
    GPtrArray *parts;
    guint index_part;

    parts = split_top_level_commas(params_text);
    for (index_part = 0; index_part < parts->len; index_part++) {
        gchar *part;
        gchar *name;
        gchar *type_text;

        part = g_ptr_array_index(parts, index_part);
        if (part[0] == '\0' || g_str_equal(part, "void") || strstr(part, "...") != NULL) {
            continue;
        }
        name = extract_declared_name(part);
        if (name == NULL) {
            continue;
        }
        type_text = extract_type_text_from_part(part, name);
        add_child_symbol(index, function_symbol, ODYSIA_SYMBOL_PARAMETER, name, type_text, part);
        g_free(type_text);
        g_free(name);
    }
    g_ptr_array_free(parts, TRUE);
}

static void parse_calls(OdysiaSymbol *function_symbol, const gchar *body_text)
{
    gchar *sanitized;
    GHashTable *seen;
    gsize index;

    sanitized = strip_comments_and_literals(body_text);
    seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    for (index = 0; sanitized[index] != '\0'; index++) {
        if (!is_ident_start(sanitized[index])) {
            continue;
        }
        {
            gsize start;
            gsize end;
            gchar *name;
            gsize lookahead;

            start = index;
            end = index + 1;
            while (is_ident_char(sanitized[end])) {
                end++;
            }
            name = g_strndup(sanitized + start, end - start);
            lookahead = end;
            while (g_ascii_isspace(sanitized[lookahead])) {
                lookahead++;
            }
            if (sanitized[lookahead] == '(' &&
                !g_str_equal(name, "if") &&
                !g_str_equal(name, "for") &&
                !g_str_equal(name, "while") &&
                !g_str_equal(name, "switch") &&
                !g_str_equal(name, "return") &&
                !g_str_equal(name, "sizeof") &&
                !g_hash_table_contains(seen, name)) {
                add_relation(function_symbol, ODYSIA_RELATION_CALL, "calls", name, name);
                g_hash_table_add(seen, g_strdup(name));
            }
            g_free(name);
            index = end - 1;
        }
    }
    g_hash_table_destroy(seen);
    g_free(sanitized);
}

static void parse_locals(OdysiaIndex *index, OdysiaSymbol *function_symbol, const gchar *body_text)
{
    gchar *sanitized;
    GString *statement;
    gsize offset;

    sanitized = strip_comments_and_literals(body_text);
    statement = g_string_new(NULL);
    for (offset = 0; sanitized[offset] != '\0'; offset++) {
        gchar ch;

        ch = sanitized[offset];
        g_string_append_c(statement, ch);
        if (ch == ';') {
            gchar *normalized;
            gchar *without_init;

            normalized = normalize_space(statement->str);
            if (g_str_has_suffix(normalized, ";")) {
                normalized[strlen(normalized) - 1] = '\0';
                g_strstrip(normalized);
            }
            without_init = statement_without_initializer(normalized);
            if (looks_like_declaration(without_init)) {
                add_declaration_symbols(index, function_symbol, without_init, ODYSIA_SYMBOL_LOCAL_VARIABLE);
            }
            g_free(without_init);
            g_free(normalized);
            g_string_truncate(statement, 0);
        }
    }
    g_string_free(statement, TRUE);
    g_free(sanitized);
}

static void parse_function_unit(OdysiaIndex *index, SourceUnit *unit, const gchar *file_path)
{
    gchar *sanitized;
    gchar *normalized;
    gchar *name;
    gchar *signature;
    gchar *body;
    gchar *symbol_id;
    OdysiaSymbol *symbol;
    gint open_index;
    gchar *params_text;
    gchar *name_pos;
    gchar *return_type;
    gchar *return_candidate;

    sanitized = strip_comments_and_literals(unit->text);
    normalized = normalize_space(sanitized);
    name = extract_function_name(normalized);
    if (name == NULL) {
        g_free(normalized);
        g_free(sanitized);
        return;
    }

    signature = g_strndup(normalized, strchr(normalized, '{') - normalized);
    body = extract_braced_body(unit->text);
    symbol_id = build_symbol_id(file_path, unit->line, ODYSIA_SYMBOL_FUNCTION, name);
    symbol = symbol_new(ODYSIA_SYMBOL_FUNCTION, symbol_id, name, file_path, unit->line);
    symbol->signature = trim_copy(signature);
    symbol->type_text = NULL;
    symbol->documentation = trim_copy(unit->doc);
    symbol->snippet = trim_copy(unit->text);
    index_add_symbol(index, symbol);

    name_pos = strstr(signature, name);
    if (name_pos != NULL) {
        return_type = g_strndup(signature, name_pos - signature);
        symbol->type_text = trim_copy(return_type);
        return_candidate = odysia_extract_type_candidate(symbol->type_text);
        if (return_candidate != NULL) {
            add_relation(symbol, ODYSIA_RELATION_TYPE, "returns", return_candidate, symbol->type_text);
        }
        g_free(return_candidate);
        g_free(return_type);
    }

    open_index = find_last_top_level_paren(signature);
    params_text = extract_paren_contents(signature, open_index);
    parse_parameters(index, symbol, params_text);
    parse_locals(index, symbol, body);
    parse_calls(symbol, body);
    scan_symbol_keywords(symbol);

    g_free(params_text);
    g_free(symbol_id);
    g_free(body);
    g_free(signature);
    g_free(name);
    g_free(normalized);
    g_free(sanitized);
}

static void parse_fields(OdysiaIndex *index, OdysiaSymbol *symbol, const gchar *body_text)
{
    gchar **parts;
    guint index_part;

    parts = g_strsplit(body_text, ";", -1);
    for (index_part = 0; parts[index_part] != NULL; index_part++) {
        gchar *normalized;

        normalized = normalize_space(parts[index_part]);
        if (normalized[0] == '\0' || (strchr(normalized, '(') != NULL && !contains_function_pointer_pattern(normalized))) {
            g_free(normalized);
            continue;
        }
        add_declaration_symbols(index, symbol, normalized, ODYSIA_SYMBOL_FIELD);
        g_free(normalized);
    }
    g_strfreev(parts);
}

static void parse_enum_members(OdysiaIndex *index, OdysiaSymbol *enum_symbol, const gchar *body_text)
{
    GPtrArray *parts;
    guint index_part;

    parts = split_top_level_commas(body_text);
    for (index_part = 0; index_part < parts->len; index_part++) {
        gchar *part;
        gchar *normalized;
        gchar *lhs_text;
        gchar *name;
        gchar *value_pos;
        gchar *value_text;

        part = g_ptr_array_index(parts, index_part);
        normalized = normalize_space(part);
        if (normalized[0] == '\0') {
            g_free(normalized);
            continue;
        }

        value_pos = strchr(normalized, '=');
        lhs_text = value_pos != NULL ? g_strndup(normalized, value_pos - normalized) : g_strdup(normalized);
        name = extract_last_identifier(lhs_text);
        if (name == NULL) {
            g_free(lhs_text);
            g_free(normalized);
            continue;
        }

        value_text = value_pos != NULL ? trim_copy(value_pos + 1) : g_strdup("(implicit)");
        {
            OdysiaSymbol *enumerator_symbol;

            enumerator_symbol = add_child_symbol(index,
                                                 enum_symbol,
                                                 ODYSIA_SYMBOL_ENUMERATOR,
                                                 name,
                                                 "enum value",
                                                 normalized);
            if (value_text != NULL && value_text[0] != '\0') {
                add_relation(enumerator_symbol, ODYSIA_RELATION_ALIAS, "value", value_text, value_text);
            }
        }

        g_free(value_text);
        g_free(name);
        g_free(lhs_text);
        g_free(normalized);
    }
    g_ptr_array_free(parts, TRUE);
}

static void parse_composite_unit(OdysiaIndex *index,
                                 SourceUnit *unit,
                                 const gchar *file_path,
                                 OdysiaSymbolKind kind,
                                 const gchar *keyword)
{
    gchar *normalized;
    gchar *body;
    gchar *name;
    gchar *alias;
    gchar *symbol_id;
    OdysiaSymbol *symbol;
    gchar *open_brace;

    normalized = prepare_parsed_text(unit->text);
    open_brace = strchr(normalized, '{');
    if (open_brace == NULL) {
        g_free(normalized);
        return;
    }

    name = NULL;
    alias = NULL;
    if (g_str_has_prefix(normalized, "typedef ")) {
        gchar *before_brace;
        gchar *after_brace;
        gchar *keyword_pos;

        before_brace = g_strndup(normalized, open_brace - normalized);
        keyword_pos = strstr(before_brace, keyword);
        if (keyword_pos != NULL) {
            name = trim_copy(keyword_pos + strlen(keyword));
        }
        after_brace = g_strdup(strrchr(normalized, '}') + 1);
        alias = extract_last_identifier(after_brace);
        if (name == NULL || name[0] == '\0') {
            g_free(name);
            name = NULL;
            name = g_strdup(alias != NULL ? alias : keyword);
        }
        g_free(after_brace);
        g_free(before_brace);
    } else {
        gchar *before_brace;
        gchar *keyword_pos;

        before_brace = g_strndup(normalized, open_brace - normalized);
        keyword_pos = strstr(before_brace, keyword);
        if (keyword_pos != NULL) {
            name = trim_copy(keyword_pos + strlen(keyword));
        }
        if (name == NULL || name[0] == '\0') {
            g_free(name);
            name = NULL;
            name = g_strdup(keyword);
        }
        g_free(before_brace);
    }

    body = extract_braced_body(unit->text);
    symbol_id = build_symbol_id(file_path, unit->line, kind, name);
    symbol = symbol_new(kind, symbol_id, name, file_path, unit->line);
    symbol->signature = trim_copy(normalized);
    symbol->type_text = g_strdup(keyword);
    symbol->documentation = trim_copy(unit->doc);
    symbol->snippet = trim_copy(unit->text);
    index_add_symbol(index, symbol);
    if (kind == ODYSIA_SYMBOL_ENUM) {
        parse_enum_members(index, symbol, body);
    } else {
        parse_fields(index, symbol, body);
    }

    if (alias != NULL && alias[0] != '\0' && !g_str_equal(alias, name)) {
        add_relation(symbol, ODYSIA_RELATION_ALIAS, "typedef alias", alias, alias);
    }

    scan_symbol_keywords(symbol);

    g_free(symbol_id);
    g_free(alias);
    g_free(name);
    g_free(body);
    g_free(normalized);
}

static void parse_typedef_unit(OdysiaIndex *index, SourceUnit *unit, const gchar *file_path)
{
    gchar *normalized;
    gchar *alias;
    gchar *alias_pos;
    gchar *type_text;
    gchar *symbol_id;
    OdysiaSymbol *symbol;
    gchar *type_candidate;

    normalized = prepare_parsed_text(unit->text);
    if (!g_str_has_prefix(normalized, "typedef ") || strchr(normalized, '{') != NULL) {
        g_free(normalized);
        return;
    }

    alias = extract_declared_name(normalized);
    if (alias == NULL) {
        g_free(normalized);
        return;
    }
    alias_pos = strstr(normalized, alias);
    if (alias_pos == NULL) {
        g_free(alias);
        g_free(normalized);
        return;
    }

    type_text = g_strndup(normalized + strlen("typedef "), alias_pos - (normalized + strlen("typedef ")));
    symbol_id = build_symbol_id(file_path, unit->line, ODYSIA_SYMBOL_TYPEDEF, alias);
    symbol = symbol_new(ODYSIA_SYMBOL_TYPEDEF, symbol_id, alias, file_path, unit->line);
    symbol->signature = trim_copy(normalized);
    symbol->type_text = trim_copy(type_text);
    symbol->documentation = trim_copy(unit->doc);
    symbol->snippet = trim_copy(unit->text);
    index_add_symbol(index, symbol);

    type_candidate = odysia_extract_type_candidate(symbol->type_text);
    if (type_candidate != NULL) {
        add_relation(symbol, ODYSIA_RELATION_ALIAS, "aliases", type_candidate, symbol->type_text);
    }

    scan_symbol_keywords(symbol);

    g_free(type_candidate);
    g_free(symbol_id);
    g_free(type_text);
    g_free(alias);
    g_free(normalized);
}

static void parse_global_variable_unit(OdysiaIndex *index, SourceUnit *unit, const gchar *file_path)
{
    gchar *normalized;
    gchar *without_init;
    gchar *name;
    gchar *name_pos;
    gchar *type_text;
    gchar *symbol_id;
    OdysiaSymbol *symbol;
    gchar *type_candidate;

    normalized = prepare_parsed_text(unit->text);
    if (g_str_has_prefix(normalized, "typedef ") ||
        (strchr(normalized, '(') != NULL && !contains_function_pointer_pattern(normalized))) {
        g_free(normalized);
        return;
    }

    without_init = statement_without_initializer(normalized);
    name = extract_declared_name(without_init);
    if (name == NULL) {
        g_free(without_init);
        g_free(normalized);
        return;
    }
    name_pos = strstr(without_init, name);
    if (name_pos == NULL) {
        g_free(name);
        g_free(without_init);
        g_free(normalized);
        return;
    }

    type_text = extract_type_text_from_part(without_init, name);
    symbol_id = build_symbol_id(file_path, unit->line, ODYSIA_SYMBOL_GLOBAL_VARIABLE, name);
    symbol = symbol_new(ODYSIA_SYMBOL_GLOBAL_VARIABLE, symbol_id, name, file_path, unit->line);
    symbol->signature = trim_copy(normalized);
    symbol->type_text = trim_copy(type_text);
    symbol->documentation = trim_copy(unit->doc);
    symbol->snippet = trim_copy(unit->text);
    index_add_symbol(index, symbol);

    type_candidate = odysia_extract_type_candidate(symbol->type_text);
    if (type_candidate != NULL) {
        add_relation(symbol, ODYSIA_RELATION_TYPE, "type", type_candidate, symbol->type_text);
    }

    scan_symbol_keywords(symbol);

    g_free(type_candidate);
    g_free(symbol_id);
    g_free(type_text);
    g_free(name);
    g_free(without_init);
    g_free(normalized);
}

typedef struct {
    const gchar *pattern;
    OdysiaSymbolKind kind;
    const gchar *language;
} LanguageSymbolRule;

static gint line_number_at_offset(const gchar *content, gint offset)
{
    gint line;
    gint index;

    line = 1;
    for (index = 0; index < offset && content[index] != '\0'; index++) {
        if (content[index] == '\n') {
            line++;
        }
    }
    return line;
}

static gchar *line_at_offset(const gchar *content, gint offset)
{
    const gchar *start;
    const gchar *end;

    start = content + offset;
    while (start > content && start[-1] != '\n') {
        start--;
    }
    end = content + offset;
    while (*end != '\0' && *end != '\n') {
        end++;
    }
    return trim_copy(g_strndup(start, end - start));
}

static void add_language_symbol(OdysiaIndex *index,
                                const gchar *file_path,
                                OdysiaSymbolKind kind,
                                const gchar *name,
                                const gchar *language,
                                const gchar *snippet,
                                gint line)
{
    gchar *symbol_id;
    OdysiaSymbol *symbol;

    if (name == NULL || name[0] == '\0') {
        return;
    }
    symbol_id = build_symbol_id(file_path, line, kind, name);
    if (odysia_index_get_symbol(index, symbol_id) != NULL) {
        g_free(symbol_id);
        return;
    }

    symbol = symbol_new(kind, symbol_id, name, file_path, line);
    symbol->signature = trim_copy(snippet);
    symbol->type_text = g_strdup(language);
    symbol->documentation = g_strdup("");
    symbol->snippet = trim_copy(snippet);
    index_add_symbol(index, symbol);
    g_free(symbol_id);
}

static void scan_language_rules(OdysiaIndex *index,
                                const gchar *content,
                                const gchar *file_path,
                                const LanguageSymbolRule *rules,
                                guint rule_count)
{
    guint rule_index;

    for (rule_index = 0; rule_index < rule_count; rule_index++) {
        GRegex *regex;
        GMatchInfo *match_info;
        GError *regex_error;

        regex_error = NULL;
        regex = g_regex_new(rules[rule_index].pattern,
                            G_REGEX_MULTILINE | G_REGEX_OPTIMIZE,
                            0,
                            &regex_error);
        if (regex == NULL) {
            if (regex_error != NULL) {
                g_error_free(regex_error);
            }
            continue;
        }

        match_info = NULL;
        g_regex_match(regex, content, 0, &match_info);
        while (g_match_info_matches(match_info)) {
            gchar *name;
            gchar *snippet;
            gint start;
            gint end;

            name = g_match_info_fetch(match_info, 1);
            start = 0;
            end = 0;
            g_match_info_fetch_pos(match_info, 0, &start, &end);
            snippet = line_at_offset(content, start);
            add_language_symbol(index,
                                file_path,
                                rules[rule_index].kind,
                                name,
                                rules[rule_index].language,
                                snippet,
                                line_number_at_offset(content, start));
            g_free(snippet);
            g_free(name);
            if (!g_match_info_next(match_info, NULL)) {
                break;
            }
        }
        g_match_info_free(match_info);
        g_regex_unref(regex);
    }
}

static void parse_non_c_source(OdysiaIndex *index,
                               const gchar *path,
                               const gchar *content,
                               const gchar *file_path)
{
    gchar *base_name;
    static const LanguageSymbolRule rust_rules[] = {
        {"^[ \\t]*(?:pub(?:\\([^)]*\\))?[ \\t]+)?(?:unsafe[ \\t]+)?(?:async[ \\t]+)?fn[ \\t]+([A-Za-z_][A-Za-z0-9_]*)", ODYSIA_SYMBOL_FUNCTION, "Rust"},
        {"^[ \\t]*(?:pub(?:\\([^)]*\\))?[ \\t]+)?struct[ \\t]+([A-Za-z_][A-Za-z0-9_]*)", ODYSIA_SYMBOL_STRUCT, "Rust"},
        {"^[ \\t]*(?:pub(?:\\([^)]*\\))?[ \\t]+)?enum[ \\t]+([A-Za-z_][A-Za-z0-9_]*)", ODYSIA_SYMBOL_ENUM, "Rust"},
        {"^[ \\t]*(?:pub(?:\\([^)]*\\))?[ \\t]+)?union[ \\t]+([A-Za-z_][A-Za-z0-9_]*)", ODYSIA_SYMBOL_UNION, "Rust"},
        {"^[ \\t]*(?:pub(?:\\([^)]*\\))?[ \\t]+)?(?:unsafe[ \\t]+)?trait[ \\t]+([A-Za-z_][A-Za-z0-9_]*)", ODYSIA_SYMBOL_TRAIT, "Rust"},
        {"^[ \\t]*(?:pub(?:\\([^)]*\\))?[ \\t]+)?mod[ \\t]+([A-Za-z_][A-Za-z0-9_]*)", ODYSIA_SYMBOL_MODULE, "Rust"},
        {"^[ \\t]*(?:pub(?:\\([^)]*\\))?[ \\t]+)?type[ \\t]+([A-Za-z_][A-Za-z0-9_]*)", ODYSIA_SYMBOL_TYPEDEF, "Rust"},
        {"^[ \\t]*(?:pub(?:\\([^)]*\\))?[ \\t]+)?(?:static|const)[ \\t]+([A-Za-z_][A-Za-z0-9_]*)", ODYSIA_SYMBOL_GLOBAL_VARIABLE, "Rust"},
        {"^[ \\t]*macro_rules![ \\t]*([A-Za-z_][A-Za-z0-9_]*)", ODYSIA_SYMBOL_MACRO, "Rust"}
    };
    static const LanguageSymbolRule assembly_rules[] = {
        {"^[ \\t]*(?:SYM_FUNC_START(?:_LOCAL)?|ENTRY)[ \\t]*\\([ \\t]*([A-Za-z_.$][A-Za-z0-9_.$]*)", ODYSIA_SYMBOL_FUNCTION, "Assembly"},
        {"^[ \\t]*\\.macro[ \\t]+([A-Za-z_.$][A-Za-z0-9_.$]*)", ODYSIA_SYMBOL_MACRO, "Assembly"},
        {"^[ \\t]*([A-Za-z_][A-Za-z0-9_.$]*):", ODYSIA_SYMBOL_LABEL, "Assembly"}
    };
    static const LanguageSymbolRule python_rules[] = {
        {"^[ \\t]*(?:async[ \\t]+)?def[ \\t]+([A-Za-z_][A-Za-z0-9_]*)", ODYSIA_SYMBOL_FUNCTION, "Python"},
        {"^[ \\t]*class[ \\t]+([A-Za-z_][A-Za-z0-9_]*)", ODYSIA_SYMBOL_CLASS, "Python"}
    };
    static const LanguageSymbolRule shell_rules[] = {
        {"^[ \\t]*(?:function[ \\t]+)?([A-Za-z_][A-Za-z0-9_]*)[ \\t]*\\(\\)[ \\t]*\\{?", ODYSIA_SYMBOL_FUNCTION, "Shell"}
    };
    static const LanguageSymbolRule perl_rules[] = {
        {"^[ \\t]*sub[ \\t]+([A-Za-z_][A-Za-z0-9_]*)", ODYSIA_SYMBOL_FUNCTION, "Perl"},
        {"^[ \\t]*package[ \\t]+([A-Za-z_][A-Za-z0-9_:]*)", ODYSIA_SYMBOL_MODULE, "Perl"}
    };
    static const LanguageSymbolRule awk_rules[] = {
        {"^[ \\t]*function[ \\t]+([A-Za-z_][A-Za-z0-9_]*)", ODYSIA_SYMBOL_FUNCTION, "AWK"}
    };
    static const LanguageSymbolRule dts_rules[] = {
        {"^[ \\t]*([A-Za-z_][A-Za-z0-9_-]*)[ \\t]*:[^;\\n]*\\{", ODYSIA_SYMBOL_DEVICE_NODE, "Device Tree"},
        {"^[ \\t]*([A-Za-z_][A-Za-z0-9_,+.-]*)(?:@[A-Fa-f0-9]+)?[ \\t]*\\{", ODYSIA_SYMBOL_DEVICE_NODE, "Device Tree"}
    };
    static const LanguageSymbolRule cocci_rules[] = {
        {"^[ \\t]*@([A-Za-z_][A-Za-z0-9_]*)@", ODYSIA_SYMBOL_RULE, "Coccinelle"}
    };
    static const LanguageSymbolRule grammar_rules[] = {
        {"^([A-Za-z_][A-Za-z0-9_]*)[ \\t]*:", ODYSIA_SYMBOL_RULE, "Lex/Yacc"}
    };
    static const LanguageSymbolRule make_rules[] = {
        {"^([A-Za-z0-9_./%+@-]+)[ \\t]*:(?:[^=]|$)", ODYSIA_SYMBOL_BUILD_TARGET, "Make"},
        {"^[ \\t]*([A-Za-z_][A-Za-z0-9_.-]*)[ \\t]*(?:[:+?]?=)", ODYSIA_SYMBOL_GLOBAL_VARIABLE, "Make"}
    };
    static const LanguageSymbolRule kconfig_rules[] = {
        {"^[ \\t]*(?:menuconfig|config)[ \\t]+([A-Za-z_][A-Za-z0-9_]*)", ODYSIA_SYMBOL_CONFIG, "Kconfig"},
        {"^[ \\t]*menu[ \\t]+\"([^\"]+)\"", ODYSIA_SYMBOL_MODULE, "Kconfig"}
    };
    static const LanguageSymbolRule linker_rules[] = {
        {"^[ \\t]*([.A-Za-z_][.A-Za-z0-9_]*)[ \\t]*:", ODYSIA_SYMBOL_LABEL, "Linker Script"}
    };

    base_name = g_path_get_basename(path);
    if (g_str_has_suffix(path, ".rs")) {
        scan_language_rules(index, content, file_path, rust_rules, G_N_ELEMENTS(rust_rules));
    } else if (g_str_has_suffix(path, ".lds") || g_str_has_suffix(path, ".lds.S")) {
        scan_language_rules(index, content, file_path, linker_rules, G_N_ELEMENTS(linker_rules));
    } else if (g_str_has_suffix(path, ".S") || g_str_has_suffix(path, ".s") || g_str_has_suffix(path, ".asm")) {
        scan_language_rules(index, content, file_path, assembly_rules, G_N_ELEMENTS(assembly_rules));
    } else if (g_str_has_suffix(path, ".py") || (g_str_has_prefix(content, "#!") && strstr(content, "python") != NULL)) {
        scan_language_rules(index, content, file_path, python_rules, G_N_ELEMENTS(python_rules));
    } else if (g_str_has_suffix(path, ".sh") || (g_str_has_prefix(content, "#!") && strstr(content, "sh") != NULL)) {
        scan_language_rules(index, content, file_path, shell_rules, G_N_ELEMENTS(shell_rules));
    } else if (g_str_has_suffix(path, ".pl") || g_str_has_suffix(path, ".pm") ||
               (g_str_has_prefix(content, "#!") && strstr(content, "perl") != NULL)) {
        scan_language_rules(index, content, file_path, perl_rules, G_N_ELEMENTS(perl_rules));
    } else if (g_str_has_suffix(path, ".awk") || (g_str_has_prefix(content, "#!") && strstr(content, "awk") != NULL)) {
        scan_language_rules(index, content, file_path, awk_rules, G_N_ELEMENTS(awk_rules));
    } else if (g_str_has_suffix(path, ".dts") || g_str_has_suffix(path, ".dtsi")) {
        scan_language_rules(index, content, file_path, dts_rules, G_N_ELEMENTS(dts_rules));
    } else if (g_str_has_suffix(path, ".cocci")) {
        scan_language_rules(index, content, file_path, cocci_rules, G_N_ELEMENTS(cocci_rules));
    } else if (g_str_has_suffix(path, ".l") || g_str_has_suffix(path, ".y")) {
        scan_language_rules(index, content, file_path, grammar_rules, G_N_ELEMENTS(grammar_rules));
    } else if (g_str_has_prefix(base_name, "Makefile") || g_str_has_prefix(base_name, "Kbuild") || g_str_has_suffix(path, ".mk")) {
        scan_language_rules(index, content, file_path, make_rules, G_N_ELEMENTS(make_rules));
    } else if (g_str_has_prefix(base_name, "Kconfig")) {
        scan_language_rules(index, content, file_path, kconfig_rules, G_N_ELEMENTS(kconfig_rules));
    }
    g_free(base_name);
}

static void parse_source_file(OdysiaIndex *index, const gchar *path)
{
    gchar *content;
    GPtrArray *units;
    gchar *file_path;
    guint index_unit;

    if (!g_file_get_contents(path, &content, NULL, NULL)) {
        return;
    }

    file_path = relative_path(index->root_path, path);
    if (!g_str_has_suffix(path, ".c") && !g_str_has_suffix(path, ".h")) {
        parse_non_c_source(index, path, content, file_path);
        g_free(file_path);
        g_free(content);
        return;
    }
    units = collect_top_level_units(content);
    for (index_unit = 0; index_unit < units->len; index_unit++) {
        SourceUnit *unit;
        gchar *normalized;

        unit = g_ptr_array_index(units, index_unit);
        if (unit_is_function_definition(unit->text)) {
            parse_function_unit(index, unit, file_path);
            continue;
        }
        if (unit_is_composite_definition(unit->text, "struct")) {
            parse_composite_unit(index, unit, file_path, ODYSIA_SYMBOL_STRUCT, "struct");
            continue;
        }
        if (unit_is_composite_definition(unit->text, "union")) {
            parse_composite_unit(index, unit, file_path, ODYSIA_SYMBOL_UNION, "union");
            continue;
        }
        if (unit_is_composite_definition(unit->text, "enum")) {
            parse_composite_unit(index, unit, file_path, ODYSIA_SYMBOL_ENUM, "enum");
            continue;
        }
        normalized = prepare_parsed_text(unit->text);
        if (g_str_has_prefix(normalized, "typedef ")) {
            parse_typedef_unit(index, unit, file_path);
        } else {
            parse_global_variable_unit(index, unit, file_path);
        }
        g_free(normalized);
    }

    g_ptr_array_free(units, TRUE);
    g_free(file_path);
    g_free(content);
}

static void parse_doc_file(OdysiaIndex *index, const gchar *path)
{
    gchar *content;
    OdysiaDocFile *doc_file;

    if (!g_file_get_contents(path, &content, NULL, NULL)) {
        return;
    }

    doc_file = g_new0(OdysiaDocFile, 1);
    doc_file->path = relative_path(index->root_path, path);
    doc_file->content = content;
    g_ptr_array_add(index->doc_files, doc_file);
}

static OdysiaIndex *index_new_empty(const gchar *root_path)
{
    OdysiaIndex *index;

    index = g_new0(OdysiaIndex, 1);
    index->root_path = g_strdup(root_path);
    index->symbols = g_ptr_array_new_with_free_func(symbol_free);
    index->symbols_by_id = g_hash_table_new(g_str_hash, g_str_equal);
    index->symbols_by_name = g_hash_table_new_full(g_str_hash,
                                                   g_str_equal,
                                                   g_free,
                                                   (GDestroyNotify) g_ptr_array_unref);
    index->doc_files = g_ptr_array_new_with_free_func(doc_file_free);
    return index;
}

static void merge_partial_index(OdysiaIndex *target, OdysiaIndex *partial)
{
    guint symbol_index;

    for (symbol_index = 0; symbol_index < partial->symbols->len; symbol_index++) {
        index_add_symbol(target, g_ptr_array_index(partial->symbols, symbol_index));
    }
    g_ptr_array_set_free_func(partial->symbols, NULL);
}

static void parse_source_task(gpointer data, gpointer user_data)
{
    SourceParseTask *task;
    SourceParseContext *context;
    OdysiaIndex *partial;

    (void) user_data;
    task = data;
    context = task->context;
    if (context->cancellable != NULL && g_cancellable_is_cancelled(context->cancellable)) {
        g_free(task);
        return;
    }

    partial = index_new_empty(context->index->root_path);
    parse_source_file(partial, task->path);

    g_mutex_lock(&context->mutex);
    if (context->cancellable == NULL || !g_cancellable_is_cancelled(context->cancellable)) {
        merge_partial_index(context->index, partial);
        context->completed_files++;
        if (context->progress_func != NULL) {
            context->progress_func(ODYSIA_INDEX_STAGE_PARSE_SOURCE,
                                   context->completed_files,
                                   context->total_files,
                                   task->path,
                                   context->progress_user_data);
        }
    }
    g_mutex_unlock(&context->mutex);

    odysia_index_free(partial);
    g_free(task);
}

OdysiaIndex *odysia_index_build(const gchar *root_path, GError **error)
{
    return odysia_index_build_with_progress(root_path,
                                            NULL,
                                            NULL,
                                            NULL,
                                            MAX(1, g_get_num_processors()),
                                            error);
}

OdysiaIndex *odysia_index_build_with_progress(const gchar *root_path,
                                              OdysiaIndexProgressFunc progress_func,
                                              gpointer user_data,
                                              GCancellable *cancellable,
                                              guint source_thread_count,
                                              GError **error)
{
    OdysiaIndex *index;
    GPtrArray *source_files;
    GPtrArray *doc_files;
    guint array_index;
    guint discovered_count;

    {
        gchar *canonical_root;

        canonical_root = g_canonicalize_filename(root_path, NULL);
        index = index_new_empty(canonical_root);
        g_free(canonical_root);
    }

    source_files = g_ptr_array_new_with_free_func(g_free);
    doc_files = g_ptr_array_new_with_free_func(g_free);
    discovered_count = 0;
    if (progress_func != NULL) {
        progress_func(ODYSIA_INDEX_STAGE_DISCOVER, 0, 0, NULL, user_data);
    }
    collect_files_recursive(index->root_path,
                            source_files,
                            doc_files,
                            progress_func,
                            user_data,
                            cancellable,
                            &discovered_count,
                            error);
    if (error != NULL && *error != NULL) {
        g_ptr_array_free(source_files, TRUE);
        g_ptr_array_free(doc_files, TRUE);
        odysia_index_free(index);
        return NULL;
    }
    if (cancellable != NULL && g_cancellable_is_cancelled(cancellable)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Indexing was cancelled.");
        g_ptr_array_free(source_files, TRUE);
        g_ptr_array_free(doc_files, TRUE);
        odysia_index_free(index);
        return NULL;
    }

    if (progress_func != NULL) {
        progress_func(ODYSIA_INDEX_STAGE_DISCOVER, discovered_count, discovered_count, NULL, user_data);
        progress_func(ODYSIA_INDEX_STAGE_PARSE_SOURCE, 0, source_files->len, NULL, user_data);
    }

    if (source_files->len > 0) {
        SourceParseContext context;
        GThreadPool *pool;

        context.index = index;
        context.progress_func = progress_func;
        context.progress_user_data = user_data;
        context.cancellable = cancellable;
        context.completed_files = 0;
        context.total_files = source_files->len;
        g_mutex_init(&context.mutex);

        pool = g_thread_pool_new(parse_source_task,
                                 NULL,
                                 (gint) MAX(1, source_thread_count),
                                 FALSE,
                                 error);
        if (pool == NULL) {
            g_mutex_clear(&context.mutex);
            g_ptr_array_free(source_files, TRUE);
            g_ptr_array_free(doc_files, TRUE);
            odysia_index_free(index);
            return NULL;
        }

        for (array_index = 0; array_index < source_files->len; array_index++) {
            SourceParseTask *task;

            task = g_new0(SourceParseTask, 1);
            task->context = &context;
            task->path = g_ptr_array_index(source_files, array_index);
            if (!g_thread_pool_push(pool, task, error)) {
                g_free(task);
                break;
            }
        }
        g_thread_pool_free(pool, FALSE, TRUE);
        g_mutex_clear(&context.mutex);

        if (error != NULL && *error != NULL) {
            g_ptr_array_free(source_files, TRUE);
            g_ptr_array_free(doc_files, TRUE);
            odysia_index_free(index);
            return NULL;
        }
        if (cancellable != NULL && g_cancellable_is_cancelled(cancellable)) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Indexing was cancelled.");
            g_ptr_array_free(source_files, TRUE);
            g_ptr_array_free(doc_files, TRUE);
            odysia_index_free(index);
            return NULL;
        }
    }
    if (progress_func != NULL) {
        progress_func(ODYSIA_INDEX_STAGE_PARSE_DOCS, 0, doc_files->len, NULL, user_data);
    }
    for (array_index = 0; array_index < doc_files->len; array_index++) {
        const gchar *path;

        if (cancellable != NULL && g_cancellable_is_cancelled(cancellable)) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Indexing was cancelled.");
            g_ptr_array_free(source_files, TRUE);
            g_ptr_array_free(doc_files, TRUE);
            odysia_index_free(index);
            return NULL;
        }

        path = g_ptr_array_index(doc_files, array_index);
        parse_doc_file(index, path);
        if (progress_func != NULL) {
            progress_func(ODYSIA_INDEX_STAGE_PARSE_DOCS, array_index + 1, doc_files->len, path, user_data);
        }
    }

    g_ptr_array_free(source_files, TRUE);
    g_ptr_array_free(doc_files, TRUE);
    return index;
}

static gboolean sqlite_exec(sqlite3 *db, const gchar *sql, GError **error)
{
    char *message;
    gint rc;

    message = NULL;
    rc = sqlite3_exec(db, sql, NULL, NULL, &message);
    if (rc == SQLITE_OK) {
        return TRUE;
    }

    g_set_error(error,
                G_FILE_ERROR,
                G_FILE_ERROR_FAILED,
                "SQLite error: %s",
                message != NULL ? message : sqlite3_errmsg(db));
    sqlite3_free(message);
    return FALSE;
}

static gboolean sqlite_prepare(sqlite3 *db, sqlite3_stmt **stmt, const gchar *sql, GError **error)
{
    gint rc;

    rc = sqlite3_prepare_v2(db, sql, -1, stmt, NULL);
    if (rc == SQLITE_OK) {
        return TRUE;
    }

    g_set_error(error,
                G_FILE_ERROR,
                G_FILE_ERROR_FAILED,
                "SQLite prepare failed: %s",
                sqlite3_errmsg(db));
    return FALSE;
}

static void sqlite_bind_text_or_null(sqlite3_stmt *stmt, gint column, const gchar *text)
{
    if (text == NULL) {
        sqlite3_bind_null(stmt, column);
        return;
    }
    sqlite3_bind_text(stmt, column, text, -1, SQLITE_TRANSIENT);
}

gboolean odysia_index_save_sqlite(const OdysiaIndex *index, const gchar *sqlite_path, GError **error)
{
    sqlite3 *db;
    sqlite3_stmt *stmt;
    guint symbol_index;
    guint doc_index;
    gint rc;

    if (index == NULL || sqlite_path == NULL || sqlite_path[0] == '\0') {
        g_set_error_literal(error, G_FILE_ERROR, G_FILE_ERROR_INVAL, "Index and sqlite path are required.");
        return FALSE;
    }

    db = NULL;
    rc = sqlite3_open(sqlite_path, &db);
    if (rc != SQLITE_OK) {
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_FAILED, "Unable to open SQLite file: %s", sqlite3_errmsg(db));
        if (db != NULL) {
            sqlite3_close(db);
        }
        return FALSE;
    }

    if (!sqlite_exec(db, "PRAGMA foreign_keys=OFF;", error) ||
        !sqlite_exec(db, "BEGIN IMMEDIATE;", error) ||
        !sqlite_exec(db, "DROP TABLE IF EXISTS meta;", error) ||
        !sqlite_exec(db, "DROP TABLE IF EXISTS symbols;", error) ||
        !sqlite_exec(db, "DROP TABLE IF EXISTS symbol_children;", error) ||
        !sqlite_exec(db, "DROP TABLE IF EXISTS relations;", error) ||
        !sqlite_exec(db, "DROP TABLE IF EXISTS docs;", error) ||
        !sqlite_exec(db,
                     "CREATE TABLE meta (root_path TEXT NOT NULL);",
                     error) ||
        !sqlite_exec(db,
                     "CREATE TABLE symbols ("
                     "id TEXT PRIMARY KEY, "
                     "name TEXT, "
                     "display_name TEXT, "
                     "parent_id TEXT, "
                     "file_path TEXT, "
                     "line INTEGER, "
                     "kind TEXT, "
                     "signature TEXT, "
                     "type_text TEXT, "
                     "documentation TEXT, "
                     "snippet TEXT"
                     ");",
                     error) ||
        !sqlite_exec(db,
                     "CREATE TABLE symbol_children ("
                     "parent_id TEXT NOT NULL, "
                     "child_id TEXT NOT NULL, "
                     "order_index INTEGER NOT NULL"
                     ");",
                     error) ||
        !sqlite_exec(db,
                     "CREATE TABLE relations ("
                     "symbol_id TEXT NOT NULL, "
                     "kind TEXT NOT NULL, "
                     "label TEXT, "
                     "target_name TEXT, "
                     "detail TEXT"
                     ");",
                     error) ||
        !sqlite_exec(db,
                     "CREATE TABLE docs (path TEXT NOT NULL, content TEXT NOT NULL);",
                     error)) {
        sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
        sqlite3_close(db);
        return FALSE;
    }

    stmt = NULL;
    if (!sqlite_prepare(db, &stmt, "INSERT INTO meta(root_path) VALUES (?1);", error)) {
        sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
        sqlite3_close(db);
        return FALSE;
    }
    sqlite_bind_text_or_null(stmt, 1, index->root_path);
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_FAILED, "Failed to save metadata: %s", sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
        sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
        sqlite3_close(db);
        return FALSE;
    }
    sqlite3_finalize(stmt);

    if (!sqlite_prepare(db,
                        &stmt,
                        "INSERT INTO symbols(id,name,display_name,parent_id,file_path,line,kind,signature,type_text,documentation,snippet) "
                        "VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11);",
                        error)) {
        sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
        sqlite3_close(db);
        return FALSE;
    }
    for (symbol_index = 0; symbol_index < index->symbols->len; symbol_index++) {
        OdysiaSymbol *symbol;

        symbol = g_ptr_array_index(index->symbols, symbol_index);
        sqlite_bind_text_or_null(stmt, 1, symbol->id);
        sqlite_bind_text_or_null(stmt, 2, symbol->name);
        sqlite_bind_text_or_null(stmt, 3, symbol->display_name);
        sqlite_bind_text_or_null(stmt, 4, symbol->parent_id);
        sqlite_bind_text_or_null(stmt, 5, symbol->file_path);
        sqlite3_bind_int(stmt, 6, symbol->line);
        sqlite_bind_text_or_null(stmt, 7, odysia_symbol_kind_name(symbol->kind));
        sqlite_bind_text_or_null(stmt, 8, symbol->signature);
        sqlite_bind_text_or_null(stmt, 9, symbol->type_text);
        sqlite_bind_text_or_null(stmt, 10, symbol->documentation);
        sqlite_bind_text_or_null(stmt, 11, symbol->snippet);

        if (sqlite3_step(stmt) != SQLITE_DONE) {
            g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_FAILED, "Failed to save symbol '%s': %s", symbol->name, sqlite3_errmsg(db));
            sqlite3_finalize(stmt);
            sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
            sqlite3_close(db);
            return FALSE;
        }
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
    }
    sqlite3_finalize(stmt);

    if (!sqlite_prepare(db,
                        &stmt,
                        "INSERT INTO symbol_children(parent_id,child_id,order_index) VALUES (?1,?2,?3);",
                        error)) {
        sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
        sqlite3_close(db);
        return FALSE;
    }
    for (symbol_index = 0; symbol_index < index->symbols->len; symbol_index++) {
        OdysiaSymbol *symbol;
        guint child_index;

        symbol = g_ptr_array_index(index->symbols, symbol_index);
        for (child_index = 0; child_index < symbol->children->len; child_index++) {
            const gchar *child_id;

            child_id = g_ptr_array_index(symbol->children, child_index);
            sqlite_bind_text_or_null(stmt, 1, symbol->id);
            sqlite_bind_text_or_null(stmt, 2, child_id);
            sqlite3_bind_int(stmt, 3, (gint) child_index);
            if (sqlite3_step(stmt) != SQLITE_DONE) {
                g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_FAILED, "Failed to save symbol children: %s", sqlite3_errmsg(db));
                sqlite3_finalize(stmt);
                sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
                sqlite3_close(db);
                return FALSE;
            }
            sqlite3_reset(stmt);
            sqlite3_clear_bindings(stmt);
        }
    }
    sqlite3_finalize(stmt);

    if (!sqlite_prepare(db,
                        &stmt,
                        "INSERT INTO relations(symbol_id,kind,label,target_name,detail) VALUES (?1,?2,?3,?4,?5);",
                        error)) {
        sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
        sqlite3_close(db);
        return FALSE;
    }
    for (symbol_index = 0; symbol_index < index->symbols->len; symbol_index++) {
        OdysiaSymbol *symbol;
        guint relation_index;

        symbol = g_ptr_array_index(index->symbols, symbol_index);
        for (relation_index = 0; relation_index < symbol->relations->len; relation_index++) {
            OdysiaRelation *relation;

            relation = g_ptr_array_index(symbol->relations, relation_index);
            sqlite_bind_text_or_null(stmt, 1, symbol->id);
            sqlite_bind_text_or_null(stmt, 2, relation_kind_name(relation->kind));
            sqlite_bind_text_or_null(stmt, 3, relation->label);
            sqlite_bind_text_or_null(stmt, 4, relation->target_name);
            sqlite_bind_text_or_null(stmt, 5, relation->detail);
            if (sqlite3_step(stmt) != SQLITE_DONE) {
                g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_FAILED, "Failed to save symbol relations: %s", sqlite3_errmsg(db));
                sqlite3_finalize(stmt);
                sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
                sqlite3_close(db);
                return FALSE;
            }
            sqlite3_reset(stmt);
            sqlite3_clear_bindings(stmt);
        }
    }
    sqlite3_finalize(stmt);

    if (!sqlite_prepare(db, &stmt, "INSERT INTO docs(path,content) VALUES (?1,?2);", error)) {
        sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
        sqlite3_close(db);
        return FALSE;
    }
    for (doc_index = 0; doc_index < index->doc_files->len; doc_index++) {
        OdysiaDocFile *doc_file;

        doc_file = g_ptr_array_index(index->doc_files, doc_index);
        sqlite_bind_text_or_null(stmt, 1, doc_file->path);
        sqlite_bind_text_or_null(stmt, 2, doc_file->content);
        if (sqlite3_step(stmt) != SQLITE_DONE) {
            g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_FAILED, "Failed to save docs: %s", sqlite3_errmsg(db));
            sqlite3_finalize(stmt);
            sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
            sqlite3_close(db);
            return FALSE;
        }
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
    }
    sqlite3_finalize(stmt);

    if (!sqlite_exec(db, "COMMIT;", error)) {
        sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
        sqlite3_close(db);
        return FALSE;
    }

    sqlite3_close(db);
    return TRUE;
}

OdysiaIndex *odysia_index_load_sqlite(const gchar *sqlite_path, GError **error)
{
    sqlite3 *db;
    sqlite3_stmt *stmt;
    OdysiaIndex *index;
    gint rc;

    if (sqlite_path == NULL || sqlite_path[0] == '\0') {
        g_set_error_literal(error, G_FILE_ERROR, G_FILE_ERROR_INVAL, "SQLite path is required.");
        return NULL;
    }

    db = NULL;
    rc = sqlite3_open(sqlite_path, &db);
    if (rc != SQLITE_OK) {
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_FAILED, "Unable to open SQLite file: %s", sqlite3_errmsg(db));
        if (db != NULL) {
            sqlite3_close(db);
        }
        return NULL;
    }

    index = g_new0(OdysiaIndex, 1);
    index->symbols = g_ptr_array_new_with_free_func(symbol_free);
    index->symbols_by_id = g_hash_table_new(g_str_hash, g_str_equal);
    index->symbols_by_name = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify) g_ptr_array_unref);
    index->doc_files = g_ptr_array_new_with_free_func(doc_file_free);

    stmt = NULL;
    if (!sqlite_prepare(db, &stmt, "SELECT root_path FROM meta LIMIT 1;", error)) {
        odysia_index_free(index);
        sqlite3_close(db);
        return NULL;
    }
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const gchar *root_path;

        root_path = (const gchar *) sqlite3_column_text(stmt, 0);
        index->root_path = g_strdup(root_path != NULL ? root_path : ".");
    } else {
        index->root_path = g_strdup(".");
    }
    sqlite3_finalize(stmt);

    if (!sqlite_prepare(db,
                        &stmt,
                        "SELECT id,name,display_name,parent_id,file_path,line,kind,signature,type_text,documentation,snippet "
                        "FROM symbols ORDER BY rowid;",
                        error)) {
        odysia_index_free(index);
        sqlite3_close(db);
        return NULL;
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        OdysiaSymbol *symbol;
        const gchar *id;
        const gchar *name;
        const gchar *display_name;
        const gchar *parent_id;
        const gchar *file_path;
        gint line;
        const gchar *kind_name;

        id = (const gchar *) sqlite3_column_text(stmt, 0);
        name = (const gchar *) sqlite3_column_text(stmt, 1);
        display_name = (const gchar *) sqlite3_column_text(stmt, 2);
        parent_id = (const gchar *) sqlite3_column_text(stmt, 3);
        file_path = (const gchar *) sqlite3_column_text(stmt, 4);
        line = sqlite3_column_int(stmt, 5);
        kind_name = (const gchar *) sqlite3_column_text(stmt, 6);

        symbol = symbol_new(symbol_kind_from_name(kind_name),
                            id != NULL ? id : "",
                            name != NULL ? name : "",
                            file_path != NULL ? file_path : "",
                            line);
        g_free(symbol->display_name);
        symbol->display_name = g_strdup(display_name != NULL ? display_name : symbol->name);
        symbol->parent_id = g_strdup(parent_id);
        symbol->signature = g_strdup((const gchar *) sqlite3_column_text(stmt, 7));
        symbol->type_text = g_strdup((const gchar *) sqlite3_column_text(stmt, 8));
        symbol->documentation = g_strdup((const gchar *) sqlite3_column_text(stmt, 9));
        symbol->snippet = g_strdup((const gchar *) sqlite3_column_text(stmt, 10));
        index_add_symbol(index, symbol);
    }
    sqlite3_finalize(stmt);

    if (!sqlite_prepare(db,
                        &stmt,
                        "SELECT parent_id,child_id FROM symbol_children ORDER BY parent_id,order_index;",
                        error)) {
        odysia_index_free(index);
        sqlite3_close(db);
        return NULL;
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const gchar *parent_id;
        const gchar *child_id;
        OdysiaSymbol *parent;

        parent_id = (const gchar *) sqlite3_column_text(stmt, 0);
        child_id = (const gchar *) sqlite3_column_text(stmt, 1);
        parent = odysia_index_get_symbol(index, parent_id);
        if (parent != NULL && child_id != NULL) {
            g_ptr_array_add(parent->children, g_strdup(child_id));
        }
    }
    sqlite3_finalize(stmt);

    if (!sqlite_prepare(db,
                        &stmt,
                        "SELECT symbol_id,kind,label,target_name,detail FROM relations ORDER BY rowid;",
                        error)) {
        odysia_index_free(index);
        sqlite3_close(db);
        return NULL;
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const gchar *symbol_id;
        const gchar *kind_name;
        const gchar *label;
        const gchar *target_name;
        const gchar *detail;
        OdysiaSymbol *symbol;

        symbol_id = (const gchar *) sqlite3_column_text(stmt, 0);
        kind_name = (const gchar *) sqlite3_column_text(stmt, 1);
        label = (const gchar *) sqlite3_column_text(stmt, 2);
        target_name = (const gchar *) sqlite3_column_text(stmt, 3);
        detail = (const gchar *) sqlite3_column_text(stmt, 4);
        symbol = odysia_index_get_symbol(index, symbol_id);
        if (symbol != NULL) {
            add_relation(symbol,
                         relation_kind_from_name(kind_name),
                         label != NULL ? label : "",
                         target_name != NULL ? target_name : "",
                         detail != NULL ? detail : "");
        }
    }
    sqlite3_finalize(stmt);

    if (!sqlite_prepare(db, &stmt, "SELECT path,content FROM docs ORDER BY rowid;", error)) {
        odysia_index_free(index);
        sqlite3_close(db);
        return NULL;
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        OdysiaDocFile *doc_file;
        const gchar *path;
        const gchar *content;

        path = (const gchar *) sqlite3_column_text(stmt, 0);
        content = (const gchar *) sqlite3_column_text(stmt, 1);
        doc_file = g_new0(OdysiaDocFile, 1);
        doc_file->path = g_strdup(path != NULL ? path : "");
        doc_file->content = g_strdup(content != NULL ? content : "");
        g_ptr_array_add(index->doc_files, doc_file);
    }
    sqlite3_finalize(stmt);

    sqlite3_close(db);
    return index;
}

void odysia_index_free(OdysiaIndex *index)
{
    if (index == NULL) {
        return;
    }

    g_free(index->root_path);
    if (index->symbols != NULL) {
        g_ptr_array_free(index->symbols, TRUE);
    }
    if (index->symbols_by_id != NULL) {
        g_hash_table_destroy(index->symbols_by_id);
    }
    if (index->symbols_by_name != NULL) {
        g_hash_table_destroy(index->symbols_by_name);
    }
    if (index->doc_files != NULL) {
        g_ptr_array_free(index->doc_files, TRUE);
    }
    g_free(index);
}

OdysiaSymbol *odysia_index_get_symbol(const OdysiaIndex *index, const gchar *symbol_id)
{
    if (index == NULL || symbol_id == NULL) {
        return NULL;
    }
    return g_hash_table_lookup(index->symbols_by_id, symbol_id);
}

OdysiaSymbol *odysia_index_resolve_name(const OdysiaIndex *index, const gchar *symbol_name, OdysiaSymbolKind preferred_kind)
{
    GPtrArray *matches;
    guint array_index;

    if (index == NULL || symbol_name == NULL) {
        return NULL;
    }
    matches = g_hash_table_lookup(index->symbols_by_name, symbol_name);
    if (matches == NULL || matches->len == 0) {
        return NULL;
    }
    for (array_index = 0; array_index < matches->len; array_index++) {
        OdysiaSymbol *symbol;

        symbol = g_ptr_array_index(matches, array_index);
        if (symbol->kind == preferred_kind) {
            return symbol;
        }
    }
    return g_ptr_array_index(matches, 0);
}

static gchar *extract_doc_paragraph(const gchar *text, const gchar *needle)
{
    gchar *match;
    const gchar *start;
    const gchar *end;

    match = g_strstr_len(text, -1, needle);
    if (match == NULL) {
        return NULL;
    }
    start = match;
    while (start > text) {
        if (start[-1] == '\n' && (start == text + 1 || start[-2] == '\n')) {
            break;
        }
        start--;
    }
    end = match;
    while (*end != '\0') {
        if (end[0] == '\n' && end[1] == '\n') {
            break;
        }
        end++;
    }
    return trim_copy(g_strndup(start, end - start));
}

gchar *odysia_index_collect_external_docs(const OdysiaIndex *index, const gchar *symbol_name)
{
    GString *buffer;
    guint array_index;

    if (index == NULL || symbol_name == NULL || symbol_name[0] == '\0') {
        return g_strdup("");
    }

    buffer = g_string_new(NULL);
    for (array_index = 0; array_index < index->doc_files->len; array_index++) {
        OdysiaDocFile *doc_file;
        gchar *excerpt;

        doc_file = g_ptr_array_index(index->doc_files, array_index);
        excerpt = extract_doc_paragraph(doc_file->content, symbol_name);
        if (excerpt == NULL || excerpt[0] == '\0') {
            g_free(excerpt);
            continue;
        }
        if (buffer->len > 0) {
            g_string_append(buffer, "\n\n");
        }
        g_string_append_printf(buffer, "[%s]\n%s", doc_file->path, excerpt);
        g_free(excerpt);
        if (buffer->len > 4000) {
            break;
        }
    }

    return g_string_free(buffer, FALSE);
}