#include "markdown.h"
#include "code_highlight.h"
#include "config.h"
#include "md4c/md4c.h"
#include <stdarg.h>
#include <string.h>

/* Tag names */
#define TAG_H1 "h1"
#define TAG_H2 "h2"
#define TAG_H3 "h3"
#define TAG_BOLD "bold"
#define TAG_ITALIC "italic"
#define TAG_STRIKE "strike"
#define TAG_UNDERLINE "underline"
#define TAG_SUP "sup"
#define TAG_SUB "sub"
#define TAG_SMALL "small"
#define TAG_KBD "kbd"
#define TAG_CODE "code"
#define TAG_CODE_BLOCK "code_block"
#define TAG_QUOTE "quote"
#define TAG_LIST "list"
#define TAG_LIST_BULLET "list_bullet"
#define TAG_LINK "link"
#define TAG_HRULE "hrule"
#define TAG_TABLE "table"
#define TAG_TABLE_HEADER "table_header"
#define TAG_TABLE_SEPARATOR "table_separator"
#define TAG_INVISIBLE "invisible"
#define TABLE_MODEL_DATA_KEY "seemd-table-model"

typedef struct {
  gboolean ordered;
  guint next_index;
} ListState;

typedef struct {
  gint left_margin;
  gint indent;
} ListLayoutSpec;

typedef struct {
  MD_BLOCKTYPE type;
  guint pushed_tags;
} BlockState;

typedef struct {
  MD_SPANTYPE type;
  guint pushed_tags;
} SpanState;

typedef enum {
  HTML_EL_GENERIC,
  HTML_EL_BLOCK,
  HTML_EL_HEADING,
  HTML_EL_QUOTE,
  HTML_EL_UL,
  HTML_EL_OL,
  HTML_EL_LI,
  HTML_EL_LINK,
  HTML_EL_PRE,
  HTML_EL_CODE,
  HTML_EL_INLINE,
  HTML_EL_TABLE,
  HTML_EL_THEAD,
  HTML_EL_TBODY,
  HTML_EL_TR,
  HTML_EL_TH,
  HTML_EL_TD,
  HTML_EL_SUPPRESSED
} HtmlElementKind;

typedef struct {
  gchar *name;
  HtmlElementKind kind;
  guint pushed_tags;
  gboolean heading_started;
  gboolean list_started;
  gboolean quote_started;
  gboolean pre_started;
  gboolean table_cell_started;
} HtmlElement;

typedef struct {
  gboolean is_header;
  GPtrArray *cells; /* gchar* */
} SeemdTableRow;

typedef struct {
  guint col_count;
  GArray *aligns;   /* MD_ALIGN */
  GPtrArray *rows;  /* SeemdTableRow* */
} SeemdTable;

typedef struct {
  gint start_offset;
  gint end_offset;
  const MarkydLanguageHighlight *language;
} CodeBlockRange;

typedef struct {
  GtkTextBuffer *buffer;
  GtkTextIter iter;
  GPtrArray *active_tags; /* GtkTextTag* */
  GArray *block_stack;    /* BlockState */
  GArray *span_stack;     /* SpanState */
  GPtrArray *html_stack;  /* HtmlElement* */
  GString *html_pending;
  GArray *list_stack;     /* ListState */
  GHashTable *anchor_counts;
  GString *heading_text;
  gint heading_start_offset;
  gboolean in_heading;
  gboolean list_item_prefix_pending;
  guint quote_depth;
  gboolean in_table_head;
  SeemdTable *table_model;
  SeemdTableRow *table_current_row;
  GString *table_cell_text;
  guint table_current_col;
  gboolean in_image;
  gchar *image_src;
  GString *image_alt;
  gint image_width;
  gint image_height;
  GArray *code_blocks; /* CodeBlockRange */
  gint current_code_start_offset;
  const MarkydLanguageHighlight *current_code_language;
  gboolean has_output;
  guint trailing_newlines;
  guint html_suppressed_depth;
  guint html_pre_depth;
} RenderCtx;

static gchar *decode_html_entities(const gchar *text, gsize len);

static void html_element_free(gpointer data) {
  HtmlElement *el = (HtmlElement *)data;
  if (!el) {
    return;
  }
  g_free(el->name);
  g_free(el);
}

static void seemd_table_row_free(gpointer data) {
  SeemdTableRow *row = (SeemdTableRow *)data;
  if (!row) {
    return;
  }
  if (row->cells) {
    g_ptr_array_free(row->cells, TRUE);
  }
  g_free(row);
}

static void seemd_table_free(gpointer data) {
  SeemdTable *table = (SeemdTable *)data;
  if (!table) {
    return;
  }
  if (table->aligns) {
    g_array_free(table->aligns, TRUE);
  }
  if (table->rows) {
    g_ptr_array_free(table->rows, TRUE);
  }
  g_free(table);
}

static SeemdTable *seemd_table_new(guint col_count) {
  SeemdTable *table = g_new0(SeemdTable, 1);
  table->col_count = col_count;
  table->aligns = g_array_sized_new(FALSE, FALSE, sizeof(MD_ALIGN), col_count);
  table->rows = g_ptr_array_new_with_free_func(seemd_table_row_free);
  for (guint i = 0; i < col_count; i++) {
    MD_ALIGN align = MD_ALIGN_DEFAULT;
    g_array_append_val(table->aligns, align);
  }
  return table;
}

static void table_ensure_col_count(SeemdTable *table, guint col_count) {
  if (!table || col_count <= table->col_count) {
    return;
  }

  while (table->aligns->len < col_count) {
    MD_ALIGN align = MD_ALIGN_DEFAULT;
    g_array_append_val(table->aligns, align);
  }
  table->col_count = col_count;
}

static GtkTextTag *lookup_tag(GtkTextBuffer *buffer, const gchar *name) {
  GtkTextTagTable *table;
  if (!buffer || !name) {
    return NULL;
  }
  table = gtk_text_buffer_get_tag_table(buffer);
  return table ? gtk_text_tag_table_lookup(table, name) : NULL;
}

static gboolean line_is_blank(const gchar *line, gsize len) {
  for (gsize i = 0; i < len; i++) {
    gchar ch = line[i];
    if (ch == '\r') {
      continue;
    }
    if (ch != ' ' && ch != '\t') {
      return FALSE;
    }
  }
  return TRUE;
}

static gboolean line_is_dash_rule(const gchar *line, gsize len) {
  gsize i = 0;
  guint dashes = 0;

  while (i < len && (line[i] == ' ' || line[i] == '\t')) {
    i++;
  }

  for (; i < len; i++) {
    gchar ch = line[i];
    if (ch == '\r') {
      continue;
    }
    if (ch == '-') {
      dashes++;
      continue;
    }
    if (ch == ' ' || ch == '\t') {
      continue;
    }
    return FALSE;
  }

  return dashes >= 3;
}

static gboolean line_is_fence(const gchar *line, gsize len, gchar *fence_ch,
                              guint *fence_len) {
  gsize i = 0;
  gchar ch;
  guint run = 0;

  while (i < len && (line[i] == ' ' || line[i] == '\t')) {
    i++;
  }
  if (i >= len) {
    return FALSE;
  }

  ch = line[i];
  if (ch != '`' && ch != '~') {
    return FALSE;
  }

  while (i < len && line[i] == ch) {
    run++;
    i++;
  }

  if (run < 3) {
    return FALSE;
  }

  if (fence_ch) {
    *fence_ch = ch;
  }
  if (fence_len) {
    *fence_len = run;
  }
  return TRUE;
}

static gchar *normalize_markdown_source(const gchar *source) {
  const gchar *p;
  GString *out;
  gboolean prev_nonblank = FALSE;
  gboolean in_fence = FALSE;
  gchar active_fence_ch = 0;
  guint active_fence_len = 0;

  if (!source || source[0] == '\0') {
    return g_strdup("");
  }

  out = g_string_new(NULL);
  p = source;

  while (*p != '\0') {
    const gchar *line_start = p;
    const gchar *line_end = strchr(p, '\n');
    gsize line_len = line_end ? (gsize)(line_end - line_start)
                              : strlen(line_start);
    gboolean has_nl = (line_end != NULL);
    gboolean is_blank = line_is_blank(line_start, line_len);
    gboolean is_rule = FALSE;
    gchar fence_ch = 0;
    guint fence_len = 0;

    if (line_is_fence(line_start, line_len, &fence_ch, &fence_len)) {
      if (!in_fence) {
        in_fence = TRUE;
        active_fence_ch = fence_ch;
        active_fence_len = fence_len;
      } else if (fence_ch == active_fence_ch && fence_len >= active_fence_len) {
        in_fence = FALSE;
      }
    }

    if (!in_fence) {
      is_rule = line_is_dash_rule(line_start, line_len);
    }

    if (is_rule && prev_nonblank) {
      g_string_append_c(out, '\n');
    }

    g_string_append_len(out, line_start, line_len);
    if (has_nl) {
      g_string_append_c(out, '\n');
      p = line_end + 1;
    } else {
      p = line_start + line_len;
    }

    prev_nonblank = !is_blank;
  }

  return g_string_free(out, FALSE);
}

static void update_newline_state(RenderCtx *ctx, const gchar *text, gsize len) {
  if (!ctx || !text || len == 0) {
    return;
  }
  ctx->has_output = TRUE;
  for (gsize i = 0; i < len; i++) {
    if (text[i] == '\n') {
      ctx->trailing_newlines++;
    } else {
      ctx->trailing_newlines = 0;
    }
  }
}

static void note_non_newline_output(RenderCtx *ctx) {
  if (!ctx) {
    return;
  }
  ctx->has_output = TRUE;
  ctx->trailing_newlines = 0;
}

static void apply_tag_by_offsets(GtkTextBuffer *buffer, GtkTextTag *tag,
                                 gint start_offset, gint end_offset) {
  GtkTextIter start;
  GtkTextIter end;

  if (!buffer || !tag || end_offset <= start_offset) {
    return;
  }

  gtk_text_buffer_get_iter_at_offset(buffer, &start, start_offset);
  gtk_text_buffer_get_iter_at_offset(buffer, &end, end_offset);
  gtk_text_buffer_apply_tag(buffer, tag, &start, &end);
}

static void apply_tag_by_name_offsets(GtkTextBuffer *buffer, const gchar *name,
                                      gint start_offset, gint end_offset) {
  GtkTextTag *tag = lookup_tag(buffer, name);
  apply_tag_by_offsets(buffer, tag, start_offset, end_offset);
}

static void apply_active_tags(RenderCtx *ctx, gint start_offset,
                              gint end_offset) {
  if (!ctx || end_offset <= start_offset) {
    return;
  }
  for (guint i = 0; i < ctx->active_tags->len; i++) {
    GtkTextTag *tag = g_ptr_array_index(ctx->active_tags, i);
    apply_tag_by_offsets(ctx->buffer, tag, start_offset, end_offset);
  }
}

static void insert_text(RenderCtx *ctx, const gchar *text, gsize len) {
  gint start_offset;
  gint end_offset;

  if (!ctx || !text || len == 0) {
    return;
  }

  start_offset = gtk_text_iter_get_offset(&ctx->iter);
  gtk_text_buffer_insert(ctx->buffer, &ctx->iter, text, (gint)len);
  end_offset = gtk_text_iter_get_offset(&ctx->iter);

  apply_active_tags(ctx, start_offset, end_offset);
  update_newline_state(ctx, text, len);
}

static void insert_cstr(RenderCtx *ctx, const gchar *text) {
  if (!text) {
    return;
  }
  insert_text(ctx, text, strlen(text));
}

static void ensure_newlines(RenderCtx *ctx, guint min_newlines) {
  if (!ctx || min_newlines == 0 || !ctx->has_output) {
    return;
  }
  while (ctx->trailing_newlines < min_newlines) {
    insert_cstr(ctx, "\n");
  }
}

static guint decimal_digits(guint value) {
  guint digits = 1;
  while (value >= 10) {
    value /= 10;
    digits++;
  }
  return digits;
}

static guint list_marker_columns(RenderCtx *ctx) {
  ListState *list;
  if (!ctx || !ctx->list_stack || ctx->list_stack->len == 0) {
    return 2; /* "• " */
  }
  list = &g_array_index(ctx->list_stack, ListState, ctx->list_stack->len - 1);
  if (list->ordered) {
    return decimal_digits(MAX(1, list->next_index)) + 2; /* "12. " */
  }
  return 2; /* "• " */
}

static ListLayoutSpec compute_list_layout(guint depth, guint quote_depth,
                                          guint marker_cols) {
  ListLayoutSpec spec = {0, 0};
  guint depth_index = (depth > 0) ? (depth - 1) : 0;
  gint marker_px = (gint)CLAMP((gint)marker_cols * 9, 16, 64);
  gint base_indent = 20 + ((gint)depth_index * 22) + ((gint)quote_depth * 24);

  spec.left_margin = base_indent + marker_px;
  spec.indent = -marker_px;
  return spec;
}

static GtkTextTag *ensure_list_layout_tag(RenderCtx *ctx, guint depth,
                                          guint quote_depth,
                                          guint marker_cols) {
  gchar *name;
  GtkTextTag *tag;
  ListLayoutSpec spec;

  if (!ctx || !ctx->buffer) {
    return NULL;
  }

  name = g_strdup_printf("list_layout_%u_%u_%u", depth, quote_depth, marker_cols);
  tag = lookup_tag(ctx->buffer, name);
  if (!tag) {
    spec = compute_list_layout(depth, quote_depth, marker_cols);
    tag = gtk_text_buffer_create_tag(ctx->buffer, name, "left-margin",
                                     spec.left_margin, "indent", spec.indent, NULL);
  }
  g_free(name);
  return tag;
}

static void push_active_tag(RenderCtx *ctx, GtkTextTag *tag, guint *counter) {
  if (!ctx || !tag) {
    return;
  }
  g_ptr_array_add(ctx->active_tags, tag);
  if (counter) {
    (*counter)++;
  }
}

static void push_active_tag_by_name(RenderCtx *ctx, const gchar *name,
                                    guint *counter) {
  push_active_tag(ctx, lookup_tag(ctx->buffer, name), counter);
}

static void pop_active_tags(RenderCtx *ctx, guint count) {
  if (!ctx) {
    return;
  }
  while (count > 0 && ctx->active_tags->len > 0) {
    g_ptr_array_remove_index(ctx->active_tags, ctx->active_tags->len - 1);
    count--;
  }
}

static gchar *attr_to_string(const MD_ATTRIBUTE *attr) {
  if (!attr || !attr->text || attr->size == 0) {
    return g_strdup("");
  }
  return g_strndup(attr->text, attr->size);
}

static gchar *extract_code_language_from_detail(MD_BLOCK_CODE_DETAIL *code) {
  gchar *lang;
  gchar *info;
  gchar *trimmed;
  const gchar *end;

  if (!code) {
    return NULL;
  }

  lang = attr_to_string(&code->lang);
  g_strstrip(lang);
  if (lang[0] != '\0') {
    return lang;
  }
  g_free(lang);

  info = attr_to_string(&code->info);
  trimmed = g_strstrip(info);
  if (trimmed[0] == '\0') {
    g_free(info);
    return NULL;
  }

  end = trimmed;
  while (*end && !g_ascii_isspace(*end)) {
    end++;
  }

  lang = g_strndup(trimmed, (gsize)(end - trimmed));
  g_free(info);
  return lang;
}

typedef struct {
  GtkTextBuffer *buffer;
  gint line_offset;
} CodeTagApplyContext;

static void on_code_scan_token(gint start_char_offset, gint end_char_offset,
                               const gchar *tag_name, gpointer user_data) {
  CodeTagApplyContext *ctx = (CodeTagApplyContext *)user_data;

  if (!ctx || !ctx->buffer || !tag_name || end_char_offset <= start_char_offset) {
    return;
  }

  apply_tag_by_name_offsets(ctx->buffer, tag_name, ctx->line_offset + start_char_offset,
                            ctx->line_offset + end_char_offset);
}

