#include "ui.h"

#include "indexer.h"

#include <errno.h>

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifndef PACKAGE_VERSION
#define PACKAGE_VERSION "0.5.0-beta5"
#endif

#define DEFAULT_DETAIL_FONT "Sans 11"
#define DEFAULT_SOURCE_FONT "Monospace 11"

typedef struct {
    gint start_offset;
    gint end_offset;
    gchar *target_id;
} LinkRange;

typedef struct {
    GtkApplication *app;
    gint ref_count;
    gint shutting_down;
    GtkWidget *window;
    GtkWidget *menu_bar;
    GtkWidget *file_menu_item;
    GtkWidget *actions_menu_item;
    GtkWidget *help_menu_item;
    GtkWidget *index_item;
    GtkWidget *build_item;
    GtkWidget *stop_item;
    GtkWidget *settings_item;
    GtkWidget *input_row;
    GtkWidget *content_pane;
    GtkWidget *path_entry;
    GtkWidget *search_entry;
    GtkWidget *sort_combo;
    GtkWidget *status_label;
    GtkWidget *status_bar;
    guint status_context_id;
    GtkWidget *overall_progress_bar;
    GtkWidget *stage_progress_bar;
    GtkWidget *tree_view;
    GtkTreeStore *tree_store;
    GtkWidget *detail_view;
    GtkWidget *source_view;
    GtkCssProvider *detail_font_provider;
    GtkCssProvider *source_font_provider;
    GtkTextTag *link_tag;
    GtkTextTag *header_tag;
    GtkTextTag *section_tag;
    GtkTextTag *accent_tag;
    GtkTextTag *source_keyword_tag;
    GtkTextTag *source_kernel_tag;
    GtkTextTag *source_string_tag;
    GtkTextTag *source_comment_tag;
    GtkTextTag *source_number_tag;
    GtkTextTag *source_preproc_tag;
    GPtrArray *detail_links;
    GHashTable *row_paths;
    GtkWidget *build_dialog;
    GtkWidget *build_file_label;
    GtkWidget *build_log_view;
    GtkWidget *build_stop_button;
    GtkWidget *build_close_button;
    GtkTextBuffer *build_log_buffer;
    GSubprocess *build_process;
    GDataInputStream *build_stream;
    GCancellable *build_cancel;
    GCancellable *index_cancel;
    GThread *index_thread;
    guint tree_build_source_id;
    guint source_thread_count;
    gchar *detail_font;
    gchar *source_font;
    gboolean indexing;
    OdysiaIndex *index;
} AppState;

typedef enum {
    TREE_SORT_NAME,
    TREE_SORT_LOCATION,
    TREE_SORT_KIND
} TreeSortMode;

typedef struct {
    AppState *state;
    gchar *root_path;
    guint source_thread_count;
} IndexJob;

typedef struct {
    AppState *state;
    OdysiaIndexStage stage;
    guint current;
    guint total;
    gchar *path;
} IndexProgressUpdate;

typedef struct {
    AppState *state;
    OdysiaIndex *index;
    gchar *error_message;
    gchar *root_path;
} IndexResult;

typedef struct {
    AppState *state;
    gchar *filename;
} LoadJob;

typedef struct {
    AppState *state;
    OdysiaIndex *index;
    gchar *error_message;
    gchar *filename;
} LoadResult;

#define TREE_CATEGORY_COUNT 15

static const gchar *const tree_category_titles[TREE_CATEGORY_COUNT] = {
    "Functions", "Structs", "Unions", "Enums and Enumerators", "Typedefs",
    "Global Variables", "Modules", "Classes", "Traits", "Macros", "Labels",
    "Configuration", "Build Targets", "Device Tree Nodes", "Semantic Rules"
};

static const gchar *const tree_category_icons[TREE_CATEGORY_COUNT] = {
    "applications-engineering-symbolic", "folder-symbolic", "object-flip-horizontal-symbolic",
    "view-list-symbolic", "text-x-csrc-symbolic", "emblem-system-symbolic",
    "folder-new-symbolic", "system-users-symbolic", "emblem-shared-symbolic",
    "code-context-symbolic", "bookmark-new-symbolic", "preferences-system-symbolic",
    "applications-engineering-symbolic", "network-workgroup-symbolic", "dialog-information-symbolic"
};

typedef struct {
    AppState *state;
    gchar *query_folded;
    GPtrArray *categories[TREE_CATEGORY_COUNT];
    guint category_index;
    guint symbol_index;
    guint child_index;
    guint total_roots;
    guint completed_roots;
    gboolean parent_inserted;
    gboolean current_parent_matches;
    TreeSortMode sort_mode;
    GPtrArray *current_children;
    GHashTable *language_iters;
    GHashTable *category_iters;
    GHashTable *letter_iters;
    GHashTable *documented_names;
    GtkTreeIter current_parent_iter;
} TreeBuildTask;

enum {
    COLUMN_ICON_NAME,
    COLUMN_DOCUMENTATION_ICON,
    COLUMN_TITLE,
    COLUMN_KIND,
    COLUMN_LOCATION,
    COLUMN_SYMBOL_ID,
    COLUMN_WEIGHT,
    N_COLUMNS
};

static void clear_object_ptr(gpointer *object_ptr);
static void rebuild_tree(AppState *state);
static GtkTreeIter append_row(AppState *state,
                              GtkTreeIter *parent,
                              const gchar *icon_name,
                              const gchar *title,
                              const gchar *kind,
                              const gchar *location,
                              const gchar *symbol_id,
                              gint weight);
static gint symbol_sort_func(gconstpointer left_ptr, gconstpointer right_ptr, gpointer user_data);
static void buffer_append_plain(GtkTextBuffer *buffer, const gchar *text);
static gchar *symbol_location_text(const OdysiaSymbol *symbol);
static gchar *symbol_tree_title(const OdysiaSymbol *symbol);
static void apply_source_highlighting(AppState *state, GtkTextBuffer *buffer, const gchar *text);

static gchar *settings_file_path(void)
{
    return g_build_filename(g_get_user_config_dir(), "odysia", "settings.ini", NULL);
}

static void load_settings(AppState *state)
{
    GKeyFile *key_file;
    gchar *path;
    GError *error;
    gint thread_count;

    state->source_thread_count = MAX(1, g_get_num_processors());
    state->detail_font = g_strdup(DEFAULT_DETAIL_FONT);
    state->source_font = g_strdup(DEFAULT_SOURCE_FONT);
    key_file = g_key_file_new();
    path = settings_file_path();
    error = NULL;
    if (!g_key_file_load_from_file(key_file, path, G_KEY_FILE_NONE, &error)) {
        g_clear_error(&error);
        g_free(path);
        g_key_file_unref(key_file);
        return;
    }

    thread_count = g_key_file_get_integer(key_file, "Parser", "Threads", NULL);
    if (thread_count > 0) {
        state->source_thread_count = (guint) MIN(thread_count, 1024);
    }
    if (g_key_file_has_key(key_file, "Fonts", "Detail", NULL)) {
        g_free(state->detail_font);
        state->detail_font = g_key_file_get_string(key_file, "Fonts", "Detail", NULL);
    }
    if (g_key_file_has_key(key_file, "Fonts", "Source", NULL)) {
        g_free(state->source_font);
        state->source_font = g_key_file_get_string(key_file, "Fonts", "Source", NULL);
    }
    if (state->detail_font == NULL || state->detail_font[0] == '\0') {
        g_free(state->detail_font);
        state->detail_font = g_strdup(DEFAULT_DETAIL_FONT);
    }
    if (state->source_font == NULL || state->source_font[0] == '\0') {
        g_free(state->source_font);
        state->source_font = g_strdup(DEFAULT_SOURCE_FONT);
    }
    g_free(path);
    g_key_file_unref(key_file);
}

static gboolean save_settings(AppState *state, GError **error)
{
    GKeyFile *key_file;
    gchar *path;
    gchar *directory;
    gchar *data;
    gsize data_length;
    gboolean saved;

    key_file = g_key_file_new();
    g_key_file_set_integer(key_file, "Parser", "Threads", (gint) state->source_thread_count);
    g_key_file_set_string(key_file, "Fonts", "Detail", state->detail_font);
    g_key_file_set_string(key_file, "Fonts", "Source", state->source_font);
    data = g_key_file_to_data(key_file, &data_length, NULL);
    path = settings_file_path();
    directory = g_path_get_dirname(path);
    if (g_mkdir_with_parents(directory, 0700) != 0) {
        g_set_error(error,
                    G_FILE_ERROR,
                    g_file_error_from_errno(errno),
                    "Unable to create settings directory '%s': %s",
                    directory,
                    g_strerror(errno));
        saved = FALSE;
    } else {
        saved = g_file_set_contents(path, data, (gssize) data_length, error);
    }
    g_free(directory);
    g_free(path);
    g_free(data);
    g_key_file_unref(key_file);
    return saved;
}