static void apply_code_highlighting_for_block(GtkTextBuffer *buffer,
                                              const CodeBlockRange *range) {
  GtkTextIter start;
  GtkTextIter end;
  gchar *text;
  const gchar *line_start;
  MarkydCodeScanState state = {0};
  CodeTagApplyContext ctx = {0};
  gint line_offset;

  if (!buffer || !range || !range->language || range->end_offset <= range->start_offset) {
    return;
  }

  gtk_text_buffer_get_iter_at_offset(buffer, &start, range->start_offset);
  gtk_text_buffer_get_iter_at_offset(buffer, &end, range->end_offset);
  text = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
  if (!text) {
    return;
  }

  markyd_code_scan_state_reset(&state);
  ctx.buffer = buffer;
  line_offset = range->start_offset;
  line_start = text;

  while (TRUE) {
    const gchar *nl = strchr(line_start, '\n');
    gsize len = nl ? (gsize)(nl - line_start) : strlen(line_start);
    gchar *line = g_strndup(line_start, len);

    ctx.line_offset = line_offset;
    markyd_code_scan_line(range->language, line, &state, on_code_scan_token, &ctx);
    line_offset += (gint)g_utf8_strlen(line, -1);
    g_free(line);

    if (!nl) {
      break;
    }

    line_offset += 1; /* '\n' */
    line_start = nl + 1;
  }

  g_free(text);
}

static void apply_code_highlighting(GtkTextBuffer *buffer, GArray *code_blocks) {
  if (!buffer || !code_blocks || code_blocks->len == 0) {
    return;
  }

  for (guint i = 0; i < code_blocks->len; i++) {
    CodeBlockRange *range = &g_array_index(code_blocks, CodeBlockRange, i);
    apply_code_highlighting_for_block(buffer, range);
  }
}

static gboolean decode_named_entity(const gchar *name, gsize len,
                                    gunichar *out) {
  if (!name || !out) {
    return FALSE;
  }

#define ENTITY_MATCH(s) (len == strlen(s) && g_ascii_strncasecmp(name, s, len) == 0)
  if (ENTITY_MATCH("amp")) {
    *out = '&';
  } else if (ENTITY_MATCH("lt")) {
    *out = '<';
  } else if (ENTITY_MATCH("gt")) {
    *out = '>';
  } else if (ENTITY_MATCH("quot")) {
    *out = '"';
  } else if (ENTITY_MATCH("apos")) {
    *out = '\'';
  } else if (ENTITY_MATCH("nbsp")) {
    *out = ' ';
  } else if (ENTITY_MATCH("ndash")) {
    *out = 0x2013;
  } else if (ENTITY_MATCH("mdash")) {
    *out = 0x2014;
  } else if (ENTITY_MATCH("hellip")) {
    *out = 0x2026;
  } else if (ENTITY_MATCH("copy")) {
    *out = 0x00A9;
  } else if (ENTITY_MATCH("reg")) {
    *out = 0x00AE;
  } else if (ENTITY_MATCH("trade")) {
    *out = 0x2122;
  } else if (ENTITY_MATCH("lsquo")) {
    *out = 0x2018;
  } else if (ENTITY_MATCH("rsquo")) {
    *out = 0x2019;
  } else if (ENTITY_MATCH("ldquo")) {
    *out = 0x201C;
  } else if (ENTITY_MATCH("rdquo")) {
    *out = 0x201D;
  } else if (ENTITY_MATCH("laquo")) {
    *out = 0x00AB;
  } else if (ENTITY_MATCH("raquo")) {
    *out = 0x00BB;
  } else if (ENTITY_MATCH("bull")) {
    *out = 0x2022;
  } else if (ENTITY_MATCH("middot")) {
    *out = 0x00B7;
  } else {
    return FALSE;
  }
#undef ENTITY_MATCH

  return TRUE;
}

static gboolean decode_numeric_entity(const gchar *name, gsize len,
                                      gunichar *out) {
  gchar *num;
  gchar *endptr = NULL;
  guint base = 10;
  guint start = 1;
  guint64 value;

  if (!name || len < 2 || name[0] != '#' || !out) {
    return FALSE;
  }

  if (len >= 3 && (name[1] == 'x' || name[1] == 'X')) {
    base = 16;
    start = 2;
  }

  if (start >= len) {
    return FALSE;
  }

  num = g_strndup(name + start, len - start);
  value = g_ascii_strtoull(num, &endptr, base);
  if (!endptr || *endptr != '\0' || value > 0x10FFFF ||
      !g_unichar_validate((gunichar)value)) {
    g_free(num);
    return FALSE;
  }

  *out = (gunichar)value;
  g_free(num);
  return TRUE;
}

static gboolean decode_entity_name(const gchar *name, gsize len, gunichar *out) {
  return decode_named_entity(name, len, out) ||
         decode_numeric_entity(name, len, out);
}

static gchar *decode_html_entities(const gchar *text, gsize len) {
  GString *out;
  gsize i = 0;

  if (!text || len == 0) {
    return g_strdup("");
  }

  out = g_string_sized_new(len);
  while (i < len) {
    if (text[i] == '&') {
      gsize semi = i + 1;
      while (semi < len && semi - i <= 64 && text[semi] != ';' &&
             text[semi] != '\n' && text[semi] != '\r' &&
             !g_ascii_isspace(text[semi])) {
        semi++;
      }

      if (semi < len && text[semi] == ';') {
        gunichar c;
        if (decode_entity_name(text + i + 1, semi - i - 1, &c)) {
          g_string_append_unichar(out, c);
          i = semi + 1;
          continue;
        }
      }
    }

    g_string_append_c(out, text[i]);
    i++;
  }

  return g_string_free(out, FALSE);
}

static gchar *md_text_to_utf8(MD_TEXTTYPE type, const MD_CHAR *text,
                              MD_SIZE size) {
  if (type == MD_TEXT_NULLCHAR) {
    GString *s = g_string_new(NULL);
    for (MD_SIZE i = 0; i < size; i++) {
      g_string_append(s, "\xEF\xBF\xBD");
    }
    return g_string_free(s, FALSE);
  }

  if (!text || size == 0) {
    return g_strdup("");
  }

  if (type == MD_TEXT_ENTITY) {
    return decode_html_entities(text, size);
  }

  return g_strndup(text, size);
}

gchar *markdown_normalize_anchor_slug(const gchar *text) {
  gchar *decoded;
  const gchar *p;
  GString *out;
  gboolean prev_dash = TRUE;

  if (!text) {
    return g_strdup("");
  }

  decoded = g_uri_unescape_string(text, NULL);
  p = decoded ? decoded : text;
  out = g_string_new(NULL);

  while (*p) {
    gunichar c = g_utf8_get_char(p);
    if (g_unichar_isalnum(c)) {
      gchar utf8[8];
      gint len = g_unichar_to_utf8(g_unichar_tolower(c), utf8);
      utf8[len] = '\0';
      g_string_append_len(out, utf8, len);
      prev_dash = FALSE;
    } else if (c == ' ' || c == '-' || c == '_') {
      if (!prev_dash) {
        g_string_append_c(out, '-');
        prev_dash = TRUE;
      }
    }
    p = g_utf8_next_char(p);
  }

  while (out->len > 0 && out->str[out->len - 1] == '-') {
    g_string_truncate(out, out->len - 1);
  }

  g_free(decoded);
  return g_string_free(out, FALSE);
}

gchar *markdown_anchor_mark_name(const gchar *fragment) {
  gchar *slug = markdown_normalize_anchor_slug(fragment);
  gchar *name = g_strdup_printf("%s%s", SEEMD_ANCHOR_MARK_PREFIX, slug);
  g_free(slug);
  return name;
}

static void capture_heading_text(RenderCtx *ctx, const gchar *text) {
  if (!ctx || !ctx->in_heading || !ctx->heading_text || !text) {
    return;
  }
  for (const gchar *p = text; *p != '\0'; p++) {
    g_string_append_c(ctx->heading_text, (*p == '\n' || *p == '\r') ? ' ' : *p);
  }
}

static void create_heading_anchor(RenderCtx *ctx) {
  gchar *base;
  guint count;
  gchar *slug;
  gchar *mark_name;
  GtkTextIter at;

  if (!ctx || !ctx->heading_text) {
    return;
  }

  base = markdown_normalize_anchor_slug(ctx->heading_text->str);
  if (!base || base[0] == '\0') {
    g_free(base);
    return;
  }

  count = GPOINTER_TO_UINT(g_hash_table_lookup(ctx->anchor_counts, base));
  slug = (count == 0) ? g_strdup(base) : g_strdup_printf("%s-%u", base, count);
  g_hash_table_replace(ctx->anchor_counts, g_strdup(base), GUINT_TO_POINTER(count + 1));

  mark_name = g_strdup_printf("%s%s", SEEMD_ANCHOR_MARK_PREFIX, slug);
  gtk_text_buffer_get_iter_at_offset(ctx->buffer, &at, ctx->heading_start_offset);
  gtk_text_buffer_create_mark(ctx->buffer, mark_name, &at, TRUE);

  g_free(mark_name);
  g_free(slug);
  g_free(base);
}

static void table_capture_append(RenderCtx *ctx, const gchar *text) {
  if (!ctx || !ctx->table_cell_text || !text) {
    return;
  }
  gchar *escaped = g_markup_escape_text(text, -1);
  g_string_append(ctx->table_cell_text, escaped);
  g_free(escaped);
}

static gchar *table_cell_markup_to_plain(const gchar *markup) {
  gchar *plain = NULL;
  GError *error = NULL;

  if (!markup || markup[0] == '\0') {
    return g_strdup("");
  }

  if (pango_parse_markup(markup, -1, 0, NULL, &plain, NULL, &error)) {
    return plain;
  }

  if (error) {
    g_error_free(error);
  }
  return g_strdup(markup);
}

static void table_search_index_free(gpointer data) {
  SeemdTableSearchIndex *index = (SeemdTableSearchIndex *)data;
  if (!index) {
    return;
  }
  if (index->cells) {
    g_array_free(index->cells, TRUE);
  }
  g_free(index);
}

static void table_emit_hidden_search_text(RenderCtx *ctx, SeemdTable *table,
                                          GtkTextChildAnchor *anchor) {
  SeemdTableSearchIndex *index;
  gboolean saved_has_output;
  guint saved_trailing_newlines;
  const gchar *cell_sep = " ";

  if (!ctx || !ctx->buffer || !table || !anchor || !table->rows ||
      table->rows->len == 0 || table->col_count == 0) {
    return;
  }

  /* Hidden search text must not affect visible block spacing state. */
  saved_has_output = ctx->has_output;
  saved_trailing_newlines = ctx->trailing_newlines;

  index = g_new0(SeemdTableSearchIndex, 1);
  index->cells = g_array_new(FALSE, FALSE, sizeof(SeemdTableSearchCellRange));
  index->start_offset = gtk_text_iter_get_offset(&ctx->iter);

  for (guint r = 0; r < table->rows->len; r++) {
    SeemdTableRow *row = g_ptr_array_index(table->rows, r);
    if (!row) {
      continue;
    }

    for (guint c = 0; c < table->col_count; c++) {
      const gchar *cell_markup = "";
      gchar *plain;
      gint cell_start;
      gint cell_end;

      if (c < row->cells->len) {
        cell_markup = g_ptr_array_index(row->cells, c);
        if (!cell_markup) {
          cell_markup = "";
        }
      }

      plain = table_cell_markup_to_plain(cell_markup);
      cell_start = gtk_text_iter_get_offset(&ctx->iter);
      if (plain && plain[0] != '\0') {
        gtk_text_buffer_insert(ctx->buffer, &ctx->iter, plain, -1);
      }
      cell_end = gtk_text_iter_get_offset(&ctx->iter);

      if (cell_end > cell_start) {
        SeemdTableSearchCellRange cell_range = {(gint)r, (gint)c, cell_start,
                                                 cell_end};
        g_array_append_val(index->cells, cell_range);
      }

      g_free(plain);

      if (!(r + 1 == table->rows->len && c + 1 == table->col_count)) {
        gtk_text_buffer_insert(ctx->buffer, &ctx->iter, cell_sep, -1);
      }
    }
  }

  index->end_offset = gtk_text_iter_get_offset(&ctx->iter);
  if (index->end_offset > index->start_offset) {
    apply_tag_by_name_offsets(ctx->buffer, TAG_INVISIBLE, index->start_offset,
                              index->end_offset);
    g_object_set_data_full(G_OBJECT(anchor), SEEMD_TABLE_SEARCH_INDEX_DATA, index,
                           table_search_index_free);
  } else {
    table_search_index_free(index);
  }

  ctx->has_output = saved_has_output;
  ctx->trailing_newlines = saved_trailing_newlines;
}

static void table_capture_span_enter(RenderCtx *ctx, MD_SPANTYPE type) {
  if (!ctx || !ctx->table_cell_text) {
    return;
  }

  switch (type) {
  case MD_SPAN_EM:
    g_string_append(ctx->table_cell_text, "<i>");
    break;
  case MD_SPAN_STRONG:
    g_string_append(ctx->table_cell_text, "<b>");
    break;
  case MD_SPAN_CODE:
    g_string_append(ctx->table_cell_text, "<span font_family='monospace'>");
    break;
  case MD_SPAN_DEL:
    g_string_append(ctx->table_cell_text, "<s>");
    break;
  case MD_SPAN_A:
    g_string_append(ctx->table_cell_text, "<u>");
    break;
  default:
    break;
  }
}

static void table_capture_span_leave(RenderCtx *ctx, MD_SPANTYPE type) {
  if (!ctx || !ctx->table_cell_text) {
    return;
  }

  switch (type) {
  case MD_SPAN_EM:
    g_string_append(ctx->table_cell_text, "</i>");
    break;
  case MD_SPAN_STRONG:
    g_string_append(ctx->table_cell_text, "</b>");
    break;
  case MD_SPAN_CODE:
    g_string_append(ctx->table_cell_text, "</span>");
    break;
  case MD_SPAN_DEL:
    g_string_append(ctx->table_cell_text, "</s>");
    break;
  case MD_SPAN_A:
    g_string_append(ctx->table_cell_text, "</u>");
    break;
  default:
    break;
  }
}

static void table_start_row(RenderCtx *ctx) {
  SeemdTableRow *row;
  if (!ctx || !ctx->table_model) {
    return;
  }
  row = g_new0(SeemdTableRow, 1);
  row->is_header = ctx->in_table_head;
  row->cells = g_ptr_array_new_with_free_func(g_free);
  g_ptr_array_add(ctx->table_model->rows, row);
  ctx->table_current_row = row;
  ctx->table_current_col = 0;
}

static void table_start_cell(RenderCtx *ctx, void *detail) {
  MD_BLOCK_TD_DETAIL *td = (MD_BLOCK_TD_DETAIL *)detail;
  if (!ctx || !ctx->table_current_row) {
    return;
  }

  if (ctx->table_cell_text) {
    g_string_free(ctx->table_cell_text, TRUE);
  }
  ctx->table_cell_text = g_string_new(NULL);

  if (ctx->table_model && ctx->table_current_col < ctx->table_model->aligns->len) {
    g_array_index(ctx->table_model->aligns, MD_ALIGN, ctx->table_current_col) =
        td ? td->align : MD_ALIGN_DEFAULT;
  }
}

static void table_finish_cell(RenderCtx *ctx) {
  gchar *cell;
  if (!ctx || !ctx->table_current_row || !ctx->table_cell_text) {
    return;
  }
  cell = g_strdup(ctx->table_cell_text->str);
  g_strstrip(cell);
  g_ptr_array_add(ctx->table_current_row->cells, cell);
  g_string_free(ctx->table_cell_text, TRUE);
  ctx->table_cell_text = NULL;
  ctx->table_current_col++;
}

static void table_finish_row(RenderCtx *ctx) {
  if (!ctx || !ctx->table_current_row || !ctx->table_model) {
    return;
  }
  table_ensure_col_count(ctx->table_model, ctx->table_current_row->cells->len);
  while (ctx->table_current_row->cells->len < ctx->table_model->col_count) {
    g_ptr_array_add(ctx->table_current_row->cells, g_strdup(""));
  }
  ctx->table_current_row = NULL;
}

static void table_emit_anchor(RenderCtx *ctx) {
  GtkTextChildAnchor *anchor;

  if (!ctx || !ctx->table_model) {
    return;
  }
  if (ctx->table_model->rows->len == 0 || ctx->table_model->col_count == 0) {
    seemd_table_free(ctx->table_model);
    ctx->table_model = NULL;
    return;
  }

  anchor = gtk_text_buffer_create_child_anchor(ctx->buffer, &ctx->iter);
  note_non_newline_output(ctx);
  g_object_set_data(G_OBJECT(anchor), SEEMD_TABLE_ANCHOR_DATA, GINT_TO_POINTER(1));
  g_object_set_data_full(G_OBJECT(anchor), TABLE_MODEL_DATA_KEY, ctx->table_model,
                         seemd_table_free);

  /* Keep table text searchable via Ctrl+F without showing duplicate content. */
  table_emit_hidden_search_text(ctx, ctx->table_model, anchor);

  ctx->table_model = NULL;
}

static void image_emit_anchor(RenderCtx *ctx) {
  GtkTextChildAnchor *anchor;
  gchar *alt = NULL;

  if (!ctx || !ctx->buffer || !ctx->image_src || ctx->image_src[0] == '\0') {
    return;
  }

  anchor = gtk_text_buffer_create_child_anchor(ctx->buffer, &ctx->iter);
  note_non_newline_output(ctx);
  g_object_set_data(G_OBJECT(anchor), SEEMD_IMAGE_ANCHOR_DATA, GINT_TO_POINTER(1));
  g_object_set_data_full(G_OBJECT(anchor), SEEMD_IMAGE_SRC_DATA,
                         g_strdup(ctx->image_src), g_free);

  if (ctx->image_alt && ctx->image_alt->len > 0) {
    alt = g_strdup(ctx->image_alt->str);
  } else {
    alt = g_strdup("");
  }
  g_object_set_data_full(G_OBJECT(anchor), SEEMD_IMAGE_ALT_DATA, alt, g_free);
  if (ctx->image_width > 0) {
    g_object_set_data(G_OBJECT(anchor), SEEMD_IMAGE_WIDTH_DATA,
                      GINT_TO_POINTER(ctx->image_width));
  }
  if (ctx->image_height > 0) {
    g_object_set_data(G_OBJECT(anchor), SEEMD_IMAGE_HEIGHT_DATA,
                      GINT_TO_POINTER(ctx->image_height));
  }

  /* Keep following markdown on a new visual line after embedded images. */
  insert_cstr(ctx, "\n");
}

static void insert_list_marker(RenderCtx *ctx) {
  ListState *list;
  gchar *ordered = NULL;
  gint marker_start_offset;
  gint marker_end_offset;

  if (!ctx || ctx->list_stack->len == 0) {
    return;
  }

  list = &g_array_index(ctx->list_stack, ListState, ctx->list_stack->len - 1);

  marker_start_offset = gtk_text_iter_get_offset(&ctx->iter);
  if (list->ordered) {
    ordered = g_strdup_printf("%u.", list->next_index++);
    insert_cstr(ctx, ordered);
  } else {
    insert_cstr(ctx, "\xE2\x80\xA2");
  }
  marker_end_offset = gtk_text_iter_get_offset(&ctx->iter);
  apply_tag_by_name_offsets(ctx->buffer, TAG_LIST_BULLET, marker_start_offset,
                            marker_end_offset);

  insert_cstr(ctx, " ");
  g_free(ordered);
}

static gboolean html_name_is(const gchar *name, const gchar *match) {
  return name && match && g_ascii_strcasecmp(name, match) == 0;
}

static gboolean html_name_is_any(const gchar *name, const gchar **matches) {
  if (!name || !matches) {
    return FALSE;
  }
  for (guint i = 0; matches[i] != NULL; i++) {
    if (html_name_is(name, matches[i])) {
      return TRUE;
    }
  }
  return FALSE;
}

static gboolean html_is_void_tag(const gchar *name) {
  static const gchar *void_tags[] = {"area",  "base",  "br",    "col",
                                     "embed", "hr",    "img",   "input",
                                     "link",  "meta",  "param", "source",
                                     "track", "wbr",   NULL};
  return html_name_is_any(name, void_tags);
}

static gboolean html_is_suppressed_tag(const gchar *name) {
  static const gchar *suppressed_tags[] = {"script", "style",  "iframe",
                                           "object", "embed",  "svg",
                                           "canvas", "math",   "template",
                                           NULL};
  return html_name_is_any(name, suppressed_tags);
}

static gboolean html_is_block_tag(const gchar *name) {
  static const gchar *block_tags[] = {
      "address", "article", "aside",   "center", "dd",      "details",
      "dialog",  "div",     "dl",      "dt",     "fieldset", "figcaption",
      "figure",  "footer",  "header",  "main",   "nav",     "p",
      "section", "summary", NULL};
  return html_name_is_any(name, block_tags);
}

static gboolean html_is_heading_tag(const gchar *name, guint *level) {
  if (!name || name[0] != 'h' || name[1] < '1' || name[1] > '6' ||
      name[2] != '\0') {
    return FALSE;
  }
  if (level) {
    *level = (guint)(name[1] - '0');
  }
  return TRUE;
}

static gboolean html_is_inline_style_tag(const gchar *name) {
  static const gchar *inline_tags[] = {"b",      "strong", "i",    "em",
                                       "cite",   "dfn",    "var",  "s",
                                       "strike", "del",    "u",    "ins",
                                       "sup",    "sub",    "kbd",  "mark",
                                       "small",  NULL};
  return html_name_is_any(name, inline_tags);
}

static void html_append_escaped_markup(GString *out, const gchar *text) {
  gchar *escaped;
  if (!out || !text) {
    return;
  }
  escaped = g_markup_escape_text(text, -1);
  g_string_append(out, escaped);
  g_free(escaped);
}

static gchar *html_get_attr(const gchar *attrs, const gchar *name);

static void html_table_capture_tag_enter(RenderCtx *ctx, const gchar *name,
                                         const gchar *attrs) {
  gchar *alt;

  if (!ctx || !ctx->table_cell_text || !name) {
    return;
  }

  if (html_name_is(name, "br")) {
    g_string_append_c(ctx->table_cell_text, '\n');
  } else if (html_name_is(name, "b") || html_name_is(name, "strong")) {
    g_string_append(ctx->table_cell_text, "<b>");
  } else if (html_name_is(name, "i") || html_name_is(name, "em") ||
             html_name_is(name, "cite") || html_name_is(name, "dfn") ||
             html_name_is(name, "var")) {
    g_string_append(ctx->table_cell_text, "<i>");
  } else if (html_name_is(name, "code") || html_name_is(name, "kbd")) {
    g_string_append(ctx->table_cell_text, "<span font_family='monospace'>");
  } else if (html_name_is(name, "s") || html_name_is(name, "strike") ||
             html_name_is(name, "del")) {
    g_string_append(ctx->table_cell_text, "<s>");
  } else if (html_name_is(name, "u") || html_name_is(name, "ins") ||
             html_name_is(name, "a")) {
    g_string_append(ctx->table_cell_text, "<u>");
  } else if (html_name_is(name, "sup")) {
    g_string_append(ctx->table_cell_text, "<span rise='6000' size='smaller'>");
  } else if (html_name_is(name, "sub")) {
    g_string_append(ctx->table_cell_text, "<span rise='-3000' size='smaller'>");
  } else if (html_name_is(name, "small")) {
    g_string_append(ctx->table_cell_text, "<span size='smaller'>");
  } else if (html_name_is(name, "img")) {
    alt = html_get_attr(attrs, "alt");
    if (alt && alt[0] != '\0') {
      html_append_escaped_markup(ctx->table_cell_text, alt);
    }
    g_free(alt);
  } else if (html_is_block_tag(name)) {
    if (ctx->table_cell_text->len > 0 &&
        ctx->table_cell_text->str[ctx->table_cell_text->len - 1] != '\n') {
      g_string_append_c(ctx->table_cell_text, '\n');
    }
  }
}

static void html_table_capture_tag_leave(RenderCtx *ctx, const gchar *name) {
  if (!ctx || !ctx->table_cell_text || !name) {
    return;
  }

  if (html_name_is(name, "b") || html_name_is(name, "strong")) {
    g_string_append(ctx->table_cell_text, "</b>");
  } else if (html_name_is(name, "i") || html_name_is(name, "em") ||
             html_name_is(name, "cite") || html_name_is(name, "dfn") ||
             html_name_is(name, "var")) {
    g_string_append(ctx->table_cell_text, "</i>");
  } else if (html_name_is(name, "code") || html_name_is(name, "kbd")) {
    g_string_append(ctx->table_cell_text, "</span>");
  } else if (html_name_is(name, "s") || html_name_is(name, "strike") ||
             html_name_is(name, "del")) {
    g_string_append(ctx->table_cell_text, "</s>");
  } else if (html_name_is(name, "u") || html_name_is(name, "ins") ||
             html_name_is(name, "a")) {
    g_string_append(ctx->table_cell_text, "</u>");
  } else if (html_name_is(name, "sup")) {
    g_string_append(ctx->table_cell_text, "</span>");
  } else if (html_name_is(name, "sub")) {
    g_string_append(ctx->table_cell_text, "</span>");
  } else if (html_name_is(name, "small")) {
    g_string_append(ctx->table_cell_text, "</span>");
  } else if (html_is_block_tag(name)) {
    if (ctx->table_cell_text->len > 0 &&
        ctx->table_cell_text->str[ctx->table_cell_text->len - 1] != '\n') {
      g_string_append_c(ctx->table_cell_text, '\n');
    }
  }
}

static gchar *html_get_attr(const gchar *attrs, const gchar *name) {
  const gchar *p;

  if (!attrs || !name) {
    return NULL;
  }

  p = attrs;
  while (*p != '\0') {
    const gchar *attr_start;
    const gchar *value_start;
    gboolean matches;
    gchar *raw;
    gchar *decoded;

    while (g_ascii_isspace(*p) || *p == '/') {
      p++;
    }
    if (*p == '\0') {
      break;
    }

    attr_start = p;
    while (*p != '\0' && (g_ascii_isalnum(*p) || *p == '-' || *p == '_' ||
                          *p == ':' || *p == '.')) {
      p++;
    }
    if (p == attr_start) {
      p++;
      continue;
    }

    matches = (strlen(name) == (gsize)(p - attr_start) &&
               g_ascii_strncasecmp(attr_start, name, p - attr_start) == 0);

    while (g_ascii_isspace(*p)) {
      p++;
    }

    if (*p != '=') {
      if (matches) {
        return g_strdup("");
      }
      continue;
    }

    p++;
    while (g_ascii_isspace(*p)) {
      p++;
    }

    if (*p == '"' || *p == '\'') {
      gchar quote = *p++;
      value_start = p;
      while (*p != '\0' && *p != quote) {
        p++;
      }
      raw = g_strndup(value_start, p - value_start);
      if (*p == quote) {
        p++;
      }
    } else {
      value_start = p;
      while (*p != '\0' && !g_ascii_isspace(*p)) {
        p++;
      }
      raw = g_strndup(value_start, p - value_start);
    }

    if (matches) {
      decoded = decode_html_entities(raw, strlen(raw));
      g_free(raw);
      return decoded;
    }

    g_free(raw);
  }

  return NULL;
}

static gint html_get_dimension_attr(const gchar *attrs, const gchar *name) {
  gchar *value;
  gchar *endptr = NULL;
  guint64 parsed;

  value = html_get_attr(attrs, name);
  if (!value) {
    return 0;
  }

  g_strstrip(value);
  if (value[0] == '\0' || value[0] == '-' || strchr(value, '%') != NULL) {
    g_free(value);
    return 0;
  }

  parsed = g_ascii_strtoull(value, &endptr, 10);
  if (!endptr || endptr == value || parsed == 0 || parsed > G_MAXINT) {
    g_free(value);
    return 0;
  }

  while (g_ascii_isspace(*endptr)) {
    endptr++;
  }
  if (*endptr != '\0' && g_ascii_strcasecmp(endptr, "px") != 0) {
    g_free(value);
    return 0;
  }

  g_free(value);
  return (gint)parsed;
}

static gchar *html_extract_tag_name(const gchar *tag_text, gboolean *closing,
                                    const gchar **attrs_start) {
  const gchar *p;
  const gchar *name_start;

  if (closing) {
    *closing = FALSE;
  }
  if (attrs_start) {
    *attrs_start = NULL;
  }
  if (!tag_text) {
    return NULL;
  }

  p = tag_text;
  while (g_ascii_isspace(*p)) {
    p++;
  }
  if (*p == '/') {
    if (closing) {
      *closing = TRUE;
    }
    p++;
    while (g_ascii_isspace(*p)) {
      p++;
    }
  }

  if (*p == '\0' || *p == '!' || *p == '?') {
    return NULL;
  }

  name_start = p;
  while (*p != '\0' && (g_ascii_isalnum(*p) || *p == '-' || *p == '_' ||
                        *p == ':' || *p == '.')) {
    p++;
  }
  if (p == name_start) {
    return NULL;
  }

  if (attrs_start) {
    *attrs_start = p;
  }

  return g_ascii_strdown(name_start, p - name_start);
}

static gboolean html_tag_text_self_closing(const gchar *tag_text,
                                           const gchar *name) {
  const gchar *end;

  if (!tag_text) {
    return FALSE;
  }
  if (html_is_void_tag(name)) {
    return TRUE;
  }

  end = tag_text + strlen(tag_text);
  while (end > tag_text && g_ascii_isspace(*(end - 1))) {
    end--;
  }
  return end > tag_text && *(end - 1) == '/';
}

static gchar *html_language_from_attrs(const gchar *attrs) {
  gchar *lang = html_get_attr(attrs, "lang");
  gchar *class_attr;

  if (lang && lang[0] != '\0') {
    return lang;
  }
  g_free(lang);

  class_attr = html_get_attr(attrs, "class");
  if (class_attr) {
    gchar **tokens = g_strsplit_set(class_attr, " \t\r\n", -1);
    for (guint i = 0; tokens && tokens[i] != NULL; i++) {
      if (g_str_has_prefix(tokens[i], "language-") && tokens[i][9] != '\0') {
        lang = g_strdup(tokens[i] + 9);
        g_strfreev(tokens);
        g_free(class_attr);
        return lang;
      }
      if (g_str_has_prefix(tokens[i], "lang-") && tokens[i][5] != '\0') {
        lang = g_strdup(tokens[i] + 5);
        g_strfreev(tokens);
        g_free(class_attr);
        return lang;
      }
    }
    g_strfreev(tokens);
    g_free(class_attr);
  }

  return html_get_attr(attrs, "data-lang");
}

static void html_emit_text(RenderCtx *ctx, const gchar *text, gsize len) {
  gchar *decoded;

  if (!ctx || !text || len == 0 || ctx->html_suppressed_depth > 0) {
    return;
  }

  decoded = decode_html_entities(text, len);
  if (!decoded || decoded[0] == '\0') {
    g_free(decoded);
    return;
  }

  if (ctx->list_item_prefix_pending) {
    ctx->list_item_prefix_pending = FALSE;
  }

  if (ctx->table_cell_text) {
    table_capture_append(ctx, decoded);
  } else {
    insert_cstr(ctx, decoded);
    capture_heading_text(ctx, decoded);
  }

  g_free(decoded);
}

static void html_push_link_tags(RenderCtx *ctx, HtmlElement *el,
                                const gchar *href) {
  GtkTextTag *url_tag;

  if (!ctx || !el || !href || href[0] == '\0') {
    return;
  }

  url_tag = gtk_text_buffer_create_tag(ctx->buffer, NULL, NULL);
  g_object_set_data_full(G_OBJECT(url_tag), SEEMD_LINK_URL_DATA, g_strdup(href),
                         g_free);
  push_active_tag_by_name(ctx, TAG_LINK, &el->pushed_tags);
  push_active_tag(ctx, url_tag, &el->pushed_tags);
}