static void apply_text_view_fonts(AppState *state)
{
    PangoFontDescription *description;
    GtkCssProvider *provider;
    GtkStyleContext *context;
    gchar *family;
    gchar *css;
    gdouble size;
    const gchar *description_family;
    const gchar *style;

    if (state->detail_view == NULL || state->source_view == NULL) {
        return;
    }

    description = pango_font_description_from_string(state->detail_font);
    description_family = pango_font_description_get_family(description);
    family = g_strescape(description_family != NULL ? description_family : "Sans", NULL);
    size = (gdouble) pango_font_description_get_size(description) / PANGO_SCALE;
    style = pango_font_description_get_style(description) == PANGO_STYLE_ITALIC ? "italic" : "normal";
    css = g_strdup_printf("textview { font-family: \"%s\"; font-size: %.1fpt; font-style: %s; font-weight: %d; }",
                          family != NULL ? family : "Sans",
                          size > 0.0 ? size : 11.0,
                          style,
                          pango_font_description_get_weight(description));
    provider = gtk_css_provider_new();
    gtk_css_provider_load_from_data(provider, css, -1, NULL);
    context = gtk_widget_get_style_context(state->detail_view);
    if (state->detail_font_provider != NULL) {
        gtk_style_context_remove_provider(context, GTK_STYLE_PROVIDER(state->detail_font_provider));
        g_object_unref(state->detail_font_provider);
    }
    state->detail_font_provider = provider;
    gtk_style_context_add_provider(context, GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_free(css);
    g_free(family);
    pango_font_description_free(description);

    description = pango_font_description_from_string(state->source_font);
    description_family = pango_font_description_get_family(description);
    family = g_strescape(description_family != NULL ? description_family : "Monospace", NULL);
    size = (gdouble) pango_font_description_get_size(description) / PANGO_SCALE;
    style = pango_font_description_get_style(description) == PANGO_STYLE_ITALIC ? "italic" : "normal";
    css = g_strdup_printf("textview { font-family: \"%s\"; font-size: %.1fpt; font-style: %s; font-weight: %d; }",
                          family != NULL ? family : "Monospace",
                          size > 0.0 ? size : 11.0,
                          style,
                          pango_font_description_get_weight(description));
    provider = gtk_css_provider_new();
    gtk_css_provider_load_from_data(provider, css, -1, NULL);
    context = gtk_widget_get_style_context(state->source_view);
    if (state->source_font_provider != NULL) {
        gtk_style_context_remove_provider(context, GTK_STYLE_PROVIDER(state->source_font_provider));
        g_object_unref(state->source_font_provider);
    }
    state->source_font_provider = provider;
    gtk_style_context_add_provider(context, GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_free(css);
    g_free(family);
    pango_font_description_free(description);
}

static gboolean documentation_word_character(gchar character)
{
    return g_ascii_isalnum(character) || character == '_' || character == '-' || character == '.';
}

static GHashTable *collect_documented_names(const OdysiaIndex *index)
{
    GHashTable *known_names;
    GHashTable *documented_names;
    guint symbol_index;
    guint doc_index;

    known_names = g_hash_table_new(g_str_hash, g_str_equal);
    documented_names = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    for (symbol_index = 0; symbol_index < index->symbols->len; symbol_index++) {
        OdysiaSymbol *symbol;

        symbol = g_ptr_array_index(index->symbols, symbol_index);
        if (symbol->name == NULL || symbol->name[0] == '\0') {
            continue;
        }
        g_hash_table_add(known_names, symbol->name);
        if (symbol->documentation != NULL && symbol->documentation[0] != '\0') {
            g_hash_table_add(documented_names, g_strdup(symbol->name));
        }
    }

    for (doc_index = 0; doc_index < index->doc_files->len; doc_index++) {
        OdysiaDocFile *doc_file;
        const gchar *cursor;

        doc_file = g_ptr_array_index(index->doc_files, doc_index);
        cursor = doc_file->content;
        while (cursor != NULL && *cursor != '\0') {
            const gchar *word_start;
            gchar *word;

            while (*cursor != '\0' && !documentation_word_character(*cursor)) {
                cursor++;
            }
            word_start = cursor;
            while (*cursor != '\0' && documentation_word_character(*cursor)) {
                cursor++;
            }
            if (cursor == word_start) {
                continue;
            }
            word = g_strndup(word_start, cursor - word_start);
            if (g_hash_table_contains(known_names, word)) {
                g_hash_table_add(documented_names, word);
            } else {
                gchar *normalized_start;
                gchar *normalized_end;

                normalized_start = word;
                normalized_end = word + strlen(word);
                while (normalized_start < normalized_end &&
                       (*normalized_start == '.' || *normalized_start == '-')) {
                    normalized_start++;
                }
                while (normalized_end > normalized_start &&
                       (normalized_end[-1] == '.' || normalized_end[-1] == '-')) {
                    normalized_end--;
                }
                if (normalized_start < normalized_end) {
                    gchar *normalized_word;

                    normalized_word = g_strndup(normalized_start, normalized_end - normalized_start);
                    if (g_hash_table_contains(known_names, normalized_word)) {
                        g_hash_table_add(documented_names, normalized_word);
                    } else {
                        g_free(normalized_word);
                    }
                }
                g_free(word);
            }
        }
    }
    g_hash_table_destroy(known_names);
    return documented_names;
}

static void set_row_documentation_icon(TreeBuildTask *task,
                                       GtkTreeIter *iter,
                                       const OdysiaSymbol *symbol)
{
    const gchar *icon_name;

    icon_name = symbol != NULL && symbol->name != NULL &&
                g_hash_table_contains(task->documented_names, symbol->name)
                ? "dialog-warning-symbolic"
                : NULL;
    gtk_tree_store_set(task->state->tree_store,
                       iter,
                       COLUMN_DOCUMENTATION_ICON, icon_name,
                       -1);
}

static void set_application_icon(GtkWindow *window)
{
    GError *error;
    GdkPixbuf *icon;

    error = NULL;
    icon = gdk_pixbuf_new_from_resource("/org/odysia/odysia-icon.png", &error);
    if (icon == NULL) {
        g_warning("Unable to load the Odysia application icon: %s",
                  error != NULL ? error->message : "unknown error");
        g_clear_error(&error);
        return;
    }
    gtk_window_set_default_icon(icon);
    gtk_window_set_icon(window, icon);
    g_object_unref(icon);
}

static const gchar *symbol_icon_name(OdysiaSymbolKind kind)
{
    switch (kind) {
    case ODYSIA_SYMBOL_FUNCTION:
        return "system-run-symbolic";
    case ODYSIA_SYMBOL_STRUCT:
        return "folder-symbolic";
    case ODYSIA_SYMBOL_UNION:
        return "object-flip-horizontal-symbolic";
    case ODYSIA_SYMBOL_ENUM:
        return "view-list-symbolic";
    case ODYSIA_SYMBOL_ENUMERATOR:
        return "format-text-numbered-symbolic";
    case ODYSIA_SYMBOL_TYPEDEF:
        return "text-x-csrc-symbolic";
    case ODYSIA_SYMBOL_GLOBAL_VARIABLE:
        return "emblem-system-symbolic";
    case ODYSIA_SYMBOL_FIELD:
        return "go-next-symbolic";
    case ODYSIA_SYMBOL_PARAMETER:
        return "go-jump-symbolic";
    case ODYSIA_SYMBOL_LOCAL_VARIABLE:
        return "insert-text-symbolic";
    case ODYSIA_SYMBOL_MODULE:
        return "folder-new-symbolic";
    case ODYSIA_SYMBOL_CLASS:
        return "system-users-symbolic";
    case ODYSIA_SYMBOL_TRAIT:
        return "emblem-shared-symbolic";
    case ODYSIA_SYMBOL_MACRO:
        return "code-context-symbolic";
    case ODYSIA_SYMBOL_LABEL:
        return "bookmark-new-symbolic";
    case ODYSIA_SYMBOL_CONFIG:
        return "preferences-system-symbolic";
    case ODYSIA_SYMBOL_BUILD_TARGET:
        return "applications-engineering-symbolic";
    case ODYSIA_SYMBOL_DEVICE_NODE:
        return "network-workgroup-symbolic";
    case ODYSIA_SYMBOL_RULE:
        return "dialog-information-symbolic";
    default:
        return "text-x-generic-symbolic";
    }
}

static const gchar *const source_c_keywords[] = {
    "auto", "break", "case", "char", "const", "continue", "default", "do", "double",
    "else", "enum", "extern", "float", "for", "goto", "if", "inline", "int", "long",
    "register", "restrict", "return", "short", "signed", "sizeof", "static", "struct",
    "switch", "typedef", "union", "unsigned", "void", "volatile", "while", "_Bool",
    "_Complex", "_Imaginary"
};

static const gchar *const source_kernel_keywords[] = {
    "__always_inline", "__attribute__", "__cold", "__deprecated", "__exit", "__force",
    "__init", "__iomem", "__latent_entropy", "__maybe_unused", "__must_check", "__nocfi",
    "__packed", "__percpu", "__printf", "__pure", "__rcu", "__section", "__sched",
    "__user", "__visible", "__weak", "asmlinkage", "barrier", "container_of", "likely",
    "noinline", "notrace", "READ_ONCE", "smp_mb", "spin_lock", "spin_unlock", "unlikely",
    "WRITE_ONCE"
};

static gboolean token_in_set(const gchar *token, const gchar *const *tokens, gsize token_count)
{
    gsize index;

    for (index = 0; index < token_count; index++) {
        if (g_strcmp0(token, tokens[index]) == 0) {
            return TRUE;
        }
    }
    return FALSE;
}

static gchar *symbol_location_text(const OdysiaSymbol *symbol)
{
    gchar *dir_name;
    gchar *base_name;
    gchar *result;

    if (symbol == NULL || symbol->file_path == NULL || symbol->file_path[0] == '\0') {
        return g_strdup("");
    }

    dir_name = g_path_get_dirname(symbol->file_path);
    base_name = g_path_get_basename(symbol->file_path);
    result = g_strdup_printf("%s | %s:%d", dir_name, base_name, symbol->line);
    g_free(base_name);
    g_free(dir_name);
    return result;
}

static gchar *symbol_tree_title(const OdysiaSymbol *symbol)
{
    if (symbol == NULL) {
        return g_strdup("");
    }
    if (symbol->kind == ODYSIA_SYMBOL_FUNCTION) {
        return g_strdup_printf("%s()", symbol->display_name);
    }
    return g_strdup(symbol->display_name);
}

static void buffer_append_tagged(GtkTextBuffer *buffer, GtkTextTag *tag, const gchar *text)
{
    GtkTextIter start;
    GtkTextIter end;

    gtk_text_buffer_get_end_iter(buffer, &start);
    gtk_text_buffer_insert(buffer, &start, text, -1);
    gtk_text_buffer_get_end_iter(buffer, &end);
    if (tag != NULL) {
        gtk_text_buffer_apply_tag(buffer, tag, &start, &end);
    }
}

static void show_welcome_text(AppState *state)
{
    GtkTextBuffer *detail_buffer;
    GtkTextBuffer *source_buffer;

    detail_buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(state->detail_view));
    source_buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(state->source_view));
    gtk_text_buffer_set_text(detail_buffer, "", -1);
    gtk_text_buffer_set_text(source_buffer, "", -1);

    buffer_append_tagged(detail_buffer, state->header_tag, "Odysia Linux Source Explorer\n");
    buffer_append_tagged(detail_buffer, state->section_tag, "Getting started\n");
    buffer_append_plain(detail_buffer, "- Choose a Linux source tree from File > Select Source Tree.\n");
    buffer_append_plain(detail_buffer, "- Run Actions > Index to parse source, data structures, variables, and docs.\n");
    buffer_append_plain(detail_buffer, "- Use the filter box to narrow symbols as you explore.\n\n");

    buffer_append_tagged(detail_buffer, state->section_tag, "For new kernel developers\n");
    buffer_append_plain(detail_buffer, "- Start from a struct or subsystem entry point, then follow linked fields and calls.\n");
    buffer_append_plain(detail_buffer, "- Read inline docs and extracted documentation snippets before diving into call chains.\n\n");

    buffer_append_tagged(detail_buffer, state->section_tag, "For experienced kernel developers\n");
    buffer_append_plain(detail_buffer, "- Use the tree to jump quickly across symbols by kind.\n");
    buffer_append_plain(detail_buffer, "- Build from the Actions menu and monitor compile units in the build dialog.\n");

    buffer_append_plain(source_buffer, "Select a symbol to preview its source snippet here.\n");
}

static void link_range_free(gpointer data)
{
    LinkRange *range;

    range = data;
    if (range == NULL) {
        return;
    }
    g_free(range->target_id);
    g_free(range);
}

static void set_status(AppState *state, const gchar *message)
{
    const gchar *safe_message;

    safe_message = message != NULL ? message : "";
    gtk_label_set_text(GTK_LABEL(state->status_label), safe_message);
    if (state->status_bar != NULL) {
        gtk_statusbar_pop(GTK_STATUSBAR(state->status_bar), state->status_context_id);
        gtk_statusbar_push(GTK_STATUSBAR(state->status_bar), state->status_context_id, safe_message);
    }
}

static AppState *app_state_ref(AppState *state)
{
    g_atomic_int_inc(&state->ref_count);
    return state;
}

static void app_state_unref(AppState *state)
{
    if (state == NULL) {
        return;
    }
    if (!g_atomic_int_dec_and_test(&state->ref_count)) {
        return;
    }
    if (state->index != NULL) {
        odysia_index_free(state->index);
    }
    if (state->detail_links != NULL) {
        g_ptr_array_free(state->detail_links, TRUE);
    }
    if (state->row_paths != NULL) {
        g_hash_table_destroy(state->row_paths);
    }
    clear_object_ptr((gpointer *) &state->build_process);
    clear_object_ptr((gpointer *) &state->build_stream);
    clear_object_ptr((gpointer *) &state->build_cancel);
    clear_object_ptr((gpointer *) &state->index_cancel);
    clear_object_ptr((gpointer *) &state->detail_font_provider);
    clear_object_ptr((gpointer *) &state->source_font_provider);
    g_free(state->detail_font);
    g_free(state->source_font);
    g_free(state);
}

static void set_indexing_controls_sensitive(AppState *state, gboolean sensitive)
{
    gboolean can_cancel_index;

    can_cancel_index = !sensitive && state->index_cancel != NULL &&
                       !g_cancellable_is_cancelled(state->index_cancel);
    if (state->menu_bar != NULL) {
        gtk_widget_set_sensitive(state->menu_bar, TRUE);
    }
    if (state->file_menu_item != NULL) {
        gtk_widget_set_sensitive(state->file_menu_item, sensitive);
    }
    if (state->actions_menu_item != NULL) {
        gtk_widget_set_sensitive(state->actions_menu_item, sensitive || can_cancel_index);
    }
    if (state->help_menu_item != NULL) {
        gtk_widget_set_sensitive(state->help_menu_item, sensitive);
    }
    if (state->index_item != NULL) {
        gtk_widget_set_sensitive(state->index_item, sensitive);
    }
    if (state->build_item != NULL) {
        gtk_widget_set_sensitive(state->build_item, sensitive);
    }
    if (state->stop_item != NULL) {
        gtk_widget_set_sensitive(state->stop_item, sensitive || can_cancel_index);
    }
    if (state->settings_item != NULL) {
        gtk_widget_set_sensitive(state->settings_item, sensitive);
    }
    if (state->input_row != NULL) {
        gtk_widget_set_sensitive(state->input_row, sensitive);
    }
    if (state->content_pane != NULL) {
        gtk_widget_set_sensitive(state->content_pane, sensitive);
    }
    if (state->build_dialog != NULL) {
        gtk_widget_set_sensitive(state->build_dialog, sensitive);
    }
}

static void reset_progress_bars(AppState *state, const gchar *stage_text)
{
    gtk_widget_set_sensitive(state->overall_progress_bar, FALSE);
    gtk_widget_set_sensitive(state->stage_progress_bar, FALSE);
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(state->overall_progress_bar), 0.0);
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(state->stage_progress_bar), 0.0);
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(state->overall_progress_bar), "Overall progress");
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(state->stage_progress_bar), stage_text != NULL ? stage_text : "Idle");
}

static void clear_loaded_index_data(AppState *state)
{
    GtkTextBuffer *detail_buffer;
    GtkTextBuffer *source_buffer;

    if (state->tree_build_source_id != 0) {
        g_source_remove(state->tree_build_source_id);
        state->tree_build_source_id = 0;
    }
    if (state->index != NULL) {
        odysia_index_free(state->index);
        state->index = NULL;
    }

    gtk_tree_store_clear(state->tree_store);
    g_hash_table_remove_all(state->row_paths);
    g_ptr_array_set_size(state->detail_links, 0);

    detail_buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(state->detail_view));
    source_buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(state->source_view));
    gtk_text_buffer_set_text(detail_buffer, "", -1);
    gtk_text_buffer_set_text(source_buffer, "", -1);
}

static void stop_active_work(AppState *state)
{
    gboolean stopped_anything;

    stopped_anything = FALSE;
    if (state->indexing) {
        if (state->index_cancel != NULL) {
            g_cancellable_cancel(state->index_cancel);
            set_indexing_controls_sensitive(state, FALSE);
            gtk_progress_bar_set_text(GTK_PROGRESS_BAR(state->stage_progress_bar), "Cancelling...");
            set_status(state, "Cancelling indexing...");
            stopped_anything = TRUE;
        }
        if (state->tree_build_source_id != 0) {
            g_source_remove(state->tree_build_source_id);
            state->tree_build_source_id = 0;
            state->indexing = FALSE;
            clear_object_ptr((gpointer *) &state->index_cancel);
            set_indexing_controls_sensitive(state, TRUE);
            reset_progress_bars(state, "Stopped");
            stopped_anything = TRUE;
        }
    }
    if (state->build_process != NULL) {
        if (state->build_cancel != NULL) {
            g_cancellable_cancel(state->build_cancel);
        }
        g_subprocess_force_exit(state->build_process);
        stopped_anything = TRUE;
    }
    if (stopped_anything && !state->indexing) {
        set_status(state, "Stopped active work.");
    } else if (!stopped_anything) {
        set_status(state, "No active indexing or build to stop.");
    }
}

static const gchar *index_stage_name(OdysiaIndexStage stage)
{
    switch (stage) {
    case ODYSIA_INDEX_STAGE_DISCOVER:
        return "Discovering files";
    case ODYSIA_INDEX_STAGE_PARSE_SOURCE:
        return "Parsing source files";
    case ODYSIA_INDEX_STAGE_PARSE_DOCS:
        return "Parsing documentation";
    case ODYSIA_INDEX_STAGE_BUILD_TREE:
        return "Building symbol tree";
    default:
        return "Indexing";
    }
}

static gdouble overall_fraction_for_stage(OdysiaIndexStage stage, guint current, guint total)
{
    gdouble stage_fraction;

    stage_fraction = total > 0 ? (gdouble) current / (gdouble) total : 0.0;
    if (stage_fraction < 0.0) {
        stage_fraction = 0.0;
    }
    if (stage_fraction > 1.0) {
        stage_fraction = 1.0;
    }

    switch (stage) {
    case ODYSIA_INDEX_STAGE_DISCOVER:
        return 0.10 * stage_fraction;
    case ODYSIA_INDEX_STAGE_PARSE_SOURCE:
        return 0.10 + (0.70 * stage_fraction);
    case ODYSIA_INDEX_STAGE_PARSE_DOCS:
        return 0.80 + (0.10 * stage_fraction);
    case ODYSIA_INDEX_STAGE_BUILD_TREE:
        return 0.90 + (0.10 * stage_fraction);
    default:
        return 0.0;
    }
}

static void set_index_stage_progress(AppState *state,
                                     OdysiaIndexStage stage,
                                     guint current,
                                     guint total,
                                     const gchar *path)
{
    gchar *stage_text;
    gchar *status_text;
    gchar *base_name;

    base_name = path != NULL ? g_path_get_basename(path) : NULL;

    if (total > 0) {
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(state->stage_progress_bar), (gdouble) current / (gdouble) total);
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(state->overall_progress_bar), overall_fraction_for_stage(stage, current, total));
    } else {
        gtk_progress_bar_pulse(GTK_PROGRESS_BAR(state->stage_progress_bar));
        if (stage == ODYSIA_INDEX_STAGE_DISCOVER) {
            gtk_progress_bar_pulse(GTK_PROGRESS_BAR(state->overall_progress_bar));
        }
    }

    if (base_name != NULL && base_name[0] != '\0') {
        if (total > 0) {
            stage_text = g_strdup_printf("%s %u/%u: %s", index_stage_name(stage), current, total, base_name);
        } else {
            stage_text = g_strdup_printf("%s: %s", index_stage_name(stage), base_name);
        }
    } else if (total > 0) {
        stage_text = g_strdup_printf("%s %u/%u", index_stage_name(stage), current, total);
    } else {
        stage_text = g_strdup(index_stage_name(stage));
    }

    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(state->overall_progress_bar), "Overall progress");
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(state->stage_progress_bar), stage_text);
    status_text = g_strdup(stage_text);
    set_status(state, status_text);
    g_free(status_text);
    g_free(stage_text);
    g_free(base_name);
}

static void clear_object_ptr(gpointer *object_ptr)
{
    if (object_ptr != NULL && *object_ptr != NULL) {
        g_object_unref(*object_ptr);
        *object_ptr = NULL;
    }
}

static gboolean text_matches_query(const gchar *text, const gchar *query_folded)
{
    gchar *text_folded;
    gboolean matches;

    if (query_folded == NULL || query_folded[0] == '\0') {
        return TRUE;
    }
    if (text == NULL || text[0] == '\0') {
        return FALSE;
    }

    text_folded = g_utf8_casefold(text, -1);
    matches = strstr(text_folded, query_folded) != NULL;
    g_free(text_folded);
    return matches;
}

static void index_progress_update_free(IndexProgressUpdate *update)
{
    if (update == NULL) {
        return;
    }
    app_state_unref(update->state);
    g_free(update->path);
    g_free(update);
}

static void index_result_free(IndexResult *result)
{
    if (result == NULL) {
        return;
    }
    if (result->index != NULL) {
        odysia_index_free(result->index);
    }
    app_state_unref(result->state);
    g_free(result->error_message);
    g_free(result->root_path);
    g_free(result);
}

static void index_job_free(IndexJob *job)
{
    if (job == NULL) {
        return;
    }
    app_state_unref(job->state);
    g_free(job->root_path);
    g_free(job);
}

static void load_result_free(LoadResult *result)
{
    if (result == NULL) {
        return;
    }
    if (result->index != NULL) {
        odysia_index_free(result->index);
    }
    app_state_unref(result->state);
    g_free(result->error_message);
    g_free(result->filename);
    g_free(result);
}

static void load_job_free(LoadJob *job)
{
    if (job == NULL) {
        return;
    }
    app_state_unref(job->state);
    g_free(job->filename);
    g_free(job);
}

static gboolean symbol_matches_query(OdysiaSymbol *symbol, const gchar *query_folded)
{
    return text_matches_query(symbol->display_name, query_folded) ||
           text_matches_query(symbol->file_path, query_folded) ||
           text_matches_query(symbol->signature, query_folded) ||
           text_matches_query(symbol->type_text, query_folded) ||
           text_matches_query(symbol->documentation, query_folded) ||
           text_matches_query(odysia_symbol_kind_name(symbol->kind), query_folded);
}

static void tree_build_task_free(TreeBuildTask *task)
{
    guint index;

    if (task == NULL) {
        return;
    }
    for (index = 0; index < G_N_ELEMENTS(task->categories); index++) {
        if (task->categories[index] != NULL) {
            g_ptr_array_free(task->categories[index], TRUE);
        }
    }
    if (task->current_children != NULL) {
        g_ptr_array_free(task->current_children, TRUE);
    }
    if (task->letter_iters != NULL) {
        g_hash_table_destroy(task->letter_iters);
    }
    if (task->category_iters != NULL) {
        g_hash_table_destroy(task->category_iters);
    }
    if (task->language_iters != NULL) {
        g_hash_table_destroy(task->language_iters);
    }
    if (task->documented_names != NULL) {
        g_hash_table_destroy(task->documented_names);
    }
    app_state_unref(task->state);
    g_free(task->query_folded);
    g_free(task);
}

static gboolean symbol_or_child_matches(AppState *state,
                                        OdysiaSymbol *symbol,
                                        gboolean include_children,
                                        const gchar *query_folded)
{
    guint child_index;

    if (query_folded == NULL || query_folded[0] == '\0') {
        return TRUE;
    }
    if (symbol_matches_query(symbol, query_folded)) {
        return TRUE;
    }
    if (!include_children) {
        return FALSE;
    }
    for (child_index = 0; child_index < symbol->children->len; child_index++) {
        OdysiaSymbol *child;

        child = odysia_index_get_symbol(state->index, g_ptr_array_index(symbol->children, child_index));
        if (child != NULL && symbol_matches_query(child, query_folded)) {
            return TRUE;
        }
    }
    return FALSE;
}

static GPtrArray *collect_category_matches(AppState *state,
                                           OdysiaSymbolKind kind,
                                           gboolean include_children,
                                           const gchar *query_folded,
                                           TreeSortMode sort_mode)
{
    GPtrArray *matches;
    guint index_symbol;

    matches = g_ptr_array_new();
    for (index_symbol = 0; index_symbol < state->index->symbols->len; index_symbol++) {
        OdysiaSymbol *symbol;

        symbol = g_ptr_array_index(state->index->symbols, index_symbol);
        if (symbol->kind != kind || symbol->parent_id != NULL) {
            continue;
        }
        if (!symbol_or_child_matches(state, symbol, include_children, query_folded)) {
            continue;
        }
        g_ptr_array_add(matches, symbol);
    }
    g_ptr_array_sort_with_data(matches, symbol_sort_func, GINT_TO_POINTER(sort_mode));
    return matches;
}

static GtkTreeIter ensure_language_iter(TreeBuildTask *task, const gchar *language)
{
    GtkTreeIter *cached_iter;

    cached_iter = g_hash_table_lookup(task->language_iters, language);
    if (cached_iter == NULL) {
        cached_iter = g_new(GtkTreeIter, 1);
        *cached_iter = append_row(task->state,
                                  NULL,
                                  "text-x-script-symbolic",
                                  language,
                                  "Language",
                                  "",
                                  NULL,
                                  700);
        g_hash_table_insert(task->language_iters, g_strdup(language), cached_iter);
    }
    return *cached_iter;
}

static GtkTreeIter ensure_category_iter(TreeBuildTask *task,
                                        const gchar *language,
                                        guint category_index)
{
    GtkTreeIter *cached_iter;
    GtkTreeIter language_iter;
    gchar *cache_key;

    cache_key = g_strdup_printf("%s:%u", language, category_index);
    cached_iter = g_hash_table_lookup(task->category_iters, cache_key);
    if (cached_iter == NULL) {
        language_iter = ensure_language_iter(task, language);
        cached_iter = g_new(GtkTreeIter, 1);
        *cached_iter = append_row(task->state,
                                  &language_iter,
                                  tree_category_icons[category_index],
                                  tree_category_titles[category_index],
                                  "Category",
                                  "",
                                  NULL,
                                  650);
        g_hash_table_insert(task->category_iters, cache_key, cached_iter);
    } else {
        g_free(cache_key);
    }
    return *cached_iter;
}

static GtkTreeIter ensure_letter_iter(TreeBuildTask *task,
                                      guint category_index,
                                      const gchar *language,
                                      const OdysiaSymbol *symbol)
{
    GtkTreeIter *cached_iter;
    GtkTreeIter category_iter;
    gchar letter_text[2];
    gchar *cache_key;
    gchar first_character;

    first_character = symbol != NULL && symbol->display_name != NULL
                      ? g_ascii_toupper(symbol->display_name[0])
                      : '#';
    if (!g_ascii_isalpha(first_character)) {
        first_character = '#';
    }
    letter_text[0] = first_character;
    letter_text[1] = '\0';
    cache_key = g_strdup_printf("%s:%u:%c", language, category_index, first_character);
    cached_iter = g_hash_table_lookup(task->letter_iters, cache_key);
    if (cached_iter == NULL) {
        category_iter = ensure_category_iter(task, language, category_index);
        cached_iter = g_new(GtkTreeIter, 1);
        *cached_iter = append_row(task->state,
                                  &category_iter,
                                  "folder-symbolic",
                                  letter_text,
                                  "Letter",
                                  "",
                                  NULL,
                                  600);
        g_hash_table_insert(task->letter_iters, cache_key, cached_iter);
    } else {
        g_free(cache_key);
    }
    return *cached_iter;
}

static gboolean tree_build_step(gpointer user_data)
{
    TreeBuildTask *task;
    guint work_budget;

    task = user_data;
    if (g_atomic_int_get(&task->state->shutting_down)) {
        task->state->tree_build_source_id = 0;
        tree_build_task_free(task);
        return G_SOURCE_REMOVE;
    }

    work_budget = 80;
    while (work_budget > 0 && task->category_index < G_N_ELEMENTS(task->categories)) {
        GPtrArray *matches;

        matches = task->categories[task->category_index];
        if (task->symbol_index >= matches->len) {
            if (task->current_children != NULL) {
                g_ptr_array_free(task->current_children, TRUE);
                task->current_children = NULL;
            }
            task->category_index++;
            task->symbol_index = 0;
            task->child_index = 0;
            task->parent_inserted = FALSE;
            continue;
        }

        {
            OdysiaSymbol *symbol;
            gboolean include_children;

            symbol = g_ptr_array_index(matches, task->symbol_index);
            include_children = task->category_index <= 3;

            if (!task->parent_inserted) {
                GtkTreePath *path;
                gchar *location_text;
                gchar *tree_title;
                GtkTreeIter letter_iter;
                const gchar *language;

                location_text = symbol_location_text(symbol);
                tree_title = symbol_tree_title(symbol);
                language = odysia_symbol_language_name(symbol);
                letter_iter = ensure_letter_iter(task, task->category_index, language, symbol);
                task->current_parent_iter = append_row(task->state,
                                                       &letter_iter,
                                                       symbol_icon_name(symbol->kind),
                                                       tree_title,
                                                       odysia_symbol_kind_name(symbol->kind),
                                                       location_text,
                                                       symbol->id,
                                                       400);
                set_row_documentation_icon(task, &task->current_parent_iter, symbol);
                g_free(tree_title);
                g_free(location_text);
                path = gtk_tree_model_get_path(GTK_TREE_MODEL(task->state->tree_store), &task->current_parent_iter);
                g_hash_table_insert(task->state->row_paths, g_strdup(symbol->id), gtk_tree_path_to_string(path));
                gtk_tree_path_free(path);
                task->current_parent_matches = symbol_matches_query(symbol, task->query_folded);
                task->current_children = g_ptr_array_new();
                if (include_children) {
                    guint child_index;

                    for (child_index = 0; child_index < symbol->children->len; child_index++) {
                        OdysiaSymbol *child;

                        child = odysia_index_get_symbol(task->state->index,
                                                        g_ptr_array_index(symbol->children, child_index));
                        if (child == NULL) {
                            continue;
                        }
                        if (task->query_folded[0] != '\0' &&
                            !task->current_parent_matches &&
                            !symbol_matches_query(child, task->query_folded)) {
                            continue;
                        }
                        g_ptr_array_add(task->current_children, child);
                    }
                    g_ptr_array_sort_with_data(task->current_children,
                                               symbol_sort_func,
                                               GINT_TO_POINTER(task->sort_mode));
                }
                task->parent_inserted = TRUE;
                work_budget--;
            }

            while (include_children && task->child_index < task->current_children->len && work_budget > 0) {
                OdysiaSymbol *child;

                child = g_ptr_array_index(task->current_children, task->child_index);
                task->child_index++;

                {
                    GtkTreeIter child_iter;
                    GtkTreePath *child_path;
                    gchar *location_text;
                    gchar *tree_title;

                    location_text = symbol_location_text(child);
                    tree_title = symbol_tree_title(child);
                    child_iter = append_row(task->state,
                                            &task->current_parent_iter,
                                            symbol_icon_name(child->kind),
                                            tree_title,
                                            odysia_symbol_kind_name(child->kind),
                                            location_text,
                                            child->id,
                                            400);
                    set_row_documentation_icon(task, &child_iter, child);
                    g_free(tree_title);
                    g_free(location_text);
                    child_path = gtk_tree_model_get_path(GTK_TREE_MODEL(task->state->tree_store), &child_iter);
                    g_hash_table_insert(task->state->row_paths, g_strdup(child->id), gtk_tree_path_to_string(child_path));
                    gtk_tree_path_free(child_path);
                }
                work_budget--;
            }

            if (!include_children || task->child_index >= task->current_children->len) {
                task->completed_roots++;
                set_index_stage_progress(task->state,
                                         ODYSIA_INDEX_STAGE_BUILD_TREE,
                                         task->completed_roots,
                                         task->total_roots,
                                         symbol->file_path);
                task->symbol_index++;
                task->child_index = 0;
                task->parent_inserted = FALSE;
                g_ptr_array_free(task->current_children, TRUE);
                task->current_children = NULL;
            }
        }
    }

    if (task->category_index >= G_N_ELEMENTS(task->categories)) {
        GHashTableIter language_iterator;
        gpointer language_iter_value;

        g_hash_table_iter_init(&language_iterator, task->language_iters);
        while (g_hash_table_iter_next(&language_iterator, NULL, &language_iter_value)) {
            GtkTreePath *root_path;

            root_path = gtk_tree_model_get_path(GTK_TREE_MODEL(task->state->tree_store),
                                                language_iter_value);
            gtk_tree_view_expand_row(GTK_TREE_VIEW(task->state->tree_view), root_path, FALSE);
            gtk_tree_path_free(root_path);
        }
        task->state->tree_build_source_id = 0;
        task->state->indexing = FALSE;
        clear_object_ptr((gpointer *) &task->state->index_cancel);
        gtk_widget_set_sensitive(task->state->overall_progress_bar, FALSE);
        gtk_widget_set_sensitive(task->state->stage_progress_bar, FALSE);
        set_indexing_controls_sensitive(task->state, TRUE);
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(task->state->overall_progress_bar), 1.0);
        gtk_progress_bar_set_text(GTK_PROGRESS_BAR(task->state->overall_progress_bar), "Overall progress");
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(task->state->stage_progress_bar), 1.0);
        gtk_progress_bar_set_text(GTK_PROGRESS_BAR(task->state->stage_progress_bar), "Building symbol tree complete");
        tree_build_task_free(task);
        return G_SOURCE_REMOVE;
    }

    return G_SOURCE_CONTINUE;
}