static void html_push_inline_style_tag(RenderCtx *ctx, HtmlElement *el,
                                       const gchar *name) {
  if (!ctx || !el || !name) {
    return;
  }

  if (html_name_is(name, "b") || html_name_is(name, "strong")) {
    push_active_tag_by_name(ctx, TAG_BOLD, &el->pushed_tags);
  } else if (html_name_is(name, "i") || html_name_is(name, "em") ||
             html_name_is(name, "cite") || html_name_is(name, "dfn") ||
             html_name_is(name, "var")) {
    push_active_tag_by_name(ctx, TAG_ITALIC, &el->pushed_tags);
  } else if (html_name_is(name, "s") || html_name_is(name, "strike") ||
             html_name_is(name, "del")) {
    push_active_tag_by_name(ctx, TAG_STRIKE, &el->pushed_tags);
  } else if (html_name_is(name, "u") || html_name_is(name, "ins")) {
    push_active_tag_by_name(ctx, TAG_UNDERLINE, &el->pushed_tags);
  } else if (html_name_is(name, "sup")) {
    push_active_tag_by_name(ctx, TAG_SUP, &el->pushed_tags);
  } else if (html_name_is(name, "sub")) {
    push_active_tag_by_name(ctx, TAG_SUB, &el->pushed_tags);
  } else if (html_name_is(name, "kbd")) {
    push_active_tag_by_name(ctx, TAG_KBD, &el->pushed_tags);
  } else if (html_name_is(name, "mark")) {
    push_active_tag_by_name(ctx, TAG_TABLE_SEPARATOR, &el->pushed_tags);
  } else if (html_name_is(name, "small")) {
    push_active_tag_by_name(ctx, TAG_SMALL, &el->pushed_tags);
  }
}

static void html_open_heading(RenderCtx *ctx, HtmlElement *el, guint level) {
  if (!ctx || !el) {
    return;
  }

  ensure_newlines(ctx, 2);
  ctx->heading_start_offset = gtk_text_iter_get_offset(&ctx->iter);
  ctx->in_heading = TRUE;
  if (!ctx->heading_text) {
    ctx->heading_text = g_string_new(NULL);
  } else {
    g_string_set_size(ctx->heading_text, 0);
  }
  el->heading_started = TRUE;

  if (level == 1) {
    push_active_tag_by_name(ctx, TAG_H1, &el->pushed_tags);
  } else if (level == 2) {
    push_active_tag_by_name(ctx, TAG_H2, &el->pushed_tags);
  } else {
    push_active_tag_by_name(ctx, TAG_H3, &el->pushed_tags);
  }
}

static void html_start_pre(RenderCtx *ctx, HtmlElement *el,
                           const gchar *attrs) {
  gchar *language;

  if (!ctx || !el) {
    return;
  }

  ensure_newlines(ctx, 2);
  ctx->current_code_start_offset = gtk_text_iter_get_offset(&ctx->iter);
  language = html_language_from_attrs(attrs);
  ctx->current_code_language = markyd_code_lookup_language(language);
  g_free(language);
  push_active_tag_by_name(ctx, TAG_CODE_BLOCK, &el->pushed_tags);
  ctx->html_pre_depth++;
  el->pre_started = TRUE;
}

static void html_finish_pre(RenderCtx *ctx) {
  if (!ctx) {
    return;
  }

  if (ctx->code_blocks && ctx->current_code_start_offset >= 0) {
    CodeBlockRange range = {ctx->current_code_start_offset,
                            gtk_text_iter_get_offset(&ctx->iter),
                            ctx->current_code_language};
    if (range.end_offset > range.start_offset) {
      g_array_append_val(ctx->code_blocks, range);
    }
  }
  ctx->current_code_start_offset = -1;
  ctx->current_code_language = NULL;
  if (ctx->html_pre_depth > 0) {
    ctx->html_pre_depth--;
  }
  ensure_newlines(ctx, 2);
}

static void html_emit_hr(RenderCtx *ctx) {
  gint start_offset;
  gint end_offset;

  if (!ctx || ctx->html_suppressed_depth > 0) {
    return;
  }

  ensure_newlines(ctx, 2);
  start_offset = gtk_text_iter_get_offset(&ctx->iter);
  insert_cstr(ctx, "\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80");
  end_offset = gtk_text_iter_get_offset(&ctx->iter);
  apply_tag_by_name_offsets(ctx->buffer, TAG_HRULE, start_offset, end_offset);
  ensure_newlines(ctx, 2);
}

static void html_emit_image(RenderCtx *ctx, const gchar *attrs) {
  gchar *src;
  gchar *alt;

  if (!ctx || ctx->html_suppressed_depth > 0) {
    return;
  }

  src = html_get_attr(attrs, "src");
  alt = html_get_attr(attrs, "alt");
  if (src && src[0] != '\0') {
    g_free(ctx->image_src);
    ctx->image_src = g_strdup(src);
    ctx->image_width = html_get_dimension_attr(attrs, "width");
    ctx->image_height = html_get_dimension_attr(attrs, "height");
    if (!ctx->image_alt) {
      ctx->image_alt = g_string_new(NULL);
    } else {
      g_string_set_size(ctx->image_alt, 0);
    }
    if (alt) {
      g_string_append(ctx->image_alt, alt);
    }
    image_emit_anchor(ctx);
    g_free(ctx->image_src);
    ctx->image_src = NULL;
    ctx->image_width = 0;
    ctx->image_height = 0;
    if (ctx->image_alt) {
      g_string_set_size(ctx->image_alt, 0);
    }
  } else if (alt && alt[0] != '\0') {
    html_emit_text(ctx, alt, strlen(alt));
  }
  g_free(src);
  g_free(alt);
}

static guint html_ol_start_from_attrs(const gchar *attrs) {
  gchar *start = html_get_attr(attrs, "start");
  guint value = 1;

  if (start && start[0] != '\0') {
    gchar *endptr = NULL;
    guint64 parsed = g_ascii_strtoull(start, &endptr, 10);
    if (endptr && *endptr == '\0' && parsed > 0 && parsed <= G_MAXUINT) {
      value = (guint)parsed;
    }
  }
  g_free(start);
  return value;
}

static void html_close_element(RenderCtx *ctx, HtmlElement *el);

static void html_open_element(RenderCtx *ctx, const gchar *name,
                              const gchar *attrs, gboolean self_closing) {
  HtmlElement *el;
  guint heading_level = 0;

  if (!ctx || !name || name[0] == '\0') {
    return;
  }

  if (html_name_is(name, "br")) {
    if (ctx->table_cell_text) {
      html_table_capture_tag_enter(ctx, name, attrs);
    } else if (ctx->html_suppressed_depth == 0) {
      insert_cstr(ctx, "\n");
    }
    return;
  }
  if (html_name_is(name, "hr")) {
    html_emit_hr(ctx);
    return;
  }
  if (html_name_is(name, "img")) {
    if (ctx->table_cell_text) {
      html_table_capture_tag_enter(ctx, name, attrs);
    } else {
      html_emit_image(ctx, attrs);
    }
    return;
  }

  el = g_new0(HtmlElement, 1);
  el->name = g_strdup(name);

  if (html_is_suppressed_tag(name)) {
    el->kind = HTML_EL_SUPPRESSED;
    ctx->html_suppressed_depth++;
  } else if (ctx->html_suppressed_depth > 0) {
    el->kind = HTML_EL_GENERIC;
  } else if (ctx->table_cell_text &&
             !(html_name_is(name, "table") || html_name_is(name, "thead") ||
               html_name_is(name, "tbody") || html_name_is(name, "tr") ||
               html_name_is(name, "td") || html_name_is(name, "th"))) {
    el->kind = HTML_EL_INLINE;
    html_table_capture_tag_enter(ctx, name, attrs);
    el->table_cell_started = TRUE;
  } else if (html_is_heading_tag(name, &heading_level)) {
    el->kind = HTML_EL_HEADING;
    html_open_heading(ctx, el, heading_level);
  } else if (html_name_is(name, "blockquote")) {
    el->kind = HTML_EL_QUOTE;
    ensure_newlines(ctx, 2);
    ctx->quote_depth++;
    el->quote_started = TRUE;
    push_active_tag_by_name(ctx, TAG_QUOTE, &el->pushed_tags);
  } else if (html_name_is(name, "ul")) {
    ListState list = {FALSE, 1};
    el->kind = HTML_EL_UL;
    ensure_newlines(ctx, (ctx->list_stack->len > 0) ? 1 : 2);
    g_array_append_val(ctx->list_stack, list);
    el->list_started = TRUE;
  } else if (html_name_is(name, "ol")) {
    ListState list = {TRUE, html_ol_start_from_attrs(attrs)};
    el->kind = HTML_EL_OL;
    ensure_newlines(ctx, (ctx->list_stack->len > 0) ? 1 : 2);
    g_array_append_val(ctx->list_stack, list);
    el->list_started = TRUE;
  } else if (html_name_is(name, "li")) {
    guint marker_cols = list_marker_columns(ctx);
    GtkTextTag *layout_tag = ensure_list_layout_tag(
        ctx, ctx->list_stack->len, ctx->quote_depth, marker_cols);
    el->kind = HTML_EL_LI;
    ensure_newlines(ctx, 1);
    push_active_tag(ctx, layout_tag, &el->pushed_tags);
    insert_list_marker(ctx);
    ctx->list_item_prefix_pending = TRUE;
  } else if (html_name_is(name, "a")) {
    gchar *href = html_get_attr(attrs, "href");
    el->kind = HTML_EL_LINK;
    html_push_link_tags(ctx, el, href);
    g_free(href);
  } else if (html_name_is(name, "pre")) {
    el->kind = HTML_EL_PRE;
    html_start_pre(ctx, el, attrs);
  } else if (html_name_is(name, "code")) {
    el->kind = HTML_EL_CODE;
    if (ctx->html_pre_depth == 0) {
      push_active_tag_by_name(ctx, TAG_CODE, &el->pushed_tags);
    } else if (!ctx->current_code_language) {
      gchar *language = html_language_from_attrs(attrs);
      ctx->current_code_language = markyd_code_lookup_language(language);
      g_free(language);
    }
  } else if (html_name_is(name, "table")) {
    el->kind = HTML_EL_TABLE;
    ensure_newlines(ctx, 2);
    if (ctx->table_model) {
      seemd_table_free(ctx->table_model);
    }
    ctx->table_model = seemd_table_new(0);
    ctx->table_current_row = NULL;
    ctx->in_table_head = FALSE;
  } else if (html_name_is(name, "thead")) {
    el->kind = HTML_EL_THEAD;
    ctx->in_table_head = TRUE;
  } else if (html_name_is(name, "tbody")) {
    el->kind = HTML_EL_TBODY;
    ctx->in_table_head = FALSE;
  } else if (html_name_is(name, "tr")) {
    el->kind = HTML_EL_TR;
    table_start_row(ctx);
  } else if (html_name_is(name, "th")) {
    el->kind = HTML_EL_TH;
    if (ctx->table_model && !ctx->table_current_row) {
      table_start_row(ctx);
    }
    if (ctx->table_current_row) {
      ctx->table_current_row->is_header = TRUE;
    }
    table_start_cell(ctx, NULL);
    el->table_cell_started = TRUE;
  } else if (html_name_is(name, "td")) {
    el->kind = HTML_EL_TD;
    if (ctx->table_model && !ctx->table_current_row) {
      table_start_row(ctx);
    }
    table_start_cell(ctx, NULL);
    el->table_cell_started = TRUE;
  } else if (html_is_inline_style_tag(name)) {
    el->kind = HTML_EL_INLINE;
    html_push_inline_style_tag(ctx, el, name);
  } else if (html_is_block_tag(name)) {
    el->kind = HTML_EL_BLOCK;
    ensure_newlines(ctx, 2);
    if (html_name_is(name, "summary")) {
      push_active_tag_by_name(ctx, TAG_BOLD, &el->pushed_tags);
    }
  } else {
    el->kind = HTML_EL_GENERIC;
  }

  if (self_closing) {
    html_close_element(ctx, el);
    html_element_free(el);
  } else {
    g_ptr_array_add(ctx->html_stack, el);
  }
}

static void html_close_element(RenderCtx *ctx, HtmlElement *el) {
  if (!ctx || !el) {
    return;
  }

  if (el->table_cell_started &&
      (el->kind == HTML_EL_INLINE || el->kind == HTML_EL_BLOCK)) {
    html_table_capture_tag_leave(ctx, el->name);
  }

  if (el->kind == HTML_EL_HEADING && el->heading_started) {
    create_heading_anchor(ctx);
    ctx->in_heading = FALSE;
    ensure_newlines(ctx, 1);
  } else if (el->kind == HTML_EL_QUOTE) {
    if (ctx->quote_depth > 0) {
      ctx->quote_depth--;
    }
    ensure_newlines(ctx, 2);
  } else if ((el->kind == HTML_EL_UL || el->kind == HTML_EL_OL) &&
             el->list_started) {
    if (ctx->list_stack->len > 0) {
      g_array_set_size(ctx->list_stack, ctx->list_stack->len - 1);
    }
    ensure_newlines(ctx, 1);
  } else if (el->kind == HTML_EL_LI) {
    ctx->list_item_prefix_pending = FALSE;
    ensure_newlines(ctx, 1);
  } else if (el->kind == HTML_EL_PRE && el->pre_started) {
    html_finish_pre(ctx);
  } else if (el->kind == HTML_EL_TH || el->kind == HTML_EL_TD) {
    table_finish_cell(ctx);
  } else if (el->kind == HTML_EL_TR) {
    table_finish_row(ctx);
  } else if (el->kind == HTML_EL_THEAD) {
    ctx->in_table_head = FALSE;
  } else if (el->kind == HTML_EL_TABLE) {
    table_finish_row(ctx);
    table_emit_anchor(ctx);
    ensure_newlines(ctx, 2);
  } else if (el->kind == HTML_EL_BLOCK) {
    ensure_newlines(ctx, html_name_is(el->name, "summary") ? 1 : 2);
  } else if (el->kind == HTML_EL_SUPPRESSED) {
    if (ctx->html_suppressed_depth > 0) {
      ctx->html_suppressed_depth--;
    }
  }

  pop_active_tags(ctx, el->pushed_tags);
}

static void html_pop_stack_at(RenderCtx *ctx, guint index) {
  HtmlElement *el;

  if (!ctx || !ctx->html_stack || index >= ctx->html_stack->len) {
    return;
  }

  el = g_ptr_array_index(ctx->html_stack, index);
  g_ptr_array_remove_index(ctx->html_stack, index);
  html_close_element(ctx, el);
  html_element_free(el);
}

static void html_close_to_name(RenderCtx *ctx, const gchar *name) {
  gint match = -1;

  if (!ctx || !ctx->html_stack || !name) {
    return;
  }

  for (gint i = (gint)ctx->html_stack->len - 1; i >= 0; i--) {
    HtmlElement *el = g_ptr_array_index(ctx->html_stack, (guint)i);
    if (el && html_name_is(el->name, name)) {
      match = i;
      break;
    }
  }

  if (match < 0) {
    return;
  }

  while ((gint)ctx->html_stack->len > match) {
    html_pop_stack_at(ctx, ctx->html_stack->len - 1);
  }
}

static void html_close_all(RenderCtx *ctx) {
  if (!ctx || !ctx->html_stack) {
    return;
  }

  while (ctx->html_stack->len > 0) {
    html_pop_stack_at(ctx, ctx->html_stack->len - 1);
  }
}

static void html_handle_tag(RenderCtx *ctx, const gchar *tag_text) {
  gboolean closing = FALSE;
  const gchar *attrs = NULL;
  gchar *name;
  gboolean self_closing;

  if (!ctx || !tag_text) {
    return;
  }

  name = html_extract_tag_name(tag_text, &closing, &attrs);
  if (!name) {
    return;
  }

  if (closing) {
    html_close_to_name(ctx, name);
    g_free(name);
    return;
  }

  self_closing = html_tag_text_self_closing(tag_text, name);
  html_open_element(ctx, name, attrs, self_closing);
  g_free(name);
}

static gssize html_find_tag_end(const gchar *text, gsize len, gsize start) {
  gchar quote = 0;

  for (gsize i = start; i < len; i++) {
    if (quote) {
      if (text[i] == quote) {
        quote = 0;
      }
      continue;
    }
    if (text[i] == '"' || text[i] == '\'') {
      quote = text[i];
      continue;
    }
    if (text[i] == '>') {
      return (gssize)i;
    }
  }

  return -1;
}

static const gchar *html_find_comment_end(const gchar *text, gsize len,
                                          gsize start) {
  for (gsize i = start; i + 2 < len; i++) {
    if (text[i] == '-' && text[i + 1] == '-' && text[i + 2] == '>') {
      return text + i + 3;
    }
  }
  return NULL;
}