static GtkTreeIter append_row(AppState *state,
                              GtkTreeIter *parent,
                              const gchar *icon_name,
                              const gchar *title,
                              const gchar *kind,
                              const gchar *location,
                              const gchar *symbol_id,
                              gint weight)
{
    GtkTreeIter iter;

    gtk_tree_store_append(state->tree_store, &iter, parent);
    gtk_tree_store_set(state->tree_store,
                       &iter,
                       COLUMN_ICON_NAME, icon_name,
                       COLUMN_TITLE, title,
                       COLUMN_KIND, kind,
                       COLUMN_LOCATION, location,
                       COLUMN_SYMBOL_ID, symbol_id,
                       COLUMN_WEIGHT, weight,
                       -1);
    return iter;
}

static gint symbol_sort_func(gconstpointer left_ptr, gconstpointer right_ptr, gpointer user_data)
{
    const OdysiaSymbol *left;
    const OdysiaSymbol *right;
    TreeSortMode sort_mode;
    gint result;

    left = *(const OdysiaSymbol * const *) left_ptr;
    right = *(const OdysiaSymbol * const *) right_ptr;
    sort_mode = (TreeSortMode) GPOINTER_TO_INT(user_data);

    if (sort_mode == TREE_SORT_LOCATION) {
        result = g_ascii_strcasecmp(left->file_path, right->file_path);
        if (result != 0) {
            return result;
        }
        if (left->line != right->line) {
            return left->line < right->line ? -1 : 1;
        }
    } else if (sort_mode == TREE_SORT_KIND) {
        result = g_ascii_strcasecmp(odysia_symbol_kind_name(left->kind),
                                    odysia_symbol_kind_name(right->kind));
        if (result != 0) {
            return result;
        }
    }
    return g_ascii_strcasecmp(left->display_name, right->display_name);
}

static gchar *resolve_symbol_target(AppState *state, const gchar *name)
{
    OdysiaSymbol *symbol;
    gchar *candidate;

    symbol = odysia_index_resolve_name(state->index, name, ODYSIA_SYMBOL_FUNCTION);
    if (symbol == NULL) {
        symbol = odysia_index_resolve_name(state->index, name, ODYSIA_SYMBOL_STRUCT);
    }
    if (symbol == NULL) {
        symbol = odysia_index_resolve_name(state->index, name, ODYSIA_SYMBOL_TYPEDEF);
    }
    if (symbol == NULL) {
        symbol = odysia_index_resolve_name(state->index, name, ODYSIA_SYMBOL_ENUM);
    }
    if (symbol == NULL) {
        symbol = odysia_index_resolve_name(state->index, name, ODYSIA_SYMBOL_ENUMERATOR);
    }
    if (symbol == NULL) {
        symbol = odysia_index_resolve_name(state->index, name, ODYSIA_SYMBOL_GLOBAL_VARIABLE);
    }
    if (symbol != NULL) {
        return g_strdup(symbol->id);
    }

    candidate = odysia_extract_type_candidate(name);
    if (candidate == NULL) {
        return NULL;
    }
    symbol = odysia_index_resolve_name(state->index, candidate, ODYSIA_SYMBOL_TYPEDEF);
    if (symbol == NULL) {
        symbol = odysia_index_resolve_name(state->index, candidate, ODYSIA_SYMBOL_STRUCT);
    }
    g_free(candidate);
    return symbol != NULL ? g_strdup(symbol->id) : NULL;
}

static void buffer_append_plain(GtkTextBuffer *buffer, const gchar *text)
{
    GtkTextIter iter;

    gtk_text_buffer_get_end_iter(buffer, &iter);
    gtk_text_buffer_insert(buffer, &iter, text, -1);
}

static void buffer_append_link(AppState *state, GtkTextBuffer *buffer, const gchar *text, const gchar *target_id)
{
    GtkTextIter start;
    GtkTextIter end;
    LinkRange *range;

    gtk_text_buffer_get_end_iter(buffer, &start);
    gtk_text_buffer_insert(buffer, &start, text, -1);
    gtk_text_buffer_get_end_iter(buffer, &end);
    gtk_text_buffer_apply_tag(buffer, state->link_tag, &start, &end);

    range = g_new0(LinkRange, 1);
    range->start_offset = gtk_text_iter_get_offset(&start);
    range->end_offset = gtk_text_iter_get_offset(&end);
    range->target_id = g_strdup(target_id);
    g_ptr_array_add(state->detail_links, range);
}

static void apply_tag_to_range(GtkTextBuffer *buffer, GtkTextTag *tag, gint start_offset, gint end_offset)
{
    GtkTextIter start;
    GtkTextIter end;

    if (tag == NULL || start_offset >= end_offset) {
        return;
    }

    gtk_text_buffer_get_iter_at_offset(buffer, &start, start_offset);
    gtk_text_buffer_get_iter_at_offset(buffer, &end, end_offset);
    gtk_text_buffer_apply_tag(buffer, tag, &start, &end);
}

static void apply_source_highlighting(AppState *state, GtkTextBuffer *buffer, const gchar *text)
{
    gint offset;
    gboolean in_string;
    gboolean in_char;

    gtk_text_buffer_set_text(buffer, text != NULL ? text : "", -1);
    if (text == NULL) {
        return;
    }

    offset = 0;
    in_string = FALSE;
    in_char = FALSE;
    while (text[offset] != '\0') {
        gchar current;
        gchar next;

        current = text[offset];
        next = text[offset + 1];

        if (!in_string && !in_char && current == '/' && next == '/') {
            gint start;

            start = offset;
            offset += 2;
            while (text[offset] != '\0' && text[offset] != '\n') {
                offset++;
            }
            apply_tag_to_range(buffer, state->source_comment_tag, start, offset);
            continue;
        }
        if (!in_string && !in_char && current == '/' && next == '*') {
            gint start;

            start = offset;
            offset += 2;
            while (text[offset] != '\0') {
                if (text[offset] == '*' && text[offset + 1] == '/') {
                    offset += 2;
                    break;
                }
                offset++;
            }
            apply_tag_to_range(buffer, state->source_comment_tag, start, offset);
            continue;
        }
        if (!in_char && current == '"') {
            gint start;

            start = offset;
            offset++;
            in_string = TRUE;
            while (text[offset] != '\0') {
                if (text[offset] == '\\' && text[offset + 1] != '\0') {
                    offset += 2;
                    continue;
                }
                if (text[offset] == '"') {
                    offset++;
                    in_string = FALSE;
                    break;
                }
                offset++;
            }
            apply_tag_to_range(buffer, state->source_string_tag, start, offset);
            continue;
        }
        if (!in_string && current == '\'') {
            gint start;

            start = offset;
            offset++;
            in_char = TRUE;
            while (text[offset] != '\0') {
                if (text[offset] == '\\' && text[offset + 1] != '\0') {
                    offset += 2;
                    continue;
                }
                if (text[offset] == '\'') {
                    offset++;
                    in_char = FALSE;
                    break;
                }
                offset++;
            }
            apply_tag_to_range(buffer, state->source_string_tag, start, offset);
            continue;
        }
        if (current == '#' && (offset == 0 || text[offset - 1] == '\n')) {
            gint start;

            start = offset;
            while (text[offset] != '\0' && text[offset] != '\n') {
                offset++;
            }
            apply_tag_to_range(buffer, state->source_preproc_tag, start, offset);
            continue;
        }
        if (g_ascii_isdigit(current)) {
            gint start;

            start = offset;
            offset++;
            while (g_ascii_isalnum(text[offset]) || text[offset] == '_' || text[offset] == '.' || text[offset] == 'x') {
                offset++;
            }
            apply_tag_to_range(buffer, state->source_number_tag, start, offset);
            continue;
        }
        if (g_ascii_isalpha(current) || current == '_') {
            gint start;
            gchar *token;

            start = offset;
            offset++;
            while (g_ascii_isalnum(text[offset]) || text[offset] == '_') {
                offset++;
            }

            token = g_strndup(text + start, offset - start);
            if (token_in_set(token, source_c_keywords, G_N_ELEMENTS(source_c_keywords))) {
                apply_tag_to_range(buffer, state->source_keyword_tag, start, offset);
            } else if (token_in_set(token, source_kernel_keywords, G_N_ELEMENTS(source_kernel_keywords))) {
                apply_tag_to_range(buffer, state->source_kernel_tag, start, offset);
            }
            g_free(token);
            continue;
        }

        offset++;
    }
}

static void select_symbol_row(AppState *state, const gchar *symbol_id);

static void append_related_symbol(AppState *state,
                                  GtkTextBuffer *buffer,
                                  const gchar *prefix,
                                  const gchar *text,
                                  const gchar *target_name)
{
    gchar *target_id;

    buffer_append_plain(buffer, prefix);
    target_id = resolve_symbol_target(state, target_name);
    if (target_id != NULL) {
        buffer_append_link(state, buffer, text, target_id);
        g_free(target_id);
    } else {
        buffer_append_plain(buffer, text);
    }
    buffer_append_plain(buffer, "\n");
}

static void render_symbol(AppState *state, OdysiaSymbol *symbol)
{
    GtkTextBuffer *detail_buffer;
    GtkTextBuffer *source_buffer;
    guint child_index;
    guint relation_index;
    gchar *docs;

    detail_buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(state->detail_view));
    source_buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(state->source_view));
    gtk_text_buffer_set_text(detail_buffer, "", -1);
    gtk_text_buffer_set_text(source_buffer, "", -1);
    apply_source_highlighting(state, source_buffer, symbol->snippet != NULL ? symbol->snippet : "");
    g_ptr_array_set_size(state->detail_links, 0);

    buffer_append_tagged(detail_buffer, state->header_tag, odysia_symbol_kind_name(symbol->kind));
    buffer_append_tagged(detail_buffer, state->header_tag, ": ");
    buffer_append_tagged(detail_buffer, state->header_tag, symbol->display_name);
    buffer_append_plain(detail_buffer, "\n\n");

    if (symbol->signature != NULL && symbol->signature[0] != '\0') {
        buffer_append_tagged(detail_buffer, state->section_tag, "Signature\n");
        buffer_append_plain(detail_buffer, symbol->signature);
        buffer_append_plain(detail_buffer, "\n\n");
    }

    if (symbol->type_text != NULL && symbol->type_text[0] != '\0') {
        buffer_append_tagged(detail_buffer, state->section_tag, "Type\n");
        append_related_symbol(state, detail_buffer, "Type\n", symbol->type_text, symbol->type_text);
        buffer_append_plain(detail_buffer, "\n");
    }

    buffer_append_tagged(detail_buffer, state->section_tag, "Location\n");
    {
        gchar *dir_name;
        gchar *base_name;

        dir_name = g_path_get_dirname(symbol->file_path != NULL ? symbol->file_path : "");
        base_name = g_path_get_basename(symbol->file_path != NULL ? symbol->file_path : "");
        buffer_append_plain(detail_buffer, "Directory: ");
        buffer_append_plain(detail_buffer, dir_name);
        buffer_append_plain(detail_buffer, "\n");
        buffer_append_plain(detail_buffer, "Source file: ");
        buffer_append_plain(detail_buffer, base_name);
        buffer_append_plain(detail_buffer, "\n");
        g_free(base_name);
        g_free(dir_name);
    }
    buffer_append_plain(detail_buffer, "Line: ");
    {
        gchar *line_text;

        line_text = g_strdup_printf("%d", symbol->line);
        buffer_append_plain(detail_buffer, line_text);
        buffer_append_plain(detail_buffer, "\n\n");
        g_free(line_text);
    }

    if (symbol->documentation != NULL && symbol->documentation[0] != '\0') {
        buffer_append_tagged(detail_buffer, state->section_tag, "Inline Documentation\n");
        buffer_append_plain(detail_buffer, symbol->documentation);
        buffer_append_plain(detail_buffer, "\n\n");
    }

    if (symbol->children->len > 0) {
        buffer_append_tagged(detail_buffer, state->section_tag, "Contains\n");
        for (child_index = 0; child_index < symbol->children->len; child_index++) {
            OdysiaSymbol *child;
            gchar *child_id;
            gchar *prefix;

            child_id = g_ptr_array_index(symbol->children, child_index);
            child = odysia_index_get_symbol(state->index, child_id);
            if (child == NULL) {
                continue;
            }
            prefix = g_strdup_printf("- %s: ", odysia_symbol_kind_name(child->kind));
            buffer_append_plain(detail_buffer, prefix);
            buffer_append_link(state, detail_buffer, child->display_name, child->id);
            if (child->type_text != NULL && child->type_text[0] != '\0') {
                buffer_append_plain(detail_buffer, " (type: ");
                {
                    gchar *target_id;

                    target_id = resolve_symbol_target(state, child->type_text);
                    if (target_id != NULL) {
                        buffer_append_link(state, detail_buffer, child->type_text, target_id);
                        g_free(target_id);
                    } else {
                        buffer_append_plain(detail_buffer, child->type_text);
                    }
                }
                buffer_append_plain(detail_buffer, ")");
            }
            buffer_append_plain(detail_buffer, "\n");
            g_free(prefix);
        }
        buffer_append_plain(detail_buffer, "\n");
    }

    if (symbol->relations->len > 0) {
        buffer_append_tagged(detail_buffer, state->section_tag, "Relations\n");
        for (relation_index = 0; relation_index < symbol->relations->len; relation_index++) {
            OdysiaRelation *relation;
            gchar *prefix;

            relation = g_ptr_array_index(symbol->relations, relation_index);
            prefix = g_strdup_printf("- %s: ", relation->label);
            append_related_symbol(state,
                                  detail_buffer,
                                  prefix,
                                  relation->detail != NULL && relation->detail[0] != '\0' ? relation->detail : relation->target_name,
                                  relation->target_name);
            g_free(prefix);
        }
        buffer_append_plain(detail_buffer, "\n");
    }

    docs = odysia_index_collect_external_docs(state->index, symbol->name);
    if (docs != NULL && docs[0] != '\0') {
        buffer_append_tagged(detail_buffer, state->section_tag, "Documentation Files\n");
        buffer_append_plain(detail_buffer, docs);
        buffer_append_plain(detail_buffer, "\n");
    }
    g_free(docs);
}

static gboolean detail_view_button_release(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
    AppState *state;
    GtkTextIter iter;
    gint buffer_x;
    gint buffer_y;
    guint index;
    gint offset;

    if (event->button != GDK_BUTTON_PRIMARY) {
        return FALSE;
    }

    state = user_data;
    gtk_text_view_window_to_buffer_coords(GTK_TEXT_VIEW(widget), GTK_TEXT_WINDOW_WIDGET, (gint) event->x, (gint) event->y, &buffer_x, &buffer_y);
    gtk_text_view_get_iter_at_location(GTK_TEXT_VIEW(widget), &iter, buffer_x, buffer_y);
    offset = gtk_text_iter_get_offset(&iter);
    for (index = 0; index < state->detail_links->len; index++) {
        LinkRange *range;

        range = g_ptr_array_index(state->detail_links, index);
        if (offset >= range->start_offset && offset <= range->end_offset) {
            select_symbol_row(state, range->target_id);
            return TRUE;
        }
    }

    return FALSE;
}

static void on_tree_selection_changed(GtkTreeSelection *selection, gpointer user_data)
{
    AppState *state;
    GtkTreeModel *model;
    GtkTreeIter iter;
    gchar *symbol_id;

    state = user_data;
    if (!gtk_tree_selection_get_selected(selection, &model, &iter)) {
        return;
    }
    symbol_id = NULL;
    gtk_tree_model_get(model, &iter, COLUMN_SYMBOL_ID, &symbol_id, -1);
    if (symbol_id != NULL && state->index != NULL) {
        OdysiaSymbol *symbol;

        symbol = odysia_index_get_symbol(state->index, symbol_id);
        if (symbol != NULL) {
            gchar *dir_name;
            gchar *base_name;
            gchar *status_text;

            render_symbol(state, symbol);
            dir_name = g_path_get_dirname(symbol->file_path != NULL ? symbol->file_path : "");
            base_name = g_path_get_basename(symbol->file_path != NULL ? symbol->file_path : "");
            status_text = g_strdup_printf("%s selected: %s/%s:%d",
                                          odysia_symbol_kind_name(symbol->kind),
                                          dir_name,
                                          base_name,
                                          symbol->line);
            set_status(state, status_text);
            g_free(status_text);
            g_free(base_name);
            g_free(dir_name);
        }
    }
    g_free(symbol_id);
}

static void rebuild_tree(AppState *state)
{
    TreeBuildTask *task;
    const gchar *query_text;
    guint category_index;
    GHashTable *languages;
    GList *language_names;
    GList *language_node;

    if (state->tree_build_source_id != 0) {
        g_source_remove(state->tree_build_source_id);
        state->tree_build_source_id = 0;
    }

    gtk_tree_store_clear(state->tree_store);
    g_hash_table_remove_all(state->row_paths);

    task = g_new0(TreeBuildTask, 1);
    task->state = app_state_ref(state);
    task->sort_mode = (TreeSortMode) gtk_combo_box_get_active(GTK_COMBO_BOX(state->sort_combo));
    if (task->sort_mode < TREE_SORT_NAME || task->sort_mode > TREE_SORT_KIND) {
        task->sort_mode = TREE_SORT_NAME;
    }
    task->language_iters = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    task->category_iters = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    task->letter_iters = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    task->documented_names = collect_documented_names(state->index);
    query_text = gtk_entry_get_text(GTK_ENTRY(state->search_entry));
    task->query_folded = g_utf8_casefold(query_text != NULL ? query_text : "", -1);
    task->categories[0] = collect_category_matches(state, ODYSIA_SYMBOL_FUNCTION, TRUE, task->query_folded, task->sort_mode);
    task->categories[1] = collect_category_matches(state, ODYSIA_SYMBOL_STRUCT, TRUE, task->query_folded, task->sort_mode);
    task->categories[2] = collect_category_matches(state, ODYSIA_SYMBOL_UNION, TRUE, task->query_folded, task->sort_mode);
    task->categories[3] = collect_category_matches(state, ODYSIA_SYMBOL_ENUM, TRUE, task->query_folded, task->sort_mode);
    task->categories[4] = collect_category_matches(state, ODYSIA_SYMBOL_TYPEDEF, FALSE, task->query_folded, task->sort_mode);
    task->categories[5] = collect_category_matches(state, ODYSIA_SYMBOL_GLOBAL_VARIABLE, FALSE, task->query_folded, task->sort_mode);
    task->categories[6] = collect_category_matches(state, ODYSIA_SYMBOL_MODULE, FALSE, task->query_folded, task->sort_mode);
    task->categories[7] = collect_category_matches(state, ODYSIA_SYMBOL_CLASS, FALSE, task->query_folded, task->sort_mode);
    task->categories[8] = collect_category_matches(state, ODYSIA_SYMBOL_TRAIT, FALSE, task->query_folded, task->sort_mode);
    task->categories[9] = collect_category_matches(state, ODYSIA_SYMBOL_MACRO, FALSE, task->query_folded, task->sort_mode);
    task->categories[10] = collect_category_matches(state, ODYSIA_SYMBOL_LABEL, FALSE, task->query_folded, task->sort_mode);
    task->categories[11] = collect_category_matches(state, ODYSIA_SYMBOL_CONFIG, FALSE, task->query_folded, task->sort_mode);
    task->categories[12] = collect_category_matches(state, ODYSIA_SYMBOL_BUILD_TARGET, FALSE, task->query_folded, task->sort_mode);
    task->categories[13] = collect_category_matches(state, ODYSIA_SYMBOL_DEVICE_NODE, FALSE, task->query_folded, task->sort_mode);
    task->categories[14] = collect_category_matches(state, ODYSIA_SYMBOL_RULE, FALSE, task->query_folded, task->sort_mode);
    task->total_roots = 0;
    for (category_index = 0; category_index < G_N_ELEMENTS(task->categories); category_index++) {
        task->total_roots += task->categories[category_index]->len;
    }

    languages = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    for (category_index = 0; category_index < G_N_ELEMENTS(task->categories); category_index++) {
        guint symbol_index;

        for (symbol_index = 0; symbol_index < task->categories[category_index]->len; symbol_index++) {
            OdysiaSymbol *symbol;

            symbol = g_ptr_array_index(task->categories[category_index], symbol_index);
            g_hash_table_add(languages, g_strdup(odysia_symbol_language_name(symbol)));
        }
    }
    language_names = g_hash_table_get_keys(languages);
    language_names = g_list_sort(language_names, (GCompareFunc) g_strcmp0);
    for (language_node = language_names; language_node != NULL; language_node = language_node->next) {
        ensure_language_iter(task, language_node->data);
    }
    g_list_free(language_names);
    g_hash_table_destroy(languages);

    gtk_widget_set_sensitive(state->overall_progress_bar, TRUE);
    gtk_widget_set_sensitive(state->stage_progress_bar, TRUE);
    set_index_stage_progress(state, ODYSIA_INDEX_STAGE_BUILD_TREE, 0, task->total_roots, NULL);
    state->tree_build_source_id = g_idle_add(tree_build_step, task);
}

static void append_build_log(AppState *state, const gchar *line)
{
    GtkTextIter iter;
    GtkTextIter end;
    GtkTextMark *mark;

    if (state->build_log_buffer == NULL) {
        return;
    }

    gtk_text_buffer_get_end_iter(state->build_log_buffer, &iter);
    gtk_text_buffer_insert(state->build_log_buffer, &iter, line, -1);
    gtk_text_buffer_insert(state->build_log_buffer, &iter, "\n", -1);
    gtk_text_buffer_get_end_iter(state->build_log_buffer, &end);
    mark = gtk_text_buffer_create_mark(state->build_log_buffer, NULL, &end, FALSE);
    gtk_text_view_scroll_mark_onscreen(GTK_TEXT_VIEW(state->build_log_view), mark);
    gtk_text_buffer_delete_mark(state->build_log_buffer, mark);
}

static gchar *extract_current_build_file(const gchar *line)
{
    gchar **tokens;
    gchar *result;
    gint index;

    tokens = g_strsplit_set(line, " \t", -1);
    result = NULL;
    for (index = 0; tokens[index] != NULL; index++) {
        if (tokens[index][0] == '\0') {
            continue;
        }
        if (g_str_has_suffix(tokens[index], ".c") ||
            g_str_has_suffix(tokens[index], ".h") ||
            g_str_has_suffix(tokens[index], ".S") ||
            g_str_has_suffix(tokens[index], ".s") ||
            g_str_has_suffix(tokens[index], ".rs") ||
            g_str_has_suffix(tokens[index], ".o")) {
            result = g_strdup(tokens[index]);
            break;
        }
    }
    if (result == NULL && tokens[0] != NULL && tokens[1] != NULL && tokens[1][0] != '\0') {
        if (g_str_equal(tokens[0], "CC") || g_str_equal(tokens[0], "HOSTCC") || g_str_equal(tokens[0], "LD") || g_str_equal(tokens[0], "AR")) {
            result = g_strdup(tokens[1]);
        }
    }
    g_strfreev(tokens);
    return result;
}

static void update_build_file_label(AppState *state, const gchar *line)
{
    gchar *current_file;
    gchar *label_text;

    current_file = extract_current_build_file(line);
    if (current_file == NULL) {
        return;
    }

    label_text = g_strdup_printf("Compiling: %s", current_file);
    gtk_label_set_text(GTK_LABEL(state->build_file_label), label_text);
    g_free(label_text);
    g_free(current_file);
}

static void finish_build_ui(AppState *state, const gchar *message, gboolean success)
{
    gtk_widget_set_sensitive(state->build_stop_button, FALSE);
    gtk_widget_set_sensitive(state->build_close_button, TRUE);
    set_status(state, message);
    if (!success) {
        gtk_label_set_text(GTK_LABEL(state->build_file_label), "Build stopped or failed.");
    }
}

static gboolean apply_index_progress_update(gpointer user_data)
{
    IndexProgressUpdate *update;

    update = user_data;
    if (g_atomic_int_get(&update->state->shutting_down) || update->state->overall_progress_bar == NULL) {
        index_progress_update_free(update);
        return G_SOURCE_REMOVE;
    }

    set_index_stage_progress(update->state,
                             update->stage,
                             update->current,
                             update->total,
                             update->path);
    index_progress_update_free(update);
    return G_SOURCE_REMOVE;
}

static void index_progress_callback(OdysiaIndexStage stage,
                                    guint current,
                                    guint total,
                                    const gchar *path,
                                    gpointer user_data)
{
    IndexJob *job;
    IndexProgressUpdate *update;

    job = user_data;
    if (g_atomic_int_get(&job->state->shutting_down)) {
        return;
    }
    update = g_new0(IndexProgressUpdate, 1);
    update->state = app_state_ref(job->state);
    update->stage = stage;
    update->current = current;
    update->total = total;
    update->path = path != NULL ? g_strdup(path) : NULL;
    g_main_context_invoke(NULL, apply_index_progress_update, update);
}