static gsize html_text_emit_end(const gchar *text, gsize start, gsize end,
                                gboolean final) {
  gsize last_amp = end;

  if (final) {
    return end;
  }

  for (gsize i = start; i < end; i++) {
    if (text[i] == '&') {
      last_amp = i;
    } else if (text[i] == ';') {
      last_amp = end;
    } else if (last_amp != end && g_ascii_isspace(text[i])) {
      last_amp = end;
    }
  }

  return last_amp == end ? end : last_amp;
}

static void html_process_pending(RenderCtx *ctx, gboolean final) {
  gsize pos = 0;

  if (!ctx || !ctx->html_pending || ctx->html_pending->len == 0) {
    return;
  }

  while (pos < ctx->html_pending->len) {
    const gchar *text = ctx->html_pending->str;
    gsize len = ctx->html_pending->len;

    if (text[pos] == '<') {
      if (pos + 4 <= len && strncmp(text + pos, "<!--", 4) == 0) {
        const gchar *comment_end = html_find_comment_end(text, len, pos + 4);
        if (!comment_end) {
          if (final) {
            pos = len;
          }
          break;
        }
        pos = (gsize)(comment_end - text);
        continue;
      }

      gssize tag_end = html_find_tag_end(text, len, pos + 1);
      if (tag_end < 0) {
        if (final) {
          html_emit_text(ctx, text + pos, 1);
          pos++;
          continue;
        }
        break;
      }

      gchar *tag_text = g_strndup(text + pos + 1, (gsize)tag_end - pos - 1);
      html_handle_tag(ctx, tag_text);
      g_free(tag_text);
      pos = (gsize)tag_end + 1;
    } else {
      gsize next = pos;
      gsize emit_end;
      while (next < ctx->html_pending->len && ctx->html_pending->str[next] != '<') {
        next++;
      }
      emit_end = html_text_emit_end(ctx->html_pending->str, pos, next, final);
      if (emit_end == pos && !final) {
        break;
      }
      html_emit_text(ctx, ctx->html_pending->str + pos, emit_end - pos);
      pos = emit_end;
    }
  }

  if (pos > 0) {
    g_string_erase(ctx->html_pending, 0, pos);
  }
}

static void html_render(RenderCtx *ctx, const gchar *text, gsize len) {
  if (!ctx || !text || len == 0) {
    return;
  }
  if (!ctx->html_pending) {
    ctx->html_pending = g_string_new(NULL);
  }
  g_string_append_len(ctx->html_pending, text, len);
  html_process_pending(ctx, FALSE);
}

static void html_flush(RenderCtx *ctx) {
  if (!ctx || !ctx->html_pending) {
    return;
  }
  html_process_pending(ctx, TRUE);
}

static int on_enter_block(MD_BLOCKTYPE type, void *detail, void *userdata) {
  RenderCtx *ctx = (RenderCtx *)userdata;
  BlockState state = {type, 0};

  g_array_append_val(ctx->block_stack, state);

  switch (type) {
  case MD_BLOCK_DOC:
    break;

  case MD_BLOCK_P:
    if (ctx->list_item_prefix_pending) {
      ctx->list_item_prefix_pending = FALSE;
    } else {
      ensure_newlines(ctx, (ctx->list_stack->len > 0) ? 1 : 2);
    }
    break;

  case MD_BLOCK_H: {
    MD_BLOCK_H_DETAIL *h = (MD_BLOCK_H_DETAIL *)detail;
    ensure_newlines(ctx, 2);
    ctx->heading_start_offset = gtk_text_iter_get_offset(&ctx->iter);
    ctx->in_heading = TRUE;
    if (!ctx->heading_text) {
      ctx->heading_text = g_string_new(NULL);
    } else {
      g_string_set_size(ctx->heading_text, 0);
    }

    if (h && h->level == 1) {
      push_active_tag_by_name(ctx, TAG_H1,
                              &g_array_index(ctx->block_stack, BlockState,
                                             ctx->block_stack->len - 1)
                                   .pushed_tags);
    } else if (h && h->level == 2) {
      push_active_tag_by_name(ctx, TAG_H2,
                              &g_array_index(ctx->block_stack, BlockState,
                                             ctx->block_stack->len - 1)
                                   .pushed_tags);
    } else {
      push_active_tag_by_name(ctx, TAG_H3,
                              &g_array_index(ctx->block_stack, BlockState,
                                             ctx->block_stack->len - 1)
                                   .pushed_tags);
    }
    break;
  }

  case MD_BLOCK_QUOTE:
    ensure_newlines(ctx, 2);
    ctx->quote_depth++;
    push_active_tag_by_name(ctx, TAG_QUOTE,
                            &g_array_index(ctx->block_stack, BlockState,
                                           ctx->block_stack->len - 1)
                                 .pushed_tags);
    break;

  case MD_BLOCK_UL: {
    ListState list = {FALSE, 1};
    ensure_newlines(ctx, (ctx->list_stack->len > 0) ? 1 : 2);
    g_array_append_val(ctx->list_stack, list);
    break;
  }

  case MD_BLOCK_OL: {
    MD_BLOCK_OL_DETAIL *ol = (MD_BLOCK_OL_DETAIL *)detail;
    ListState list = {TRUE, (ol && ol->start > 0) ? ol->start : 1};
    ensure_newlines(ctx, (ctx->list_stack->len > 0) ? 1 : 2);
    g_array_append_val(ctx->list_stack, list);
    break;
  }

  case MD_BLOCK_LI:
    {
      guint marker_cols = list_marker_columns(ctx);
      GtkTextTag *layout_tag = ensure_list_layout_tag(
          ctx, ctx->list_stack->len, ctx->quote_depth, marker_cols);

      ensure_newlines(ctx, 1);
      push_active_tag(ctx, layout_tag,
                      &g_array_index(ctx->block_stack, BlockState,
                                     ctx->block_stack->len - 1)
                           .pushed_tags);
      insert_list_marker(ctx);
      ctx->list_item_prefix_pending = TRUE;
    }
    break;

  case MD_BLOCK_HR: {
    gint start_offset;
    gint end_offset;
    ensure_newlines(ctx, 2);
    start_offset = gtk_text_iter_get_offset(&ctx->iter);
    insert_cstr(ctx, "\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80");
    end_offset = gtk_text_iter_get_offset(&ctx->iter);
    apply_tag_by_name_offsets(ctx->buffer, TAG_HRULE, start_offset, end_offset);
    ensure_newlines(ctx, 2);
    break;
  }

  case MD_BLOCK_CODE:
    ctx->current_code_start_offset = gtk_text_iter_get_offset(&ctx->iter);
    {
      gchar *language =
          extract_code_language_from_detail((MD_BLOCK_CODE_DETAIL *)detail);
      ctx->current_code_language = markyd_code_lookup_language(language);
      g_free(language);
    }
    ensure_newlines(ctx, 2);
    push_active_tag_by_name(ctx, TAG_CODE_BLOCK,
                            &g_array_index(ctx->block_stack, BlockState,
                                           ctx->block_stack->len - 1)
                                 .pushed_tags);
    break;

  case MD_BLOCK_HTML:
    ensure_newlines(ctx, 2);
    break;

  case MD_BLOCK_TABLE: {
    MD_BLOCK_TABLE_DETAIL *tbl = (MD_BLOCK_TABLE_DETAIL *)detail;
    ensure_newlines(ctx, 2);
    if (ctx->table_model) {
      seemd_table_free(ctx->table_model);
    }
    ctx->table_model = seemd_table_new(tbl ? tbl->col_count : 0);
    ctx->table_current_row = NULL;
    ctx->in_table_head = FALSE;
    break;
  }

  case MD_BLOCK_THEAD:
    ctx->in_table_head = TRUE;
    break;

  case MD_BLOCK_TBODY:
    ctx->in_table_head = FALSE;
    break;

  case MD_BLOCK_TR:
    table_start_row(ctx);
    break;

  case MD_BLOCK_TH:
  case MD_BLOCK_TD:
    table_start_cell(ctx, detail);
    break;
  }

  return 0;
}

static int on_leave_block(MD_BLOCKTYPE type, void *detail, void *userdata) {
  RenderCtx *ctx = (RenderCtx *)userdata;
  (void)detail;

  if (type == MD_BLOCK_H) {
    create_heading_anchor(ctx);
    ctx->in_heading = FALSE;
    ensure_newlines(ctx, 1);
  } else if (type == MD_BLOCK_QUOTE) {
    if (ctx->quote_depth > 0) {
      ctx->quote_depth--;
    }
  } else if (type == MD_BLOCK_UL || type == MD_BLOCK_OL) {
    if (ctx->list_stack->len > 0) {
      g_array_set_size(ctx->list_stack, ctx->list_stack->len - 1);
    }
  } else if (type == MD_BLOCK_LI) {
    ctx->list_item_prefix_pending = FALSE;
    ensure_newlines(ctx, 1);
  } else if (type == MD_BLOCK_CODE) {
    if (ctx->code_blocks && ctx->current_code_start_offset >= 0) {
      CodeBlockRange range = {ctx->current_code_start_offset,
                              gtk_text_iter_get_offset(&ctx->iter),
                              ctx->current_code_language};
      if (range.end_offset > range.start_offset) {
        g_array_append_val(ctx->code_blocks, range);
      }
    }
    ctx->current_code_start_offset = -1;
    ctx->current_code_language = NULL;
  } else if (type == MD_BLOCK_TH || type == MD_BLOCK_TD) {
    table_finish_cell(ctx);
  } else if (type == MD_BLOCK_TR) {
    table_finish_row(ctx);
  } else if (type == MD_BLOCK_TABLE) {
    table_finish_row(ctx);
    table_emit_anchor(ctx);
    ensure_newlines(ctx, 2);
  }

  if (ctx->block_stack->len > 0) {
    BlockState state =
        g_array_index(ctx->block_stack, BlockState, ctx->block_stack->len - 1);
    pop_active_tags(ctx, state.pushed_tags);
    g_array_set_size(ctx->block_stack, ctx->block_stack->len - 1);
  }

  return 0;
}

static int on_enter_span(MD_SPANTYPE type, void *detail, void *userdata) {
  RenderCtx *ctx = (RenderCtx *)userdata;
  SpanState state = {type, 0};
  gboolean capture_cell = (ctx && ctx->table_cell_text);

  g_array_append_val(ctx->span_stack, state);

  if (capture_cell) {
    table_capture_span_enter(ctx, type);
    return 0;
  }

  switch (type) {
  case MD_SPAN_EM:
    push_active_tag_by_name(ctx, TAG_ITALIC,
                            &g_array_index(ctx->span_stack, SpanState,
                                           ctx->span_stack->len - 1)
                                 .pushed_tags);
    break;

  case MD_SPAN_STRONG:
    push_active_tag_by_name(ctx, TAG_BOLD,
                            &g_array_index(ctx->span_stack, SpanState,
                                           ctx->span_stack->len - 1)
                                 .pushed_tags);
    break;

  case MD_SPAN_CODE:
    push_active_tag_by_name(ctx, TAG_CODE,
                            &g_array_index(ctx->span_stack, SpanState,
                                           ctx->span_stack->len - 1)
                                 .pushed_tags);
    break;

  case MD_SPAN_DEL:
    push_active_tag_by_name(ctx, TAG_STRIKE,
                            &g_array_index(ctx->span_stack, SpanState,
                                           ctx->span_stack->len - 1)
                                 .pushed_tags);
    break;

  case MD_SPAN_A: {
    MD_SPAN_A_DETAIL *a = (MD_SPAN_A_DETAIL *)detail;
    gchar *href = attr_to_string(a ? &a->href : NULL);
    GtkTextTag *url_tag = gtk_text_buffer_create_tag(ctx->buffer, NULL, NULL);
    g_object_set_data_full(G_OBJECT(url_tag), SEEMD_LINK_URL_DATA, href, g_free);
    push_active_tag_by_name(ctx, TAG_LINK,
                            &g_array_index(ctx->span_stack, SpanState,
                                           ctx->span_stack->len - 1)
                                 .pushed_tags);
    push_active_tag(ctx, url_tag,
                    &g_array_index(ctx->span_stack, SpanState,
                                   ctx->span_stack->len - 1)
                         .pushed_tags);
    break;
  }

  case MD_SPAN_IMG: {
    MD_SPAN_IMG_DETAIL *img = (MD_SPAN_IMG_DETAIL *)detail;
    g_free(ctx->image_src);
    ctx->image_src = attr_to_string(img ? &img->src : NULL);
    ctx->image_width = 0;
    ctx->image_height = 0;
    if (!ctx->image_alt) {
      ctx->image_alt = g_string_new(NULL);
    } else {
      g_string_set_size(ctx->image_alt, 0);
    }
    ctx->in_image = TRUE;
    break;
  }

  default:
    break;
  }

  return 0;
}

static int on_leave_span(MD_SPANTYPE type, void *detail, void *userdata) {
  RenderCtx *ctx = (RenderCtx *)userdata;
  (void)detail;

  if (ctx->span_stack->len > 0) {
    SpanState state =
        g_array_index(ctx->span_stack, SpanState, ctx->span_stack->len - 1);
    if (state.type == MD_SPAN_IMG && ctx->in_image) {
      image_emit_anchor(ctx);
      ctx->in_image = FALSE;
      g_free(ctx->image_src);
      ctx->image_src = NULL;
      ctx->image_width = 0;
      ctx->image_height = 0;
      if (ctx->image_alt) {
        g_string_set_size(ctx->image_alt, 0);
      }
    }
    if (ctx->table_cell_text) {
      table_capture_span_leave(ctx, state.type);
    }
    pop_active_tags(ctx, state.pushed_tags);
    g_array_set_size(ctx->span_stack, ctx->span_stack->len - 1);
  }

  (void)type;

  return 0;
}

static int on_text(MD_TEXTTYPE type, const MD_CHAR *text, MD_SIZE size,
                   void *userdata) {
  RenderCtx *ctx = (RenderCtx *)userdata;
  gchar *rendered = NULL;

  if (type == MD_TEXT_HTML) {
    html_render(ctx, text, size);
    return 0;
  }

  if (type == MD_TEXT_BR || type == MD_TEXT_SOFTBR) {
    rendered = g_strdup("\n");
  } else {
    rendered = md_text_to_utf8(type, text, size);
  }

  if (ctx->list_item_prefix_pending && rendered[0] != '\0') {
    ctx->list_item_prefix_pending = FALSE;
  }
  if (ctx->in_image) {
    if (!ctx->image_alt) {
      ctx->image_alt = g_string_new(NULL);
    }
    g_string_append(ctx->image_alt, rendered);
  } else if (ctx->table_cell_text) {
    table_capture_append(ctx, rendered);
  } else {
    insert_cstr(ctx, rendered);
    capture_heading_text(ctx, rendered);
  }
  g_free(rendered);
  return 0;
}

void markdown_init_tags(GtkTextBuffer *buffer) {
  gtk_text_buffer_create_tag(buffer, TAG_INVISIBLE, "invisible", TRUE, NULL);

  gtk_text_buffer_create_tag(buffer, TAG_H1, "weight", PANGO_WEIGHT_BOLD, "scale",
                             2.0, "foreground", config->h1_color,
                             "pixels-below-lines", 12, NULL);
  gtk_text_buffer_create_tag(buffer, TAG_H2, "weight", PANGO_WEIGHT_BOLD, "scale",
                             1.6, "foreground", config->h2_color,
                             "pixels-below-lines", 10, NULL);
  gtk_text_buffer_create_tag(buffer, TAG_H3, "weight", PANGO_WEIGHT_BOLD, "scale",
                             1.3, "foreground", config->h3_color,
                             "pixels-below-lines", 8, NULL);

  gtk_text_buffer_create_tag(buffer, TAG_BOLD, "weight", PANGO_WEIGHT_BOLD, NULL);
  gtk_text_buffer_create_tag(buffer, TAG_ITALIC, "style", PANGO_STYLE_ITALIC,
                             NULL);
  gtk_text_buffer_create_tag(buffer, TAG_STRIKE, "strikethrough", TRUE, NULL);
  gtk_text_buffer_create_tag(buffer, TAG_UNDERLINE, "underline",
                             PANGO_UNDERLINE_SINGLE, NULL);
  gtk_text_buffer_create_tag(buffer, TAG_SUP, "rise", 6000, "scale", 0.82,
                             NULL);
  gtk_text_buffer_create_tag(buffer, TAG_SUB, "rise", -3000, "scale", 0.82,
                             NULL);
  gtk_text_buffer_create_tag(buffer, TAG_SMALL, "scale", 0.85, NULL);
  gtk_text_buffer_create_tag(buffer, TAG_KBD, "family", "Monospace",
                             "background", "#3E4451", "foreground", "#ABB2BF",
                             NULL);

  gtk_text_buffer_create_tag(buffer, TAG_CODE, "family", "Monospace",
                             "background", "#3E4451", "foreground", "#E06C75",
                             NULL);
  gtk_text_buffer_create_tag(
      buffer, TAG_CODE_BLOCK, "family", "Monospace", "foreground", "#ABB2BF",
      "paragraph-background", "#2C313A", "left-margin", 24, "right-margin", 16,
      NULL);

  gtk_text_buffer_create_tag(buffer, MARKYD_TAG_CODE_KW_A, "family", "Monospace",
                             "foreground", config->h1_color, "weight",
                             PANGO_WEIGHT_BOLD, NULL);
  gtk_text_buffer_create_tag(buffer, MARKYD_TAG_CODE_KW_B, "family", "Monospace",
                             "foreground", config->h2_color, "weight",
                             PANGO_WEIGHT_BOLD, NULL);
  gtk_text_buffer_create_tag(buffer, MARKYD_TAG_CODE_KW_C, "family", "Monospace",
                             "foreground", config->h3_color, "weight",
                             PANGO_WEIGHT_BOLD, NULL);
  gtk_text_buffer_create_tag(buffer, MARKYD_TAG_CODE_LITERAL, "family",
                             "Monospace", "foreground", config->h3_color, NULL);
  gtk_text_buffer_create_tag(buffer, MARKYD_TAG_CODE_COMMENT, "family",
                             "Monospace", "foreground", "#7F848E", "style",
                             PANGO_STYLE_ITALIC, NULL);

  gtk_text_buffer_create_tag(buffer, TAG_QUOTE, "left-margin", 24, "style",
                             PANGO_STYLE_ITALIC, "foreground", "#5C6370",
                             "paragraph-background", "#2C313A", NULL);

  gtk_text_buffer_create_tag(buffer, TAG_LIST, "left-margin", 28, NULL);
  gtk_text_buffer_create_tag(buffer, TAG_LIST_BULLET, "foreground",
                             config->list_bullet_color, NULL);

  gtk_text_buffer_create_tag(buffer, TAG_LINK, "foreground", "#61AFEF",
                             "underline", PANGO_UNDERLINE_SINGLE, NULL);

  gtk_text_buffer_create_tag(buffer, TAG_HRULE, "foreground", "#5C6370",
                             "justification", GTK_JUSTIFY_CENTER,
                             "pixels-above-lines", 6, "pixels-below-lines", 6,
                             NULL);

  gtk_text_buffer_create_tag(buffer, TAG_TABLE, "family", "Monospace",
                             "left-margin", 20, "right-margin", 12,
                             "paragraph-background", "#2C313A", NULL);
  gtk_text_buffer_create_tag(buffer, TAG_TABLE_HEADER, "family", "Monospace",
                             "weight", PANGO_WEIGHT_BOLD, "foreground",
                             config->h1_color, NULL);
  gtk_text_buffer_create_tag(buffer, TAG_TABLE_SEPARATOR, "family", "Monospace",
                             "foreground", "#5C6370", NULL);
}

void markdown_update_accent_tags(GtkTextBuffer *buffer) {
  GtkTextTagTable *table;
  GtkTextTag *tag;

  if (!buffer || !config) {
    return;
  }

  table = gtk_text_buffer_get_tag_table(buffer);
  if (!table) {
    return;
  }

  tag = gtk_text_tag_table_lookup(table, TAG_H1);
  if (tag) {
    g_object_set(tag, "foreground", config->h1_color, NULL);
  }
  tag = gtk_text_tag_table_lookup(table, TAG_H2);
  if (tag) {
    g_object_set(tag, "foreground", config->h2_color, NULL);
  }
  tag = gtk_text_tag_table_lookup(table, TAG_H3);
  if (tag) {
    g_object_set(tag, "foreground", config->h3_color, NULL);
  }
  tag = gtk_text_tag_table_lookup(table, TAG_LIST_BULLET);
  if (tag) {
    g_object_set(tag, "foreground", config->list_bullet_color, NULL);
  }
  tag = gtk_text_tag_table_lookup(table, MARKYD_TAG_CODE_KW_A);
  if (tag) {
    g_object_set(tag, "foreground", config->h1_color, NULL);
  }
  tag = gtk_text_tag_table_lookup(table, MARKYD_TAG_CODE_KW_B);
  if (tag) {
    g_object_set(tag, "foreground", config->h2_color, NULL);
  }
  tag = gtk_text_tag_table_lookup(table, MARKYD_TAG_CODE_KW_C);
  if (tag) {
    g_object_set(tag, "foreground", config->h3_color, NULL);
  }
  tag = gtk_text_tag_table_lookup(table, MARKYD_TAG_CODE_LITERAL);
  if (tag) {
    g_object_set(tag, "foreground", config->h3_color, NULL);
  }
  tag = gtk_text_tag_table_lookup(table, TAG_TABLE_HEADER);
  if (tag) {
    g_object_set(tag, "foreground", config->h1_color, NULL);
  }
}

static gfloat align_to_xalign(MD_ALIGN align) {
  switch (align) {
  case MD_ALIGN_RIGHT:
    return 1.0f;
  case MD_ALIGN_CENTER:
    return 0.5f;
  case MD_ALIGN_LEFT:
  case MD_ALIGN_DEFAULT:
  default:
    return 0.0f;
  }
}

GtkWidget *markdown_create_table_widget(GtkTextChildAnchor *anchor) {
  SeemdTable *table;
  GtkWidget *wrapper;
  GtkWidget *grid;

  if (!anchor) {
    return NULL;
  }

  table = (SeemdTable *)g_object_get_data(G_OBJECT(anchor), TABLE_MODEL_DATA_KEY);
  if (!table || table->col_count == 0 || !table->rows || table->rows->len == 0) {
    return NULL;
  }

  wrapper = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_style_context_add_class(gtk_widget_get_style_context(wrapper),
                              "seemd-table");
  gtk_widget_set_halign(wrapper, GTK_ALIGN_START);
  gtk_widget_set_margin_top(wrapper, 6);
  gtk_widget_set_margin_bottom(wrapper, 6);
  gtk_widget_set_margin_start(wrapper, 8);
  gtk_widget_set_margin_end(wrapper, 8);

  grid = gtk_grid_new();
  gtk_style_context_add_class(gtk_widget_get_style_context(grid),
                              "seemd-table-grid");
  gtk_grid_set_row_spacing(GTK_GRID(grid), 0);
  gtk_grid_set_column_spacing(GTK_GRID(grid), 0);
  gtk_box_pack_start(GTK_BOX(wrapper), grid, TRUE, TRUE, 0);

  for (guint r = 0; r < table->rows->len; r++) {
    SeemdTableRow *row = g_ptr_array_index(table->rows, r);
    if (!row) {
      continue;
    }

    for (guint c = 0; c < table->col_count; c++) {
      GtkWidget *cell = gtk_event_box_new();
      GtkWidget *label = gtk_label_new(NULL);
      const gchar *text = "";
      gchar *markup = NULL;
      MD_ALIGN align = (c < table->aligns->len)
                           ? g_array_index(table->aligns, MD_ALIGN, c)
                           : MD_ALIGN_DEFAULT;

      if (c < row->cells->len) {
        text = g_ptr_array_index(row->cells, c);
        if (!text) {
          text = "";
        }
      }

      gtk_style_context_add_class(gtk_widget_get_style_context(cell),
                                  "seemd-table-cell");
      if (row->is_header) {
        gtk_style_context_add_class(gtk_widget_get_style_context(cell),
                                    "seemd-table-header-cell");
      }
      g_object_set_data(G_OBJECT(cell), SEEMD_TABLE_CELL_ROW_DATA,
                        GINT_TO_POINTER((gint)r));
      g_object_set_data(G_OBJECT(cell), SEEMD_TABLE_CELL_COL_DATA,
                        GINT_TO_POINTER((gint)c));
      gtk_style_context_add_class(gtk_widget_get_style_context(label),
                                  "seemd-table-label");
      gtk_widget_set_hexpand(cell, FALSE);
      gtk_widget_set_vexpand(cell, FALSE);
      gtk_container_add(GTK_CONTAINER(cell), label);

      gtk_label_set_xalign(GTK_LABEL(label), align_to_xalign(align));
      gtk_label_set_yalign(GTK_LABEL(label), 0.5f);
      gtk_label_set_line_wrap(GTK_LABEL(label), FALSE);
      gtk_label_set_selectable(GTK_LABEL(label), FALSE);
      gtk_widget_set_margin_start(label, 8);
      gtk_widget_set_margin_end(label, 8);
      gtk_widget_set_margin_top(label, row->is_header ? 6 : 5);
      gtk_widget_set_margin_bottom(label, row->is_header ? 6 : 5);

      if (row->is_header) {
        markup = g_strdup_printf("<b>%s</b>", text);
        gtk_label_set_markup(GTK_LABEL(label), markup);
        g_free(markup);
      } else {
        gtk_label_set_markup(GTK_LABEL(label), text);
      }

      gtk_grid_attach(GTK_GRID(grid), cell, (gint)c, (gint)r, 1, 1);
    }
  }

  return wrapper;
}

void markdown_apply_tags(GtkTextBuffer *buffer, const gchar *source) {
  RenderCtx ctx;
  MD_PARSER parser = {0};
  gchar *normalized_source;
  const gchar *input;
  gint rc;

  if (!buffer) {
    return;
  }

  gtk_text_buffer_set_text(buffer, "", -1);
  input = source ? source : "";
  normalized_source = normalize_markdown_source(input);

  memset(&ctx, 0, sizeof(ctx));
  ctx.buffer = buffer;
  ctx.active_tags = g_ptr_array_new();
  ctx.block_stack = g_array_new(FALSE, FALSE, sizeof(BlockState));
  ctx.span_stack = g_array_new(FALSE, FALSE, sizeof(SpanState));
  ctx.html_stack = g_ptr_array_new();
  ctx.html_pending = g_string_new(NULL);
  ctx.list_stack = g_array_new(FALSE, FALSE, sizeof(ListState));
  ctx.code_blocks = g_array_new(FALSE, FALSE, sizeof(CodeBlockRange));
  ctx.anchor_counts = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  ctx.current_code_start_offset = -1;
  ctx.current_code_language = NULL;
  ctx.heading_start_offset = 0;
  ctx.has_output = FALSE;
  ctx.trailing_newlines = 0;
  gtk_text_buffer_get_start_iter(buffer, &ctx.iter);

  parser.abi_version = 0;
  parser.flags = MD_DIALECT_GITHUB | MD_FLAG_PERMISSIVEATXHEADERS;
  parser.enter_block = on_enter_block;
  parser.leave_block = on_leave_block;
  parser.enter_span = on_enter_span;
  parser.leave_span = on_leave_span;
  parser.text = on_text;
  parser.debug_log = NULL;
  parser.syntax = NULL;

  rc = md_parse(normalized_source, (MD_SIZE)strlen(normalized_source), &parser,
                &ctx);
  if (rc != 0) {
    gtk_text_buffer_set_text(buffer, input, -1);
  } else {
    html_flush(&ctx);
    html_close_all(&ctx);
    apply_code_highlighting(buffer, ctx.code_blocks);
  }

  if (ctx.table_cell_text) {
    g_string_free(ctx.table_cell_text, TRUE);
  }
  g_free(ctx.image_src);
  if (ctx.image_alt) {
    g_string_free(ctx.image_alt, TRUE);
  }
  if (ctx.table_model) {
    seemd_table_free(ctx.table_model);
  }
  if (ctx.heading_text) {
    g_string_free(ctx.heading_text, TRUE);
  }
  g_hash_table_destroy(ctx.anchor_counts);
  g_array_free(ctx.list_stack, TRUE);
  g_array_free(ctx.code_blocks, TRUE);
  if (ctx.html_pending) {
    g_string_free(ctx.html_pending, TRUE);
  }
  if (ctx.html_stack) {
    for (guint i = 0; i < ctx.html_stack->len; i++) {
      html_element_free(g_ptr_array_index(ctx.html_stack, i));
    }
    g_ptr_array_free(ctx.html_stack, TRUE);
  }
  g_array_free(ctx.span_stack, TRUE);
  g_array_free(ctx.block_stack, TRUE);
  g_ptr_array_free(ctx.active_tags, TRUE);
  g_free(normalized_source);
}

typedef struct {
  gboolean ordered;
  gboolean tight;
} GithubHtmlListState;

typedef struct {
  MD_BLOCKTYPE type;
  gboolean emitted;
  guint heading_level;
} GithubHtmlBlockState;

typedef struct {
  GString *out;
  GString *heading_html;
  GString *heading_text;
  GHashTable *anchor_counts;
  GArray *block_stack; /* GithubHtmlBlockState */
  GArray *list_stack;  /* GithubHtmlListState */
  gboolean in_image;
  gchar *image_src;
  gchar *image_title;
  GString *image_alt;
  gboolean in_code_block;
  GString *code_text;
  const MarkydLanguageHighlight *code_language;
} GithubHtmlCtx;

static void github_html_append_highlighted_code(
    GString *target, const gchar *code,
    const MarkydLanguageHighlight *language);

static GString *github_html_target(GithubHtmlCtx *ctx) {
  return (ctx && ctx->heading_html) ? ctx->heading_html : (ctx ? ctx->out : NULL);
}

static void github_html_append(GithubHtmlCtx *ctx, const gchar *text) {
  GString *target = github_html_target(ctx);
  if (target && text) {
    g_string_append(target, text);
  }
}

static void github_html_append_printf(GithubHtmlCtx *ctx, const gchar *format,
                                      ...) {
  GString *target = github_html_target(ctx);
  va_list args;

  if (!target || !format) {
    return;
  }

  va_start(args, format);
  g_string_append_vprintf(target, format, args);
  va_end(args);
}

static void github_html_append_escaped_len(GString *out, const gchar *text,
                                           gsize len) {
  gchar *escaped;

  if (!out || !text || len == 0) {
    return;
  }

  escaped = g_markup_escape_text(text, (gssize)len);
  g_string_append(out, escaped);
  g_free(escaped);
}

static gchar *github_html_escape_attr_string(const gchar *text) {
  return g_markup_escape_text(text ? text : "", -1);
}

static gchar *github_html_attr_to_escaped(const MD_ATTRIBUTE *attr) {
  GString *out;

  if (!attr || !attr->text || attr->size == 0) {
    return g_strdup("");
  }

  out = g_string_sized_new(attr->size);
  if (!attr->substr_types || !attr->substr_offsets) {
    github_html_append_escaped_len(out, attr->text, attr->size);
    return g_string_free(out, FALSE);
  }

  for (guint i = 0; attr->substr_offsets[i] < attr->size; i++) {
    MD_OFFSET start = attr->substr_offsets[i];
    MD_OFFSET end = attr->substr_offsets[i + 1];
    MD_TEXTTYPE type = attr->substr_types[i];

    if (end <= start || end > attr->size) {
      continue;
    }

    if (type == MD_TEXT_ENTITY) {
      g_string_append_len(out, attr->text + start, end - start);
    } else if (type == MD_TEXT_NULLCHAR) {
      g_string_append(out, "\xEF\xBF\xBD");
    } else {
      github_html_append_escaped_len(out, attr->text + start, end - start);
    }
  }

  return g_string_free(out, FALSE);
}

static void github_html_capture_heading_text(GithubHtmlCtx *ctx,
                                             const gchar *text) {
  if (!ctx || !ctx->heading_text || !text) {
    return;
  }

  for (const gchar *p = text; *p != '\0'; p++) {
    g_string_append_c(ctx->heading_text,
                      (*p == '\n' || *p == '\r') ? ' ' : *p);
  }
}