static gboolean apply_index_result(gpointer user_data)
{
    IndexResult *result;
    gchar *summary;

    result = user_data;
    if (g_atomic_int_get(&result->state->shutting_down)) {
        if (result->state->index_thread != NULL) {
            g_thread_unref(result->state->index_thread);
            result->state->index_thread = NULL;
        }
        index_result_free(result);
        return G_SOURCE_REMOVE;
    }
    if (result->state->index_thread != NULL) {
        g_thread_unref(result->state->index_thread);
        result->state->index_thread = NULL;
    }

    if (result->error_message != NULL) {
        result->state->indexing = FALSE;
        clear_object_ptr((gpointer *) &result->state->index_cancel);
        set_indexing_controls_sensitive(result->state, TRUE);
        gtk_widget_set_sensitive(result->state->overall_progress_bar, FALSE);
        gtk_widget_set_sensitive(result->state->stage_progress_bar, FALSE);
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(result->state->overall_progress_bar), 0.0);
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(result->state->stage_progress_bar), 0.0);
        gtk_progress_bar_set_text(GTK_PROGRESS_BAR(result->state->overall_progress_bar), "Overall progress");
        gtk_progress_bar_set_text(GTK_PROGRESS_BAR(result->state->stage_progress_bar), "Indexing failed");
        set_status(result->state, result->error_message);
        index_result_free(result);
        return G_SOURCE_REMOVE;
    }

    if (result->state->index != NULL) {
        odysia_index_free(result->state->index);
    }
    result->state->index = result->index;
    result->index = NULL;
    rebuild_tree(result->state);
    summary = g_strdup_printf("Indexed %u symbols from %s",
                              result->state->index->symbols->len,
                              result->root_path);
    set_status(result->state, summary);
    g_free(summary);
    index_result_free(result);
    return G_SOURCE_REMOVE;
}

static gpointer index_worker_thread(gpointer data)
{
    IndexJob *job;
    IndexResult *result;
    GError *error;

    job = data;
    result = g_new0(IndexResult, 1);
    result->state = app_state_ref(job->state);
    result->root_path = g_strdup(job->root_path);
    error = NULL;
    result->index = odysia_index_build_with_progress(job->root_path,
                                                     index_progress_callback,
                                                     job,
                                                     job->state->index_cancel,
                                                     job->source_thread_count,
                                                     &error);
    if (error != NULL) {
        result->error_message = g_strdup(error->message);
        g_error_free(error);
    }

    g_main_context_invoke(NULL, apply_index_result, result);
    index_job_free(job);
    return NULL;
}

static gboolean apply_load_result(gpointer user_data)
{
    LoadResult *result;
    AppState *state;

    result = user_data;
    state = result->state;
    if (g_atomic_int_get(&state->shutting_down)) {
        if (state->index_thread != NULL) {
            g_thread_unref(state->index_thread);
            state->index_thread = NULL;
        }
        load_result_free(result);
        return G_SOURCE_REMOVE;
    }
    if (state->index_thread != NULL) {
        g_thread_unref(state->index_thread);
        state->index_thread = NULL;
    }
    if (result->error_message != NULL) {
        state->indexing = FALSE;
        set_indexing_controls_sensitive(state, TRUE);
        reset_progress_bars(state, "SQLite load failed");
        set_status(state, result->error_message);
        load_result_free(result);
        return G_SOURCE_REMOVE;
    }

    clear_loaded_index_data(state);
    state->index = result->index;
    result->index = NULL;
    gtk_entry_set_text(GTK_ENTRY(state->path_entry),
                       state->index->root_path != NULL ? state->index->root_path : "");
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(state->overall_progress_bar), 0.9);
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(state->stage_progress_bar), "Building loaded symbol tree...");
    set_status(state, "SQLite index loaded; building symbol tree...");
    rebuild_tree(state);
    load_result_free(result);
    return G_SOURCE_REMOVE;
}

static gpointer load_worker_thread(gpointer data)
{
    LoadJob *job;
    LoadResult *result;
    GError *error;

    job = data;
    result = g_new0(LoadResult, 1);
    result->state = app_state_ref(job->state);
    result->filename = g_strdup(job->filename);
    error = NULL;
    result->index = odysia_index_load_sqlite(job->filename, &error);
    if (error != NULL) {
        result->error_message = g_strdup(error->message);
        g_error_free(error);
    } else if (result->index == NULL) {
        result->error_message = g_strdup("SQLite loading returned no index.");
    }
    g_main_context_invoke(NULL, apply_load_result, result);
    load_job_free(job);
    return NULL;
}

static void on_build_wait_complete(GObject *source_object, GAsyncResult *result, gpointer user_data)
{
    AppState *state;
    GError *error;

    state = user_data;
    error = NULL;
    if (g_atomic_int_get(&state->shutting_down)) {
        clear_object_ptr((gpointer *) &state->build_process);
        clear_object_ptr((gpointer *) &state->build_stream);
        clear_object_ptr((gpointer *) &state->build_cancel);
        app_state_unref(state);
        return;
    }
    if (!g_subprocess_wait_check_finish(G_SUBPROCESS(source_object), result, &error)) {
        gchar *message;

        message = g_strdup_printf("Kernel build did not complete successfully: %s", error->message);
        append_build_log(state, error->message);
        finish_build_ui(state, message, FALSE);
        g_free(message);
        g_error_free(error);
    } else {
        finish_build_ui(state, "Kernel build completed successfully.", TRUE);
        gtk_label_set_text(GTK_LABEL(state->build_file_label), "Build complete.");
    }

    clear_object_ptr((gpointer *) &state->build_process);
    clear_object_ptr((gpointer *) &state->build_stream);
    clear_object_ptr((gpointer *) &state->build_cancel);
    app_state_unref(state);
}

static void read_build_output_line(GObject *source_object, GAsyncResult *result, gpointer user_data)
{
    AppState *state;
    GError *error;
    gsize length;
    gchar *line;

    state = user_data;
    if (g_atomic_int_get(&state->shutting_down)) {
        return;
    }
    error = NULL;
    length = 0;
    line = g_data_input_stream_read_line_finish(G_DATA_INPUT_STREAM(source_object), result, &length, &error);

    if (error != NULL) {
        if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            append_build_log(state, error->message);
        }
        g_error_free(error);
        return;
    }
    if (line == NULL) {
        return;
    }

    (void) length;
    append_build_log(state, line);
    update_build_file_label(state, line);
    g_free(line);

    if (state->build_stream != NULL) {
        g_data_input_stream_read_line_async(state->build_stream,
                                            G_PRIORITY_DEFAULT,
                                            state->build_cancel,
                                            read_build_output_line,
                                            state);
    }
}

static gboolean on_build_dialog_delete(GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
    AppState *state;

    (void) widget;
    (void) event;
    state = user_data;
    if (state->build_process != NULL) {
        g_subprocess_force_exit(state->build_process);
        return TRUE;
    }
    return FALSE;
}

static void on_build_dialog_response(GtkDialog *dialog, gint response_id, gpointer user_data)
{
    AppState *state;

    state = user_data;
    if (response_id == GTK_RESPONSE_CANCEL && state->build_process != NULL) {
        g_subprocess_force_exit(state->build_process);
        return;
    }

    gtk_widget_destroy(GTK_WIDGET(dialog));
    state->build_dialog = NULL;
    state->build_file_label = NULL;
    state->build_log_view = NULL;
    state->build_stop_button = NULL;
    state->build_close_button = NULL;
    state->build_log_buffer = NULL;
}

static void ensure_build_dialog(AppState *state)
{
    GtkWidget *content_area;
    GtkWidget *box;
    GtkWidget *frame;
    GtkWidget *log_view;
    GtkWidget *scroll;

    if (state->build_dialog != NULL) {
        gtk_window_present(GTK_WINDOW(state->build_dialog));
        return;
    }

    state->build_dialog = gtk_dialog_new_with_buttons("Kernel Build",
                                                      GTK_WINDOW(state->window),
                                                      GTK_DIALOG_MODAL,
                                                      NULL,
                                                      NULL);
    state->build_stop_button = gtk_dialog_add_button(GTK_DIALOG(state->build_dialog), "Stop", GTK_RESPONSE_CANCEL);
    state->build_close_button = gtk_dialog_add_button(GTK_DIALOG(state->build_dialog), "Close", GTK_RESPONSE_CLOSE);
    gtk_widget_set_sensitive(state->build_close_button, FALSE);

    content_area = gtk_dialog_get_content_area(GTK_DIALOG(state->build_dialog));
    box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_add(GTK_CONTAINER(content_area), box);

    frame = gtk_frame_new("Current compilation unit");
    gtk_box_pack_start(GTK_BOX(box), frame, FALSE, FALSE, 0);
    state->build_file_label = gtk_label_new("Waiting for compiler output...");
    gtk_label_set_xalign(GTK_LABEL(state->build_file_label), 0.0f);
    gtk_container_add(GTK_CONTAINER(frame), state->build_file_label);

    scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_widget_set_size_request(scroll, 900, 360);
    gtk_box_pack_start(GTK_BOX(box), scroll, TRUE, TRUE, 0);
    log_view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(log_view), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(log_view), FALSE);
    g_object_set(log_view, "monospace", TRUE, NULL);
    gtk_container_add(GTK_CONTAINER(scroll), log_view);
    state->build_log_view = log_view;
    state->build_log_buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(log_view));

    g_signal_connect(state->build_dialog, "response", G_CALLBACK(on_build_dialog_response), state);
    g_signal_connect(state->build_dialog, "delete-event", G_CALLBACK(on_build_dialog_delete), state);
    gtk_widget_show_all(state->build_dialog);
}

static void start_kernel_build(AppState *state)
{
    GSubprocessLauncher *launcher;
    GError *error;
    const gchar *argv[] = {"make", "-j1", "V=1", NULL};
    const gchar *path_text;

    path_text = gtk_entry_get_text(GTK_ENTRY(state->path_entry));
    if (state->indexing) {
        set_status(state, "Wait for indexing or SQLite loading to finish before building.");
        return;
    }
    if (path_text == NULL || path_text[0] == '\0') {
        set_status(state, "Select a Linux source tree before building.");
        return;
    }
    if (state->build_process != NULL) {
        ensure_build_dialog(state);
        return;
    }

    ensure_build_dialog(state);
    gtk_text_buffer_set_text(state->build_log_buffer, "", -1);
    gtk_widget_set_sensitive(state->build_stop_button, TRUE);
    gtk_widget_set_sensitive(state->build_close_button, FALSE);
    gtk_label_set_text(GTK_LABEL(state->build_file_label), "Waiting for compiler output...");

    launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_MERGE);
    g_subprocess_launcher_set_cwd(launcher, path_text);
    error = NULL;
    state->build_process = g_subprocess_launcher_spawnv(launcher, argv, &error);
    g_object_unref(launcher);

    if (error != NULL) {
        gchar *message;

        message = g_strdup_printf("Unable to start kernel build: %s", error->message);
        set_status(state, message);
        append_build_log(state, error->message);
        finish_build_ui(state, message, FALSE);
        g_free(message);
        g_error_free(error);
        return;
    }

    app_state_ref(state);
    state->build_cancel = g_cancellable_new();
    state->build_stream = g_data_input_stream_new(g_subprocess_get_stdout_pipe(state->build_process));
    g_data_input_stream_read_line_async(state->build_stream,
                                        G_PRIORITY_DEFAULT,
                                        state->build_cancel,
                                        read_build_output_line,
                                        state);
    g_subprocess_wait_check_async(state->build_process,
                                  state->build_cancel,
                                  on_build_wait_complete,
                                  state);
    set_status(state, "Kernel build started.");
}

static void select_symbol_row(AppState *state, const gchar *symbol_id)
{
    const gchar *path_string;
    GtkTreePath *path;
    GtkTreeSelection *selection;
    OdysiaSymbol *symbol;

    if (state->index == NULL) {
        return;
    }
    path_string = g_hash_table_lookup(state->row_paths, symbol_id);
    if (path_string != NULL) {
        path = gtk_tree_path_new_from_string(path_string);
        selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(state->tree_view));
        gtk_tree_selection_select_path(selection, path);
        gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(state->tree_view), path, NULL, FALSE, 0.0f, 0.0f);
        gtk_tree_path_free(path);
        return;
    }
    symbol = odysia_index_get_symbol(state->index, symbol_id);
    if (symbol != NULL) {
        render_symbol(state, symbol);
    }
}

static void index_source_tree(AppState *state)
{
    const gchar *path_text;
    IndexJob *job;

    path_text = gtk_entry_get_text(GTK_ENTRY(state->path_entry));
    if (path_text == NULL || path_text[0] == '\0') {
        set_status(state, "Select a Linux source tree first.");
        return;
    }

    if (state->indexing) {
        set_status(state, "Indexing is already in progress.");
        return;
    }
    if (state->build_process != NULL) {
        set_status(state, "Stop the active kernel build before indexing.");
        return;
    }

    state->indexing = TRUE;
    clear_object_ptr((gpointer *) &state->index_cancel);
    state->index_cancel = g_cancellable_new();
    gtk_widget_set_sensitive(state->overall_progress_bar, TRUE);
    gtk_widget_set_sensitive(state->stage_progress_bar, TRUE);
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(state->overall_progress_bar), 0.0);
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(state->stage_progress_bar), 0.0);
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(state->overall_progress_bar), "Overall progress");
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(state->stage_progress_bar), "Preparing index...");
    gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(state->overall_progress_bar), TRUE);
    gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(state->stage_progress_bar), TRUE);
    {
        gchar *status_text;

        status_text = g_strdup_printf("Preparing index with %u source parser threads...",
                                      state->source_thread_count);
        set_status(state, status_text);
        g_free(status_text);
    }
    set_indexing_controls_sensitive(state, FALSE);

    job = g_new0(IndexJob, 1);
    job->state = app_state_ref(state);
    job->root_path = g_strdup(path_text);
    job->source_thread_count = state->source_thread_count;
    state->index_thread = g_thread_new("odysia-index", index_worker_thread, job);
}