static gchar *github_html_safe_language_class(const gchar *language) {
  GString *out;

  if (!language || language[0] == '\0') {
    return NULL;
  }

  out = g_string_new(NULL);
  for (const gchar *p = language; *p != '\0'; p++) {
    if (g_ascii_isalnum(*p) || *p == '-' || *p == '_') {
      g_string_append_c(out, *p);
    } else if (out->len > 0 && out->str[out->len - 1] != '-') {
      g_string_append_c(out, '-');
    }
  }

  while (out->len > 0 && out->str[out->len - 1] == '-') {
    g_string_truncate(out, out->len - 1);
  }

  if (out->len == 0) {
    g_string_free(out, TRUE);
    return NULL;
  }

  return g_string_free(out, FALSE);
}

static gboolean github_html_in_tight_list(GithubHtmlCtx *ctx) {
  GithubHtmlListState *state;

  if (!ctx || !ctx->list_stack || ctx->list_stack->len == 0) {
    return FALSE;
  }

  state = &g_array_index(ctx->list_stack, GithubHtmlListState,
                         ctx->list_stack->len - 1);
  return state->tight;
}

static const gchar *github_html_align_attr(MD_ALIGN align) {
  switch (align) {
  case MD_ALIGN_LEFT:
    return " align=\"left\"";
  case MD_ALIGN_CENTER:
    return " align=\"center\"";
  case MD_ALIGN_RIGHT:
    return " align=\"right\"";
  case MD_ALIGN_DEFAULT:
  default:
    return "";
  }
}

static void github_html_start_heading(GithubHtmlCtx *ctx, guint level) {
  if (!ctx) {
    return;
  }

  if (ctx->heading_html) {
    g_string_free(ctx->heading_html, TRUE);
  }
  if (ctx->heading_text) {
    g_string_free(ctx->heading_text, TRUE);
  }

  ctx->heading_html = g_string_new(NULL);
  ctx->heading_text = g_string_new(NULL);
  (void)level;
}

static void github_html_finish_heading(GithubHtmlCtx *ctx, guint level) {
  gchar *base;
  gchar *slug = NULL;
  guint count;
  gchar *id_attr = NULL;
  gchar *href_attr = NULL;

  if (!ctx || !ctx->heading_html || !ctx->heading_text) {
    return;
  }

  base = markdown_normalize_anchor_slug(ctx->heading_text->str);
  if (base && base[0] != '\0') {
    count = GPOINTER_TO_UINT(g_hash_table_lookup(ctx->anchor_counts, base));
    slug = (count == 0) ? g_strdup(base) : g_strdup_printf("%s-%u", base, count);
    g_hash_table_replace(ctx->anchor_counts, g_strdup(base),
                         GUINT_TO_POINTER(count + 1));
    id_attr = github_html_escape_attr_string(slug);
    href_attr = g_strdup_printf("#%s", id_attr);
  }

  level = CLAMP(level, 1, 6);
  if (id_attr) {
    g_string_append_printf(
        ctx->out,
        "<h%u id=\"%s\"><a class=\"anchor\" aria-hidden=\"true\" "
        "href=\"%s\"></a>%s</h%u>\n",
        level, id_attr, href_attr, ctx->heading_html->str, level);
  } else {
    g_string_append_printf(ctx->out, "<h%u>%s</h%u>\n", level,
                           ctx->heading_html->str, level);
  }

  g_free(href_attr);
  g_free(id_attr);
  g_free(slug);
  g_free(base);
  g_string_free(ctx->heading_html, TRUE);
  g_string_free(ctx->heading_text, TRUE);
  ctx->heading_html = NULL;
  ctx->heading_text = NULL;
}

static int github_html_enter_block(MD_BLOCKTYPE type, void *detail,
                                   void *userdata) {
  GithubHtmlCtx *ctx = (GithubHtmlCtx *)userdata;
  GithubHtmlBlockState state = {type, TRUE, 0};

  switch (type) {
  case MD_BLOCK_DOC:
    state.emitted = FALSE;
    break;

  case MD_BLOCK_QUOTE:
    github_html_append(ctx, "<blockquote>\n");
    break;

  case MD_BLOCK_UL: {
    MD_BLOCK_UL_DETAIL *ul = (MD_BLOCK_UL_DETAIL *)detail;
    GithubHtmlListState list = {FALSE, ul ? ul->is_tight != 0 : FALSE};
    g_array_append_val(ctx->list_stack, list);
    github_html_append(ctx, "<ul>\n");
    break;
  }

  case MD_BLOCK_OL: {
    MD_BLOCK_OL_DETAIL *ol = (MD_BLOCK_OL_DETAIL *)detail;
    GithubHtmlListState list = {TRUE, ol ? ol->is_tight != 0 : FALSE};
    g_array_append_val(ctx->list_stack, list);
    if (ol && ol->start > 1) {
      github_html_append_printf(ctx, "<ol start=\"%u\">\n", ol->start);
    } else {
      github_html_append(ctx, "<ol>\n");
    }
    break;
  }

  case MD_BLOCK_LI: {
    MD_BLOCK_LI_DETAIL *li = (MD_BLOCK_LI_DETAIL *)detail;
    gboolean task = li && li->is_task;
    github_html_append(ctx, task ? "<li class=\"task-list-item\">" : "<li>");
    if (task) {
      github_html_append(
          ctx,
          (li->task_mark == 'x' || li->task_mark == 'X')
              ? "<input type=\"checkbox\" class=\"task-list-item-checkbox\" "
                "checked disabled> "
              : "<input type=\"checkbox\" class=\"task-list-item-checkbox\" "
                "disabled> ");
    }
    break;
  }

  case MD_BLOCK_HR:
    github_html_append(ctx, "<hr>\n");
    state.emitted = FALSE;
    break;

  case MD_BLOCK_H: {
    MD_BLOCK_H_DETAIL *h = (MD_BLOCK_H_DETAIL *)detail;
    state.heading_level = h ? h->level : 1;
    state.emitted = FALSE;
    github_html_start_heading(ctx, state.heading_level);
    break;
  }

  case MD_BLOCK_CODE: {
    gchar *language =
        extract_code_language_from_detail((MD_BLOCK_CODE_DETAIL *)detail);
    gchar *class_name = github_html_safe_language_class(language);
    gchar *class_attr = class_name ? github_html_escape_attr_string(class_name)
                                   : NULL;
    if (class_attr) {
      github_html_append_printf(ctx, "<pre><code class=\"language-%s\">",
                                class_attr);
    } else {
      github_html_append(ctx, "<pre><code>");
    }
    ctx->in_code_block = TRUE;
    ctx->code_language = markyd_code_lookup_language(language);
    if (ctx->code_text) {
      g_string_set_size(ctx->code_text, 0);
    } else {
      ctx->code_text = g_string_new(NULL);
    }
    g_free(class_attr);
    g_free(class_name);
    g_free(language);
    break;
  }

  case MD_BLOCK_HTML:
    state.emitted = FALSE;
    break;

  case MD_BLOCK_P:
    if (github_html_in_tight_list(ctx)) {
      state.emitted = FALSE;
    } else {
      github_html_append(ctx, "<p>");
    }
    break;

  case MD_BLOCK_TABLE:
    github_html_append(ctx, "<table>\n");
    break;

  case MD_BLOCK_THEAD:
    github_html_append(ctx, "<thead>\n");
    break;

  case MD_BLOCK_TBODY:
    github_html_append(ctx, "<tbody>\n");
    break;

  case MD_BLOCK_TR:
    github_html_append(ctx, "<tr>\n");
    break;

  case MD_BLOCK_TH: {
    MD_BLOCK_TD_DETAIL *td = (MD_BLOCK_TD_DETAIL *)detail;
    github_html_append_printf(ctx, "<th%s>", github_html_align_attr(
                                                td ? td->align
                                                   : MD_ALIGN_DEFAULT));
    break;
  }

  case MD_BLOCK_TD: {
    MD_BLOCK_TD_DETAIL *td = (MD_BLOCK_TD_DETAIL *)detail;
    github_html_append_printf(ctx, "<td%s>", github_html_align_attr(
                                                td ? td->align
                                                   : MD_ALIGN_DEFAULT));
    break;
  }
  }

  g_array_append_val(ctx->block_stack, state);
  return 0;
}

static int github_html_leave_block(MD_BLOCKTYPE type, void *detail,
                                   void *userdata) {
  GithubHtmlCtx *ctx = (GithubHtmlCtx *)userdata;
  GithubHtmlBlockState state = {type, TRUE, 0};
  (void)detail;

  if (ctx->block_stack->len > 0) {
    state = g_array_index(ctx->block_stack, GithubHtmlBlockState,
                          ctx->block_stack->len - 1);
    g_array_set_size(ctx->block_stack, ctx->block_stack->len - 1);
  }

  switch (type) {
  case MD_BLOCK_DOC:
    break;

  case MD_BLOCK_QUOTE:
    github_html_append(ctx, "</blockquote>\n");
    break;

  case MD_BLOCK_UL:
    github_html_append(ctx, "</ul>\n");
    if (ctx->list_stack->len > 0) {
      g_array_set_size(ctx->list_stack, ctx->list_stack->len - 1);
    }
    break;

  case MD_BLOCK_OL:
    github_html_append(ctx, "</ol>\n");
    if (ctx->list_stack->len > 0) {
      g_array_set_size(ctx->list_stack, ctx->list_stack->len - 1);
    }
    break;

  case MD_BLOCK_LI:
    github_html_append(ctx, "</li>\n");
    break;

  case MD_BLOCK_HR:
    break;

  case MD_BLOCK_H:
    github_html_finish_heading(ctx, state.heading_level);
    break;

  case MD_BLOCK_CODE:
    if (ctx->in_code_block) {
      github_html_append_highlighted_code(
          github_html_target(ctx),
          ctx->code_text ? ctx->code_text->str : "", ctx->code_language);
      ctx->in_code_block = FALSE;
      ctx->code_language = NULL;
      if (ctx->code_text) {
        g_string_set_size(ctx->code_text, 0);
      }
    }
    github_html_append(ctx, "</code></pre>\n");
    break;

  case MD_BLOCK_HTML:
    break;

  case MD_BLOCK_P:
    if (state.emitted) {
      github_html_append(ctx, "</p>\n");
    }
    break;

  case MD_BLOCK_TABLE:
    github_html_append(ctx, "</table>\n");
    break;

  case MD_BLOCK_THEAD:
    github_html_append(ctx, "</thead>\n");
    break;

  case MD_BLOCK_TBODY:
    github_html_append(ctx, "</tbody>\n");
    break;

  case MD_BLOCK_TR:
    github_html_append(ctx, "</tr>\n");
    break;

  case MD_BLOCK_TH:
    github_html_append(ctx, "</th>\n");
    break;

  case MD_BLOCK_TD:
    github_html_append(ctx, "</td>\n");
    break;
  }

  return 0;
}

static int github_html_enter_span(MD_SPANTYPE type, void *detail,
                                  void *userdata) {
  GithubHtmlCtx *ctx = (GithubHtmlCtx *)userdata;

  if (ctx->in_image && type != MD_SPAN_IMG) {
    return 0;
  }

  switch (type) {
  case MD_SPAN_EM:
    github_html_append(ctx, "<em>");
    break;

  case MD_SPAN_STRONG:
    github_html_append(ctx, "<strong>");
    break;

  case MD_SPAN_A: {
    MD_SPAN_A_DETAIL *a = (MD_SPAN_A_DETAIL *)detail;
    gchar *href = github_html_attr_to_escaped(a ? &a->href : NULL);
    gchar *title = github_html_attr_to_escaped(a ? &a->title : NULL);
    github_html_append_printf(ctx, "<a href=\"%s\"", href);
    if (title && title[0] != '\0') {
      github_html_append_printf(ctx, " title=\"%s\"", title);
    }
    github_html_append(ctx, ">");
    g_free(title);
    g_free(href);
    break;
  }

  case MD_SPAN_IMG: {
    MD_SPAN_IMG_DETAIL *img = (MD_SPAN_IMG_DETAIL *)detail;
    g_free(ctx->image_src);
    g_free(ctx->image_title);
    ctx->image_src = github_html_attr_to_escaped(img ? &img->src : NULL);
    ctx->image_title = github_html_attr_to_escaped(img ? &img->title : NULL);
    if (!ctx->image_alt) {
      ctx->image_alt = g_string_new(NULL);
    } else {
      g_string_set_size(ctx->image_alt, 0);
    }
    ctx->in_image = TRUE;
    break;
  }

  case MD_SPAN_CODE:
    github_html_append(ctx, "<code>");
    break;

  case MD_SPAN_DEL:
    github_html_append(ctx, "<del>");
    break;

  case MD_SPAN_U:
    github_html_append(ctx, "<u>");
    break;

  case MD_SPAN_LATEXMATH:
  case MD_SPAN_LATEXMATH_DISPLAY:
    github_html_append(ctx, "<code>");
    break;

  case MD_SPAN_WIKILINK:
    break;
  }

  return 0;
}

static int github_html_leave_span(MD_SPANTYPE type, void *detail,
                                  void *userdata) {
  GithubHtmlCtx *ctx = (GithubHtmlCtx *)userdata;
  (void)detail;

  if (type == MD_SPAN_IMG && ctx->in_image) {
    gchar *alt = github_html_escape_attr_string(
        ctx->image_alt ? ctx->image_alt->str : "");
    github_html_append_printf(ctx, "<img src=\"%s\" alt=\"%s\"",
                              ctx->image_src ? ctx->image_src : "", alt);
    if (ctx->image_title && ctx->image_title[0] != '\0') {
      github_html_append_printf(ctx, " title=\"%s\"", ctx->image_title);
    }
    github_html_append(ctx, ">");
    g_free(alt);
    g_free(ctx->image_src);
    g_free(ctx->image_title);
    ctx->image_src = NULL;
    ctx->image_title = NULL;
    if (ctx->image_alt) {
      g_string_set_size(ctx->image_alt, 0);
    }
    ctx->in_image = FALSE;
    return 0;
  }

  if (ctx->in_image) {
    return 0;
  }

  switch (type) {
  case MD_SPAN_EM:
    github_html_append(ctx, "</em>");
    break;

  case MD_SPAN_STRONG:
    github_html_append(ctx, "</strong>");
    break;

  case MD_SPAN_A:
    github_html_append(ctx, "</a>");
    break;

  case MD_SPAN_IMG:
    break;

  case MD_SPAN_CODE:
    github_html_append(ctx, "</code>");
    break;

  case MD_SPAN_DEL:
    github_html_append(ctx, "</del>");
    break;

  case MD_SPAN_U:
    github_html_append(ctx, "</u>");
    break;

  case MD_SPAN_LATEXMATH:
  case MD_SPAN_LATEXMATH_DISPLAY:
    github_html_append(ctx, "</code>");
    break;

  case MD_SPAN_WIKILINK:
    break;
  }

  return 0;
}

/* Map an internal highlight tag name to a GitHub Primer CSS class. */
static const gchar *github_html_code_token_class(const gchar *tag) {
  if (!tag) {
    return NULL;
  }
  if (strcmp(tag, MARKYD_TAG_CODE_KW_A) == 0 ||
      strcmp(tag, MARKYD_TAG_CODE_KW_B) == 0) {
    return "pl-k";
  }
  if (strcmp(tag, MARKYD_TAG_CODE_KW_C) == 0) {
    return "pl-c1";
  }
  if (strcmp(tag, MARKYD_TAG_CODE_LITERAL) == 0) {
    return "pl-s";
  }
  if (strcmp(tag, MARKYD_TAG_CODE_COMMENT) == 0) {
    return "pl-c";
  }
  return NULL;
}

typedef struct {
  GString *target;
  const gchar *line;
  gint cursor_char;
} GithubHtmlCodeLineCtx;

/* Token callback: escape the gap before the token, then wrap the token in a
 * span. Ranges are non-overlapping and delivered left-to-right. */
static void github_html_code_token(gint start_char_offset, gint end_char_offset,
                                   const gchar *tag_name, gpointer user_data) {
  GithubHtmlCodeLineCtx *lc = (GithubHtmlCodeLineCtx *)user_data;
  const gchar *cur;
  const gchar *sp;
  const gchar *ep;
  const gchar *cls;

  if (!lc || !lc->target || start_char_offset < lc->cursor_char ||
      end_char_offset <= start_char_offset) {
    return;
  }

  cur = g_utf8_offset_to_pointer(lc->line, lc->cursor_char);
  sp = g_utf8_offset_to_pointer(lc->line, start_char_offset);
  ep = g_utf8_offset_to_pointer(lc->line, end_char_offset);

  if (sp > cur) {
    github_html_append_escaped_len(lc->target, cur, (gsize)(sp - cur));
  }

  cls = github_html_code_token_class(tag_name);
  if (cls) {
    g_string_append_printf(lc->target, "<span class=\"%s\">", cls);
  }
  github_html_append_escaped_len(lc->target, sp, (gsize)(ep - sp));
  if (cls) {
    g_string_append(lc->target, "</span>");
  }

  lc->cursor_char = end_char_offset;
}