static void on_window_destroy(GtkWidget *widget, gpointer user_data)
{
    AppState *state;

    (void) widget;
    state = user_data;
    g_atomic_int_set(&state->shutting_down, TRUE);
    if (state->build_process != NULL) {
        g_subprocess_force_exit(state->build_process);
    }
    if (state->build_cancel != NULL) {
        g_cancellable_cancel(state->build_cancel);
    }
    if (state->index_cancel != NULL) {
        g_cancellable_cancel(state->index_cancel);
    }
    if (state->tree_build_source_id != 0) {
        g_source_remove(state->tree_build_source_id);
        state->tree_build_source_id = 0;
    }
    if (state->index_thread != NULL) {
        g_thread_join(state->index_thread);
        state->index_thread = NULL;
    }
}

static void on_search_changed(GtkEditable *editable, gpointer user_data)
{
    AppState *state;

    (void) editable;
    state = user_data;
    if (state->index != NULL && !state->indexing) {
        rebuild_tree(state);
    }
}

static void on_sort_changed(GtkComboBox *combo, gpointer user_data)
{
    AppState *state;

    (void) combo;
    state = user_data;
    if (state->index != NULL && !state->indexing) {
        rebuild_tree(state);
        set_status(state, "Symbol tree sorting updated.");
    }
}

static void on_stop_clicked(GtkButton *button, gpointer user_data)
{
    AppState *state;

    (void) button;
    state = user_data;
    stop_active_work(state);
}

static void on_clear_clicked(GtkButton *button, gpointer user_data)
{
    AppState *state;

    (void) button;
    state = user_data;
    stop_active_work(state);
    clear_loaded_index_data(state);
    gtk_entry_set_text(GTK_ENTRY(state->search_entry), "");
    reset_progress_bars(state, "Idle");
    set_indexing_controls_sensitive(state, TRUE);
    set_status(state, "Cleared all loaded data.");
}

static void on_index_clicked(GtkButton *button, gpointer user_data)
{
    AppState *state;

    (void) button;
    state = user_data;
    index_source_tree(state);
}

static void on_build_clicked(GtkButton *button, gpointer user_data)
{
    AppState *state;

    (void) button;
    state = user_data;
    start_kernel_build(state);
}

static void on_browse_clicked(GtkWidget *button, gpointer user_data)
{
    AppState *state;
    GtkFileChooserNative *chooser;
    gint result;

    (void) button;
    state = user_data;
    chooser = gtk_file_chooser_native_new("Select Linux source tree",
                                          GTK_WINDOW(state->window),
                                          GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
                                          "Open",
                                          "Cancel");
    result = gtk_native_dialog_run(GTK_NATIVE_DIALOG(chooser));
    if (result == GTK_RESPONSE_ACCEPT) {
        gchar *filename;

        filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(chooser));
        gtk_entry_set_text(GTK_ENTRY(state->path_entry), filename);
        g_free(filename);
    }
    g_object_unref(chooser);
}

static void on_path_entry_activate(GtkEntry *entry, gpointer user_data)
{
    AppState *state;

    (void) entry;
    state = user_data;
    index_source_tree(state);
}

static void on_quit_clicked(GtkWidget *item, gpointer user_data)
{
    AppState *state;

    (void) item;
    state = user_data;
    gtk_window_close(GTK_WINDOW(state->window));
}

static void on_about_clicked(GtkWidget *item, gpointer user_data)
{
    AppState *state;
    const gchar *authors[] = {
        "GitHub Copilot",
        NULL
    };

    (void) item;
    state = user_data;
    gtk_show_about_dialog(GTK_WINDOW(state->window),
                          "program-name", "Odysia",
                          "version", PACKAGE_VERSION,
                          "comments", "Beta 5 - Linux source and documentation explorer for kernel development.",
                          "website", "https://kernel.org",
                          "authors", authors,
                          NULL);
}

static void on_thread_preset_clicked(GtkButton *button, gpointer user_data)
{
    GtkSpinButton *spin;
    guint cpu_count;
    guint multiplier;

    (void) user_data;
    spin = GTK_SPIN_BUTTON(g_object_get_data(G_OBJECT(button), "thread-spin"));
    cpu_count = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(button), "cpu-count"));
    multiplier = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(button), "multiplier"));
    gtk_spin_button_set_value(spin, (gdouble) cpu_count * (gdouble) multiplier);
}

static GtkWidget *create_thread_preset_button(const gchar *label,
                                              GtkWidget *spin,
                                              guint cpu_count,
                                              guint multiplier)
{
    GtkWidget *button;

    button = gtk_button_new_with_label(label);
    g_object_set_data(G_OBJECT(button), "thread-spin", spin);
    g_object_set_data(G_OBJECT(button), "cpu-count", GUINT_TO_POINTER(cpu_count));
    g_object_set_data(G_OBJECT(button), "multiplier", GUINT_TO_POINTER(multiplier));
    g_signal_connect(button, "clicked", G_CALLBACK(on_thread_preset_clicked), NULL);
    return button;
}

static void on_settings_clicked(GtkWidget *item, gpointer user_data)
{
    AppState *state;
    GtkWidget *dialog;
    GtkWidget *content;
    GtkWidget *box;
    GtkWidget *description;
    GtkWidget *controls;
    GtkWidget *spin;
    GtkWidget *cpu_button;
    GtkWidget *double_button;
    GtkWidget *triple_button;
    GtkWidget *font_grid;
    GtkWidget *detail_font_button;
    GtkWidget *source_font_button;
    guint cpu_count;
    gint response;

    (void) item;
    state = user_data;
    cpu_count = MAX(1, g_get_num_processors());
    dialog = gtk_dialog_new_with_buttons("Settings",
                                         GTK_WINDOW(state->window),
                                         GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                         "Reset Defaults", GTK_RESPONSE_REJECT,
                                         "Cancel", GTK_RESPONSE_CANCEL,
                                         "Apply", GTK_RESPONSE_APPLY,
                                         NULL);
    content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(box), 12);
    gtk_container_add(GTK_CONTAINER(content), box);

    description = gtk_label_new(NULL);
    {
        gchar *description_text;

        description_text = g_strdup_printf("Source parser threads (detected logical CPUs: %u)", cpu_count);
        gtk_label_set_text(GTK_LABEL(description), description_text);
        g_free(description_text);
    }
    gtk_label_set_xalign(GTK_LABEL(description), 0.0f);
    gtk_box_pack_start(GTK_BOX(box), description, FALSE, FALSE, 0);

    controls = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(box), controls, FALSE, FALSE, 0);
    spin = gtk_spin_button_new_with_range(1.0, MAX(1024.0, (gdouble) cpu_count * 3.0), 1.0);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin), state->source_thread_count);
    gtk_box_pack_start(GTK_BOX(controls), spin, FALSE, FALSE, 0);

    cpu_button = create_thread_preset_button("CPU", spin, cpu_count, 1);
    double_button = create_thread_preset_button("2x", spin, cpu_count, 2);
    triple_button = create_thread_preset_button("3x", spin, cpu_count, 3);
    gtk_widget_set_tooltip_text(cpu_button, "Use one parser thread per detected logical CPU");
    gtk_widget_set_tooltip_text(double_button, "Use twice the detected logical CPU count");
    gtk_widget_set_tooltip_text(triple_button, "Use three times the detected logical CPU count");
    gtk_box_pack_start(GTK_BOX(controls), cpu_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(controls), double_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(controls), triple_button, FALSE, FALSE, 0);

    font_grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(font_grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(font_grid), 12);
    gtk_box_pack_start(GTK_BOX(box), font_grid, FALSE, FALSE, 0);
    description = gtk_label_new("Detail pane font");
    gtk_label_set_xalign(GTK_LABEL(description), 0.0f);
    gtk_grid_attach(GTK_GRID(font_grid), description, 0, 0, 1, 1);
    detail_font_button = gtk_font_button_new_with_font(state->detail_font);
    gtk_font_button_set_show_style(GTK_FONT_BUTTON(detail_font_button), TRUE);
    gtk_font_button_set_show_size(GTK_FONT_BUTTON(detail_font_button), TRUE);
    gtk_widget_set_hexpand(detail_font_button, TRUE);
    gtk_grid_attach(GTK_GRID(font_grid), detail_font_button, 1, 0, 1, 1);
    description = gtk_label_new("Source pane font");
    gtk_label_set_xalign(GTK_LABEL(description), 0.0f);
    gtk_grid_attach(GTK_GRID(font_grid), description, 0, 1, 1, 1);
    source_font_button = gtk_font_button_new_with_font(state->source_font);
    gtk_font_button_set_show_style(GTK_FONT_BUTTON(source_font_button), TRUE);
    gtk_font_button_set_show_size(GTK_FONT_BUTTON(source_font_button), TRUE);
    gtk_widget_set_hexpand(source_font_button, TRUE);
    gtk_grid_attach(GTK_GRID(font_grid), source_font_button, 1, 1, 1, 1);

    gtk_widget_show_all(dialog);
    do {
        response = gtk_dialog_run(GTK_DIALOG(dialog));
        if (response == GTK_RESPONSE_REJECT) {
            gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin), (gdouble) cpu_count);
            gtk_font_chooser_set_font(GTK_FONT_CHOOSER(detail_font_button), DEFAULT_DETAIL_FONT);
            gtk_font_chooser_set_font(GTK_FONT_CHOOSER(source_font_button), DEFAULT_SOURCE_FONT);
        }
    } while (response == GTK_RESPONSE_REJECT);
    if (response == GTK_RESPONSE_APPLY) {
        GError *error;
        gchar *status_text;

        state->source_thread_count = (guint) gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(spin));
        g_free(state->detail_font);
        g_free(state->source_font);
        state->detail_font = gtk_font_chooser_get_font(GTK_FONT_CHOOSER(detail_font_button));
        state->source_font = gtk_font_chooser_get_font(GTK_FONT_CHOOSER(source_font_button));
        apply_text_view_fonts(state);
        error = NULL;
        if (save_settings(state, &error)) {
            status_text = g_strdup_printf("Settings saved with %u parser threads.",
                                          state->source_thread_count);
        } else {
            status_text = g_strdup_printf("Settings applied but could not be saved: %s",
                                          error != NULL ? error->message : "unknown error");
            g_clear_error(&error);
        }
        set_status(state, status_text);
        g_free(status_text);
    }
    gtk_widget_destroy(dialog);
}

static void on_save_index_clicked(GtkWidget *item, gpointer user_data)
{
    AppState *state;
    GtkFileChooserNative *chooser;
    gint result;

    (void) item;
    state = user_data;
    if (state->index == NULL) {
        set_status(state, "No parsed data is loaded to save.");
        return;
    }

    chooser = gtk_file_chooser_native_new("Save parsed index",
                                          GTK_WINDOW(state->window),
                                          GTK_FILE_CHOOSER_ACTION_SAVE,
                                          "Save",
                                          "Cancel");
    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(chooser), "odysia-index.sqlite");
    result = gtk_native_dialog_run(GTK_NATIVE_DIALOG(chooser));
    if (result == GTK_RESPONSE_ACCEPT) {
        gchar *filename;
        GError *error;

        filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(chooser));
        error = NULL;
        if (!odysia_index_save_sqlite(state->index, filename, &error)) {
            set_status(state, error != NULL ? error->message : "Failed to save SQLite index.");
            if (error != NULL) {
                g_error_free(error);
            }
        } else {
            set_status(state, "Saved parsed index to SQLite.");
        }
        g_free(filename);
    }
    g_object_unref(chooser);
}

static void on_open_index_clicked(GtkWidget *item, gpointer user_data)
{
    AppState *state;
    GtkFileChooserNative *chooser;
    gint result;

    (void) item;
    state = user_data;
    if (state->indexing) {
        set_status(state, "Wait for the current operation to finish before loading an index.");
        return;
    }
    if (state->build_process != NULL) {
        set_status(state, "Stop the active kernel build before loading an index.");
        return;
    }
    chooser = gtk_file_chooser_native_new("Open parsed index",
                                          GTK_WINDOW(state->window),
                                          GTK_FILE_CHOOSER_ACTION_OPEN,
                                          "Open",
                                          "Cancel");
    result = gtk_native_dialog_run(GTK_NATIVE_DIALOG(chooser));
    if (result == GTK_RESPONSE_ACCEPT) {
        gchar *filename;
        LoadJob *job;

        filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(chooser));
        state->indexing = TRUE;
        clear_object_ptr((gpointer *) &state->index_cancel);
        set_indexing_controls_sensitive(state, FALSE);
        gtk_widget_set_sensitive(state->overall_progress_bar, TRUE);
        gtk_widget_set_sensitive(state->stage_progress_bar, TRUE);
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(state->overall_progress_bar), 0.0);
        gtk_progress_bar_pulse(GTK_PROGRESS_BAR(state->stage_progress_bar));
        gtk_progress_bar_set_text(GTK_PROGRESS_BAR(state->overall_progress_bar), "Loading SQLite index");
        gtk_progress_bar_set_text(GTK_PROGRESS_BAR(state->stage_progress_bar), "Reading database...");
        set_status(state, "Loading SQLite index...");
        job = g_new0(LoadJob, 1);
        job->state = app_state_ref(state);
        job->filename = g_strdup(filename);
        state->index_thread = g_thread_new("odysia-load", load_worker_thread, job);
        g_free(filename);
    }
    g_object_unref(chooser);
}

static GtkWidget *create_menu_item_with_icon(const gchar *label, const gchar *icon_name)
{
    GtkWidget *item;
    GtkWidget *box;
    GtkWidget *image;
    GtkWidget *text;

    item = gtk_menu_item_new();
    box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    image = gtk_image_new_from_icon_name(icon_name, GTK_ICON_SIZE_MENU);
    text = gtk_label_new(label);
    gtk_label_set_xalign(GTK_LABEL(text), 0.0f);
    gtk_box_pack_start(GTK_BOX(box), image, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), text, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(item), box);
    return item;
}

static GtkWidget *create_text_view(gboolean monospace)
{
    GtkWidget *view;

    view = gtk_text_view_new();
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_editable(GTK_TEXT_VIEW(view), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(view), FALSE);
    if (monospace) {
        g_object_set(view, "monospace", TRUE, NULL);
    }
    return view;
}

static void activate(GtkApplication *app, gpointer user_data)
{
    AppState *state;
    GtkWidget *window;
    GtkWidget *root;
    GtkWidget *menu_bar;
    GtkWidget *file_menu_item;
    GtkWidget *actions_menu_item;
    GtkWidget *help_menu_item;
    GtkWidget *file_menu;
    GtkWidget *actions_menu;
    GtkWidget *help_menu;
    GtkWidget *browse_item;
    GtkWidget *open_item;
    GtkWidget *save_item;
    GtkWidget *clear_item;
    GtkWidget *quit_item;
    GtkWidget *index_item;
    GtkWidget *build_item;
    GtkWidget *stop_item;
    GtkWidget *settings_item;
    GtkWidget *about_item;
    GtkWidget *input_row;
    GtkWidget *progress_box;
    GtkWidget *pane;
    GtkWidget *right_pane;
    GtkWidget *tree_scroll;
    GtkWidget *detail_scroll;
    GtkWidget *source_scroll;
    GtkCellRenderer *renderer;
    GtkTreeViewColumn *column;
    GtkTreeSelection *selection;
    GtkTextBuffer *detail_buffer;

    (void) user_data;
    state = g_new0(AppState, 1);
    state->app = app;
    state->ref_count = 1;
    load_settings(state);
    state->detail_links = g_ptr_array_new_with_free_func(link_range_free);
    state->row_paths = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);

    window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window), "Odysia Linux Source Explorer");
    gtk_window_set_default_size(GTK_WINDOW(window), 1400, 900);
    set_application_icon(GTK_WINDOW(window));
    state->window = window;
    g_object_set_data_full(G_OBJECT(window), "odysia-state", state, (GDestroyNotify) app_state_unref);

    root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_add(GTK_CONTAINER(window), root);

    menu_bar = gtk_menu_bar_new();
    state->menu_bar = menu_bar;
    gtk_box_pack_start(GTK_BOX(root), menu_bar, FALSE, FALSE, 0);

    file_menu_item = create_menu_item_with_icon("File", "document-open-symbolic");
    actions_menu_item = create_menu_item_with_icon("Actions", "applications-system-symbolic");
    help_menu_item = create_menu_item_with_icon("Help", "help-browser-symbolic");
    state->file_menu_item = file_menu_item;
    state->actions_menu_item = actions_menu_item;
    state->help_menu_item = help_menu_item;
    gtk_menu_shell_append(GTK_MENU_SHELL(menu_bar), file_menu_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu_bar), actions_menu_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu_bar), help_menu_item);

    file_menu = gtk_menu_new();
    actions_menu = gtk_menu_new();
    help_menu = gtk_menu_new();
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(file_menu_item), file_menu);
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(actions_menu_item), actions_menu);
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(help_menu_item), help_menu);

    browse_item = create_menu_item_with_icon("Select Source Tree", "folder-open-symbolic");
    open_item = create_menu_item_with_icon("Open SQLite Index", "document-open-recent-symbolic");
    save_item = create_menu_item_with_icon("Save SQLite Index", "document-save-symbolic");
    clear_item = create_menu_item_with_icon("Clear Data", "edit-clear-all-symbolic");
    quit_item = create_menu_item_with_icon("Quit", "application-exit-symbolic");
    gtk_menu_shell_append(GTK_MENU_SHELL(file_menu), browse_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(file_menu), open_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(file_menu), save_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(file_menu), clear_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(file_menu), quit_item);

    index_item = create_menu_item_with_icon("Index", "system-search-symbolic");
    build_item = create_menu_item_with_icon("Build Kernel", "applications-engineering-symbolic");
    stop_item = create_menu_item_with_icon("Stop", "process-stop-symbolic");
    settings_item = create_menu_item_with_icon("Settings", "preferences-system-symbolic");
    state->index_item = index_item;
    state->build_item = build_item;
    state->stop_item = stop_item;
    state->settings_item = settings_item;
    gtk_menu_shell_append(GTK_MENU_SHELL(actions_menu), index_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(actions_menu), build_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(actions_menu), stop_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(actions_menu), settings_item);

    about_item = create_menu_item_with_icon("About", "help-about-symbolic");
    gtk_menu_shell_append(GTK_MENU_SHELL(help_menu), about_item);

    input_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    state->input_row = input_row;
    gtk_box_pack_start(GTK_BOX(root), input_row, FALSE, FALSE, 8);

    state->path_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(state->path_entry), "Path to a Linux source tree");
    gtk_box_pack_start(GTK_BOX(input_row), state->path_entry, TRUE, TRUE, 0);

    state->search_entry = gtk_search_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(state->search_entry), "Filter symbols");
    gtk_widget_set_size_request(state->search_entry, 260, -1);
    gtk_box_pack_start(GTK_BOX(input_row), state->search_entry, FALSE, FALSE, 0);

    state->sort_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(state->sort_combo), "Sort: Name");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(state->sort_combo), "Sort: Source Line");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(state->sort_combo), "Sort: Symbol Kind");
    gtk_combo_box_set_active(GTK_COMBO_BOX(state->sort_combo), TREE_SORT_NAME);
    gtk_widget_set_tooltip_text(state->sort_combo, "Sort symbols within each source file");
    gtk_box_pack_start(GTK_BOX(input_row), state->sort_combo, FALSE, FALSE, 0);

    state->status_label = gtk_label_new("Select a Linux source tree and click Index.");
    gtk_label_set_xalign(GTK_LABEL(state->status_label), 0.0f);
    gtk_box_pack_start(GTK_BOX(root), state->status_label, FALSE, FALSE, 0);

    progress_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_box_pack_start(GTK_BOX(root), progress_box, FALSE, FALSE, 0);

    state->overall_progress_bar = gtk_progress_bar_new();
    gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(state->overall_progress_bar), TRUE);
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(state->overall_progress_bar), "Overall progress");
    gtk_widget_set_sensitive(state->overall_progress_bar, FALSE);
    gtk_box_pack_start(GTK_BOX(progress_box), state->overall_progress_bar, FALSE, FALSE, 0);

    state->stage_progress_bar = gtk_progress_bar_new();
    gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(state->stage_progress_bar), TRUE);
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(state->stage_progress_bar), "Idle");
    gtk_widget_set_sensitive(state->stage_progress_bar, FALSE);
    gtk_box_pack_start(GTK_BOX(progress_box), state->stage_progress_bar, FALSE, FALSE, 0);

    pane = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    state->content_pane = pane;
    gtk_box_pack_start(GTK_BOX(root), pane, TRUE, TRUE, 0);

    state->tree_store = gtk_tree_store_new(N_COLUMNS,
                                           G_TYPE_STRING,
                                           G_TYPE_STRING,
                                           G_TYPE_STRING,
                                           G_TYPE_STRING,
                                           G_TYPE_STRING,
                                           G_TYPE_STRING,
                                           G_TYPE_INT);
    state->tree_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(state->tree_store));
    {
        GtkCellRenderer *icon_renderer;

        icon_renderer = gtk_cell_renderer_pixbuf_new();
        column = gtk_tree_view_column_new_with_attributes("", icon_renderer, "icon-name", COLUMN_ICON_NAME, NULL);
        gtk_tree_view_append_column(GTK_TREE_VIEW(state->tree_view), column);
    }
    {
        GtkCellRenderer *documentation_renderer;

        documentation_renderer = gtk_cell_renderer_pixbuf_new();
        column = gtk_tree_view_column_new_with_attributes("Docs",
                                                          documentation_renderer,
                                                          "icon-name",
                                                          COLUMN_DOCUMENTATION_ICON,
                                                          NULL);
        gtk_tree_view_column_set_sizing(column, GTK_TREE_VIEW_COLUMN_FIXED);
        gtk_tree_view_column_set_fixed_width(column, 48);
        gtk_tree_view_column_set_resizable(column, FALSE);
        gtk_tree_view_append_column(GTK_TREE_VIEW(state->tree_view), column);
    }
    renderer = gtk_cell_renderer_text_new();
    column = gtk_tree_view_column_new_with_attributes("Symbol", renderer, "text", COLUMN_TITLE, "weight", COLUMN_WEIGHT, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(state->tree_view), column);
    renderer = gtk_cell_renderer_text_new();
    column = gtk_tree_view_column_new_with_attributes("Type", renderer, "text", COLUMN_KIND, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(state->tree_view), column);
    renderer = gtk_cell_renderer_text_new();
    column = gtk_tree_view_column_new_with_attributes("Directory / Source", renderer, "text", COLUMN_LOCATION, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(state->tree_view), column);
    tree_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(tree_scroll), state->tree_view);
    gtk_widget_set_size_request(tree_scroll, 340, -1);
    gtk_paned_pack1(GTK_PANED(pane), tree_scroll, FALSE, FALSE);

    right_pane = gtk_paned_new(GTK_ORIENTATION_VERTICAL);
    gtk_paned_pack2(GTK_PANED(pane), right_pane, TRUE, FALSE);

    state->detail_view = create_text_view(FALSE);
    detail_buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(state->detail_view));
    state->link_tag = gtk_text_buffer_create_tag(detail_buffer,
                                                 "link",
                                                 "foreground", "#1f5fbf",
                                                 "underline", PANGO_UNDERLINE_SINGLE,
                                                 NULL);
    state->header_tag = gtk_text_buffer_create_tag(detail_buffer,
                                                   "header",
                                                   "foreground", "#0f3d91",
                                                   "weight", PANGO_WEIGHT_BOLD,
                                                   "scale", 1.12,
                                                   NULL);
    state->section_tag = gtk_text_buffer_create_tag(detail_buffer,
                                                    "section",
                                                    "foreground", "#6b4f1d",
                                                    "weight", PANGO_WEIGHT_BOLD,
                                                    NULL);
    state->accent_tag = gtk_text_buffer_create_tag(detail_buffer,
                                                   "accent",
                                                   "foreground", "#1b6b57",
                                                   NULL);
    detail_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(detail_scroll), state->detail_view);
    gtk_paned_pack1(GTK_PANED(right_pane), detail_scroll, TRUE, FALSE);

    state->source_view = create_text_view(TRUE);
    {
        GtkTextBuffer *source_buffer;

        source_buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(state->source_view));
        state->source_keyword_tag = gtk_text_buffer_create_tag(source_buffer,
                                                               "source-keyword",
                                                               "foreground", "#0b4f9c",
                                                               "weight", PANGO_WEIGHT_BOLD,
                                                               NULL);
        state->source_kernel_tag = gtk_text_buffer_create_tag(source_buffer,
                                                              "source-kernel-keyword",
                                                              "foreground", "#9c3d00",
                                                              "weight", PANGO_WEIGHT_BOLD,
                                                              NULL);
        state->source_string_tag = gtk_text_buffer_create_tag(source_buffer,
                                                              "source-string",
                                                              "foreground", "#1f6f43",
                                                              NULL);
        state->source_comment_tag = gtk_text_buffer_create_tag(source_buffer,
                                                               "source-comment",
                                                               "foreground", "#6a737d",
                                                               "style", PANGO_STYLE_ITALIC,
                                                               NULL);
        state->source_number_tag = gtk_text_buffer_create_tag(source_buffer,
                                                              "source-number",
                                                              "foreground", "#7f3fbf",
                                                              NULL);
        state->source_preproc_tag = gtk_text_buffer_create_tag(source_buffer,
                                                               "source-preproc",
                                                               "foreground", "#8a2f8f",
                                                               "weight", PANGO_WEIGHT_BOLD,
                                                               NULL);
    }
    source_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(source_scroll), state->source_view);
    gtk_paned_pack2(GTK_PANED(right_pane), source_scroll, TRUE, FALSE);
    apply_text_view_fonts(state);

    state->status_bar = gtk_statusbar_new();
    state->status_context_id = gtk_statusbar_get_context_id(GTK_STATUSBAR(state->status_bar), "odysia");
    gtk_box_pack_end(GTK_BOX(root), state->status_bar, FALSE, FALSE, 0);

    selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(state->tree_view));
    g_signal_connect(selection, "changed", G_CALLBACK(on_tree_selection_changed), state);
    g_signal_connect(index_item, "activate", G_CALLBACK(on_index_clicked), state);
    g_signal_connect(build_item, "activate", G_CALLBACK(on_build_clicked), state);
    g_signal_connect(stop_item, "activate", G_CALLBACK(on_stop_clicked), state);
    g_signal_connect(settings_item, "activate", G_CALLBACK(on_settings_clicked), state);
    g_signal_connect(clear_item, "activate", G_CALLBACK(on_clear_clicked), state);
    g_signal_connect(browse_item, "activate", G_CALLBACK(on_browse_clicked), state);
    g_signal_connect(open_item, "activate", G_CALLBACK(on_open_index_clicked), state);
    g_signal_connect(save_item, "activate", G_CALLBACK(on_save_index_clicked), state);
    g_signal_connect(quit_item, "activate", G_CALLBACK(on_quit_clicked), state);
    g_signal_connect(about_item, "activate", G_CALLBACK(on_about_clicked), state);
    g_signal_connect(state->search_entry, "changed", G_CALLBACK(on_search_changed), state);
    g_signal_connect(state->sort_combo, "changed", G_CALLBACK(on_sort_changed), state);
    g_signal_connect(state->path_entry, "activate", G_CALLBACK(on_path_entry_activate), state);
    g_signal_connect(state->detail_view, "button-release-event", G_CALLBACK(detail_view_button_release), state);
    g_signal_connect(window, "destroy", G_CALLBACK(on_window_destroy), state);

    show_welcome_text(state);
    set_status(state, "Select a Linux source tree and click Index.");
    gtk_widget_show_all(window);
}

GtkApplication *odysia_create_application(void)
{
    GtkApplication *app;

    app = gtk_application_new("org.odysia.Odysia", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    return app;
}