/* Emit code-block contents into target, syntax-highlighted when the language
 * is recognized. Falls back to plain escaped text otherwise. */
static void github_html_append_highlighted_code(
    GString *target, const gchar *code,
    const MarkydLanguageHighlight *language) {
  MarkydCodeScanState state;
  const gchar *line_start;

  if (!target || !code) {
    return;
  }
  if (!language) {
    github_html_append_escaped_len(target, code, strlen(code));
    return;
  }

  markyd_code_scan_state_reset(&state);
  line_start = code;
  for (;;) {
    const gchar *nl = strchr(line_start, '\n');
    gsize line_len = nl ? (gsize)(nl - line_start) : strlen(line_start);
    gchar *line = g_strndup(line_start, line_len);
    GithubHtmlCodeLineCtx lc = {target, line, 0};
    const gchar *tail;

    markyd_code_scan_line(language, line, &state, github_html_code_token, &lc);
    tail = g_utf8_offset_to_pointer(line, lc.cursor_char);
    github_html_append_escaped_len(target, tail, strlen(tail));
    g_free(line);

    if (!nl) {
      break;
    }
    g_string_append_c(target, '\n');
    line_start = nl + 1;
  }
}

static void github_html_append_text(GithubHtmlCtx *ctx, MD_TEXTTYPE type,
                                    const MD_CHAR *text, MD_SIZE size) {
  GString *target;
  gchar *plain;

  if (!ctx) {
    return;
  }

  if (ctx->in_code_block && ctx->code_text) {
    if (type == MD_TEXT_BR || type == MD_TEXT_SOFTBR) {
      g_string_append_c(ctx->code_text, '\n');
    } else {
      gchar *code_plain = md_text_to_utf8(type, text, size);
      g_string_append(ctx->code_text, code_plain);
      g_free(code_plain);
    }
    return;
  }

  if (type == MD_TEXT_BR) {
    if (ctx->in_image) {
      g_string_append_c(ctx->image_alt, '\n');
    } else {
      github_html_append(ctx, "<br>\n");
      github_html_capture_heading_text(ctx, "\n");
    }
    return;
  }

  if (type == MD_TEXT_SOFTBR) {
    if (ctx->in_image) {
      g_string_append_c(ctx->image_alt, '\n');
    } else {
      github_html_append(ctx, "\n");
      github_html_capture_heading_text(ctx, "\n");
    }
    return;
  }

  plain = md_text_to_utf8(type, text, size);
  if (ctx->in_image) {
    g_string_append(ctx->image_alt, plain);
    g_free(plain);
    return;
  }

  target = github_html_target(ctx);
  if (!target) {
    g_free(plain);
    return;
  }

  switch (type) {
  case MD_TEXT_ENTITY:
    g_string_append_len(target, text, size);
    github_html_capture_heading_text(ctx, plain);
    break;

  case MD_TEXT_HTML:
    g_string_append_len(target, text, size);
    break;

  case MD_TEXT_NULLCHAR:
  case MD_TEXT_NORMAL:
  case MD_TEXT_CODE:
  case MD_TEXT_LATEXMATH:
  default:
    github_html_append_escaped_len(target, plain, strlen(plain));
    github_html_capture_heading_text(ctx, plain);
    break;
  }

  g_free(plain);
}

static int github_html_text(MD_TEXTTYPE type, const MD_CHAR *text, MD_SIZE size,
                            void *userdata) {
  github_html_append_text((GithubHtmlCtx *)userdata, type, text, size);
  return 0;
}

static gchar *github_markdown_css(gboolean dark) {
  GString *css = g_string_new(NULL);
  const gchar *page_bg = dark ? "#0d1117" : "#ffffff";
  const gchar *fg = dark ? "#e6edf3" : "#1f2328";
  const gchar *muted = dark ? "#8b949e" : "#59636e";
  const gchar *border = dark ? "#30363d" : "#d0d7de";
  const gchar *border_muted = dark ? "#21262d" : "#d8dee4";
  const gchar *link = dark ? "#4493f8" : "#0969da";
  const gchar *code_bg = dark ? "rgba(110,118,129,0.4)"
                              : "rgba(175,184,193,0.2)";
  const gchar *pre_bg = dark ? "#161b22" : "#f6f8fa";
  const gchar *table_alt = dark ? "#161b22" : "#f6f8fa";
  const gchar *kbd_bg = dark ? "#161b22" : "#f6f8fa";
  const gchar *kbd_shadow = dark ? "#6e7681" : "#afb8c1";
  const gchar *hl_kw = dark ? "#ff7b72" : "#cf222e";
  const gchar *hl_const = dark ? "#79c0ff" : "#0550ae";
  const gchar *hl_str = dark ? "#a5d6ff" : "#0a3069";
  const gchar *hl_com = dark ? "#8b949e" : "#6e7781";

  g_string_append_printf(css,
                         "html,body{margin:0;background:%s;color:%s;}\n",
                         page_bg, fg);
  g_string_append(css,
                  "*{box-sizing:border-box;}\n"
                  ".markdown-body{min-width:200px;max-width:980px;margin:0 "
                  "auto;padding:32px;font-family:-apple-system,BlinkMacSystemFont,"
                  "\"Segoe UI\",\"Noto Sans\",Helvetica,Arial,sans-serif,"
                  "\"Apple Color Emoji\",\"Segoe UI Emoji\";font-size:16px;"
                  "line-height:1.5;word-wrap:break-word;}\n"
                  "@media (max-width:767px){.markdown-body{padding:16px;}}\n"
                  ".markdown-body:before{display:table;content:\"\";}"
                  ".markdown-body:after{display:table;clear:both;content:\"\";}\n");
  g_string_append_printf(css,
                         ".markdown-body a{color:%s;text-decoration:none;}"
                         ".markdown-body a:hover{text-decoration:underline;}\n",
                         link);
  g_string_append(css,
                  ".markdown-body p,.markdown-body blockquote,.markdown-body ul,"
                  ".markdown-body ol,.markdown-body dl,.markdown-body table,"
                  ".markdown-body pre,.markdown-body details{margin-top:0;"
                  "margin-bottom:16px;}\n"
                  ".markdown-body blockquote>:first-child,.markdown-body li>:first-child{margin-top:0;}"
                  ".markdown-body blockquote>:last-child,.markdown-body li>:last-child{margin-bottom:0;}\n"
                  ".markdown-body h1,.markdown-body h2,.markdown-body h3,"
                  ".markdown-body h4,.markdown-body h5,.markdown-body h6{margin-top:24px;"
                  "margin-bottom:16px;font-weight:600;line-height:1.25;position:relative;}\n"
                  ".markdown-body h1{font-size:2em;}.markdown-body h2{font-size:1.5em;}"
                  ".markdown-body h3{font-size:1.25em;}.markdown-body h4{font-size:1em;}"
                  ".markdown-body h5{font-size:.875em;}.markdown-body h6{font-size:.85em;}\n"
                  ".markdown-body h1:first-child,.markdown-body h2:first-child,"
                  ".markdown-body h3:first-child{margin-top:0;}\n");
  g_string_append_printf(css,
                         ".markdown-body h1,.markdown-body h2{padding-bottom:.3em;"
                         "border-bottom:1px solid %s;}.markdown-body h6{color:%s;}\n",
                         border_muted, muted);
  g_string_append_printf(css,
                         ".markdown-body .anchor{float:left;padding-right:4px;"
                         "margin-left:-20px;line-height:1;}.markdown-body .anchor:before{content:\"#\";"
                         "color:%s;visibility:hidden;}.markdown-body h1:hover .anchor:before,"
                         ".markdown-body h2:hover .anchor:before,.markdown-body h3:hover .anchor:before,"
                         ".markdown-body h4:hover .anchor:before,.markdown-body h5:hover .anchor:before,"
                         ".markdown-body h6:hover .anchor:before{visibility:visible;}\n",
                         muted);
  g_string_append_printf(css,
                         ".markdown-body blockquote{padding:0 1em;color:%s;"
                         "border-left:.25em solid %s;}\n",
                         muted, border);
  g_string_append(css,
                  ".markdown-body ul,.markdown-body ol{padding-left:2em;}"
                  ".markdown-body ol ol,.markdown-body ul ol{list-style-type:lower-roman;}"
                  ".markdown-body ul ul ol,.markdown-body ul ol ol,"
                  ".markdown-body ol ul ol,.markdown-body ol ol ol{list-style-type:lower-alpha;}"
                  ".markdown-body li+li{margin-top:.25em;}.markdown-body dl{padding:0;}"
                  ".markdown-body dl dt{padding:0;margin-top:16px;font-size:1em;font-style:italic;"
                  "font-weight:600;}.markdown-body dl dd{padding:0 16px;margin-bottom:16px;}\n");
  g_string_append_printf(css,
                         ".markdown-body hr{height:.25em;padding:0;margin:24px 0;"
                         "background-color:%s;border:0;}\n",
                         border_muted);
  g_string_append_printf(css,
                         ".markdown-body table{display:block;width:max-content;max-width:100%%;"
                         "overflow:auto;border-spacing:0;border-collapse:collapse;}"
                         ".markdown-body table th{font-weight:600;}.markdown-body table th,"
                         ".markdown-body table td{padding:6px 13px;border:1px solid %s;}"
                         ".markdown-body table tr{background-color:%s;border-top:1px solid %s;}"
                         ".markdown-body table tr:nth-child(2n){background-color:%s;}\n",
                         border, page_bg, border_muted, table_alt);
  g_string_append_printf(css,
                         ".markdown-body img{max-width:100%%;box-sizing:content-box;background-color:%s;}"
                         ".markdown-body img[align=right]{padding-left:20px;}"
                         ".markdown-body img[align=left]{padding-right:20px;}\n",
                         page_bg);
  g_string_append_printf(css,
                         ".markdown-body code,.markdown-body tt{padding:.2em .4em;margin:0;"
                         "font-size:85%%;white-space:break-spaces;background-color:%s;"
                         "border-radius:6px;}.markdown-body pre{padding:16px;overflow:auto;"
                         "font-size:85%%;line-height:1.45;background-color:%s;border-radius:6px;}"
                         ".markdown-body pre code,.markdown-body pre tt{display:inline;max-width:auto;"
                         "padding:0;margin:0;overflow:visible;line-height:inherit;word-wrap:normal;"
                         "background-color:transparent;border:0;}\n",
                         code_bg, pre_bg);
  g_string_append_printf(css,
                         ".markdown-body pre .pl-k{color:%s;}"
                         ".markdown-body pre .pl-c1{color:%s;}"
                         ".markdown-body pre .pl-s{color:%s;}"
                         ".markdown-body pre .pl-c{color:%s;}\n",
                         hl_kw, hl_const, hl_str, hl_com);
  g_string_append_printf(css,
                         ".markdown-body kbd{display:inline-block;padding:3px 5px;font:11px ui-monospace,"
                         "SFMono-Regular,SF Mono,Consolas,Liberation Mono,Menlo,monospace;"
                         "line-height:10px;color:%s;vertical-align:middle;background-color:%s;"
                         "border:solid 1px %s;border-bottom-color:%s;border-radius:6px;"
                         "box-shadow:inset 0 -1px 0 %s;}\n",
                         fg, kbd_bg, border, kbd_shadow, kbd_shadow);
  g_string_append_printf(css,
                         ".markdown-body input[type=checkbox]{box-sizing:border-box;padding:0;"
                         "margin:0 .2em .25em -1.4em;vertical-align:middle;}"
                         ".markdown-body .task-list-item{list-style-type:none;}"
                         ".markdown-body .task-list-item+.task-list-item{margin-top:3px;}"
                         ".markdown-body .task-list-item-checkbox{margin-right:.25em;}\n"
                         ".markdown-body sup>a:before{content:\"[\";}.markdown-body sup>a:after{content:\"]\";}"
                         ".markdown-body strong{font-weight:600;}.markdown-body small{font-size:90%%;}"
                         ".markdown-body sub,.markdown-body sup{font-size:75%%;line-height:0;position:relative;"
                         "vertical-align:baseline;}.markdown-body sup{top:-.5em;}.markdown-body sub{bottom:-.25em;}"
                         ".markdown-body mark{background-color:%s;color:%s;}\n",
                         dark ? "#9a6700" : "#fff8c5", fg);

  return g_string_free(css, FALSE);
}

static gchar *github_html_render_body(const gchar *source) {
  GithubHtmlCtx ctx;
  MD_PARSER parser = {0};
  gchar *normalized_source;
  gint rc;

  memset(&ctx, 0, sizeof(ctx));
  ctx.out = g_string_new(NULL);
  ctx.anchor_counts = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  ctx.block_stack = g_array_new(FALSE, FALSE, sizeof(GithubHtmlBlockState));
  ctx.list_stack = g_array_new(FALSE, FALSE, sizeof(GithubHtmlListState));
  ctx.image_alt = g_string_new(NULL);
  ctx.code_text = g_string_new(NULL);

  normalized_source = normalize_markdown_source(source ? source : "");

  parser.abi_version = 0;
  parser.flags = MD_DIALECT_GITHUB | MD_FLAG_PERMISSIVEATXHEADERS;
  parser.enter_block = github_html_enter_block;
  parser.leave_block = github_html_leave_block;
  parser.enter_span = github_html_enter_span;
  parser.leave_span = github_html_leave_span;
  parser.text = github_html_text;
  parser.debug_log = NULL;
  parser.syntax = NULL;

  rc = md_parse(normalized_source, (MD_SIZE)strlen(normalized_source), &parser,
                &ctx);
  if (rc != 0) {
    g_string_set_size(ctx.out, 0);
    g_string_append(ctx.out, "<pre>");
    github_html_append_escaped_len(ctx.out, source ? source : "",
                                   strlen(source ? source : ""));
    g_string_append(ctx.out, "</pre>\n");
  }

  g_free(normalized_source);
  if (ctx.heading_html) {
    g_string_free(ctx.heading_html, TRUE);
  }
  if (ctx.heading_text) {
    g_string_free(ctx.heading_text, TRUE);
  }
  g_free(ctx.image_src);
  g_free(ctx.image_title);
  if (ctx.image_alt) {
    g_string_free(ctx.image_alt, TRUE);
  }
  if (ctx.code_text) {
    g_string_free(ctx.code_text, TRUE);
  }
  g_array_free(ctx.list_stack, TRUE);
  g_array_free(ctx.block_stack, TRUE);
  g_hash_table_destroy(ctx.anchor_counts);

  return g_string_free(ctx.out, FALSE);
}

gchar *markdown_render_github_html(const gchar *source, const gchar *base_uri) {
  gboolean dark = config && g_strcmp0(config->theme, "dark") == 0;
  gchar *body = github_html_render_body(source ? source : "");
  gchar *css = github_markdown_css(dark);
  GString *doc = g_string_new(NULL);
  (void)base_uri;

  g_string_append(doc, "<!doctype html>\n<html");
  g_string_append_printf(doc, " data-color-mode=\"%s\">", dark ? "dark" : "light");
  g_string_append(doc, "\n<head>\n<meta charset=\"utf-8\">\n");
  g_string_append(doc,
                  "<meta name=\"viewport\" content=\"width=device-width, "
                  "initial-scale=1\">\n");
  g_string_append_printf(doc, "<meta name=\"color-scheme\" content=\"%s\">\n",
                         dark ? "dark" : "light");
  g_string_append(doc, "<style>\n");
  g_string_append(doc, css);
  g_string_append(doc, "</style>\n</head>\n<body>\n");
  g_string_append(doc, "<article class=\"markdown-body\">\n");
  g_string_append(doc, body);
  g_string_append(doc, "\n</article>\n</body>\n</html>\n");

  g_free(css);
  g_free(body);
  return g_string_free(doc, FALSE);
}
