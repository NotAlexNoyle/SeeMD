#include "editor.h"
#include "app.h"
#include "markdown.h"
#include <string.h>
#include <webkit2/webkit2.h>

static gboolean on_button_release(GtkWidget *widget, GdkEventButton *event,
                                  gpointer user_data);
static gboolean on_motion_notify(GtkWidget *widget, GdkEventMotion *event,
                                 gpointer user_data);
static gboolean on_leave_notify(GtkWidget *widget, GdkEventCrossing *event,
                                gpointer user_data);
static void apply_markdown(MarkydEditor *self);
static void schedule_markdown_apply(MarkydEditor *self);
static void render_image_widgets(MarkydEditor *self);
static void render_table_widgets(MarkydEditor *self);
static void refresh_image_widget_scales(MarkydEditor *self);
static gboolean resolve_image_source_path(MarkydEditor *self, const gchar *src,
                                          gchar **out_path);
static void on_text_view_size_allocate(GtkWidget *widget, GtkAllocation *allocation,
                                       gpointer user_data);
static void on_buffer_changed(GtkTextBuffer *buffer, gpointer user_data);
static void sync_source_from_buffer(MarkydEditor *self);
static void show_source_buffer(MarkydEditor *self);
static void render_preview(MarkydEditor *self);
static gchar *get_preview_base_uri(MarkydEditor *self);
static gboolean on_preview_decide_policy(WebKitWebView *web_view,
                                         WebKitPolicyDecision *decision,
                                         WebKitPolicyDecisionType type,
                                         gpointer user_data);

static gchar *get_url_from_iter_tags(GtkTextIter *iter) {
  GSList *tags;
  gchar *url = NULL;

  if (!iter) {
    return NULL;
  }

  tags = gtk_text_iter_get_tags(iter);
  for (GSList *node = tags; node != NULL; node = node->next) {
    GtkTextTag *tag = GTK_TEXT_TAG(node->data);
    const gchar *stored =
        (const gchar *)g_object_get_data(G_OBJECT(tag), VIEWMD_LINK_URL_DATA);
    if (stored && stored[0] != '\0') {
      url = g_strdup(stored);
      break;
    }
  }
  g_slist_free(tags);
  return url;
}

static gboolean get_link_url_at_iter(GtkTextBuffer *buffer, GtkTextIter *at,
                                     gchar **out_url) {
  GtkTextIter hit;

  if (!buffer || !at || !out_url) {
    return FALSE;
  }
  *out_url = NULL;

  hit = *at;
  *out_url = get_url_from_iter_tags(&hit);
  if (*out_url) {
    return TRUE;
  }

  hit = *at;
  if (gtk_text_iter_backward_char(&hit)) {
    *out_url = get_url_from_iter_tags(&hit);
    if (*out_url) {
      return TRUE;
    }
  }

  hit = *at;
  if (gtk_text_iter_forward_char(&hit)) {
    *out_url = get_url_from_iter_tags(&hit);
    if (*out_url) {
      return TRUE;
    }
  }

  return FALSE;
}

static gboolean scroll_to_markdown_anchor(MarkydEditor *self,
                                          const gchar *fragment) {
  gchar *mark_name;
  GtkTextMark *mark;

  if (!self || !self->buffer || !fragment) {
    return FALSE;
  }

  if (fragment[0] == '\0') {
    GtkTextIter start;
    gtk_text_buffer_get_start_iter(self->buffer, &start);
    gtk_text_buffer_place_cursor(self->buffer, &start);
    gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(self->text_view), &start, 0.2,
                                 FALSE, 0.0, 0.0);
    return TRUE;
  }

  mark_name = markdown_anchor_mark_name(fragment);
  mark = gtk_text_buffer_get_mark(self->buffer, mark_name);
  g_free(mark_name);
  if (!mark) {
    return FALSE;
  }

  {
    GtkTextIter at;
    gtk_text_buffer_get_iter_at_mark(self->buffer, &at, mark);
    gtk_text_buffer_place_cursor(self->buffer, &at);
  }
  gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(self->text_view), mark, 0.2, FALSE,
                               0.0, 0.0);
  return TRUE;
}

static void set_editor_mouse_cursor(MarkydEditor *self, gboolean over_link) {
  GdkWindow *win;
  GdkDisplay *display;
  GdkCursor *cursor;

  if (!self || !self->text_view) {
    return;
  }

  win = gtk_text_view_get_window(GTK_TEXT_VIEW(self->text_view),
                                 GTK_TEXT_WINDOW_TEXT);

  if (!win) {
    return;
  }

  display = gdk_window_get_display(win);
  cursor = gdk_cursor_new_from_name(display, over_link ? "pointer" : "text");
  if (!cursor) {
    cursor = gdk_cursor_new_for_display(display, over_link ? GDK_HAND2 : GDK_XTERM);
  }
  gdk_window_set_cursor(win, cursor);
  if (cursor) {
    g_object_unref(cursor);
  }
}

static void clear_editor_mouse_cursor(MarkydEditor *self) {
  GdkWindow *win;

  if (!self || !self->text_view) {
    return;
  }

  win = gtk_text_view_get_window(GTK_TEXT_VIEW(self->text_view),
                                 GTK_TEXT_WINDOW_TEXT);
  if (win) {
    gdk_window_set_cursor(win, NULL);
  }
}

static void apply_markdown(MarkydEditor *self) {
  if (!self) {
    return;
  }
  if (self->edit_mode) {
    return;
  }

  render_preview(self);

  self->updating_tags = TRUE;
  markdown_apply_tags(self->buffer,
                      self->source_content ? self->source_content : "");
  render_image_widgets(self);
  render_table_widgets(self);
  refresh_image_widget_scales(self);
  self->updating_tags = FALSE;
}

static gboolean resolve_image_source_path(MarkydEditor *self, const gchar *src,
                                          gchar **out_path) {
  gchar *path = NULL;

  if (out_path) {
    *out_path = NULL;
  }
  if (!self || !src || src[0] == '\0' || !out_path) {
    return FALSE;
  }

  if (g_uri_parse_scheme(src) != NULL) {
    if (g_str_has_prefix(src, "file://")) {
      path = g_filename_from_uri(src, NULL, NULL);
    } else {
      return FALSE;
    }
  } else if (g_path_is_absolute(src)) {
    path = g_strdup(src);
  } else {
    const gchar *current_path = markyd_app_get_current_path(self->app);
    if (current_path && current_path[0] != '\0') {
      gchar *dir = g_path_get_dirname(current_path);
      path = g_build_filename(dir, src, NULL);
      g_free(dir);
    } else {
      path = g_strdup(src);
    }
  }

  if (!path) {
    return FALSE;
  }
  if (!g_file_test(path, G_FILE_TEST_EXISTS)) {
    g_free(path);
    return FALSE;
  }

  *out_path = path;
  return TRUE;
}

static gint get_image_max_width(MarkydEditor *self) {
  GtkAllocation alloc;
  GdkRectangle visible = {0, 0, 0, 0};
  GtkTextView *view;
  gint width;

  if (!self || !self->text_view) {
    return 0;
  }

  view = GTK_TEXT_VIEW(self->text_view);
  gtk_text_view_get_visible_rect(view, &visible);
  width = visible.width;

  if (width <= 0) {
    gtk_widget_get_allocation(self->text_view, &alloc);
    width = alloc.width;
  }

  width -= gtk_text_view_get_left_margin(view);
  width -= gtk_text_view_get_right_margin(view);
  return MAX(width, 0);
}

static void compute_image_display_size(gint orig_width, gint orig_height,
                                       gint requested_width,
                                       gint requested_height, gint max_width,
                                       gint *out_width, gint *out_height) {
  gint width = orig_width;
  gint height = orig_height;

  if (orig_width <= 0 || orig_height <= 0) {
    width = MAX(requested_width, 1);
    height = MAX(requested_height, 1);
  } else if (requested_width > 0 && requested_height > 0) {
    width = requested_width;
    height = requested_height;
  } else if (requested_width > 0) {
    width = requested_width;
    height = (gint)(((gdouble)orig_height * (gdouble)width) /
                    (gdouble)orig_width + 0.5);
  } else if (requested_height > 0) {
    height = requested_height;
    width = (gint)(((gdouble)orig_width * (gdouble)height) /
                   (gdouble)orig_height + 0.5);
  }

  width = MAX(width, 1);
  height = MAX(height, 1);

  if (max_width > 0 && width > max_width) {
    height = (gint)(((gdouble)height * (gdouble)max_width) /
                    (gdouble)width + 0.5);
    width = max_width;
  }

  if (out_width) {
    *out_width = MAX(width, 1);
  }
  if (out_height) {
    *out_height = MAX(height, 1);
  }
}

static void scale_image_widget(GtkWidget *widget, gint max_width) {
  GtkWidget *image;
  GdkPixbuf *orig;
  gint ow;
  gint oh;
  gint requested_width;
  gint requested_height;
  gint display_width;
  gint display_height;

  if (!widget) {
    return;
  }

  image = g_object_get_data(G_OBJECT(widget), "viewmd-image-widget-child");
  orig = g_object_get_data(G_OBJECT(widget), "viewmd-image-orig-pixbuf");
  if (!GTK_IS_IMAGE(image) || !GDK_IS_PIXBUF(orig)) {
    return;
  }

  ow = gdk_pixbuf_get_width(orig);
  oh = gdk_pixbuf_get_height(orig);
  if (ow <= 0 || oh <= 0) {
    return;
  }

  requested_width = GPOINTER_TO_INT(
      g_object_get_data(G_OBJECT(widget), "viewmd-image-requested-width"));
  requested_height = GPOINTER_TO_INT(
      g_object_get_data(G_OBJECT(widget), "viewmd-image-requested-height"));

  compute_image_display_size(ow, oh, requested_width, requested_height, max_width,
                             &display_width, &display_height);

  if (display_width == ow && display_height == oh) {
    gtk_image_set_from_pixbuf(GTK_IMAGE(image), orig);
  } else {
    GdkPixbuf *scaled = gdk_pixbuf_scale_simple(
        orig, display_width, display_height, GDK_INTERP_BILINEAR);
    gtk_image_set_from_pixbuf(GTK_IMAGE(image), scaled ? scaled : orig);
    if (scaled) {
      g_object_unref(scaled);
    }
  }
}

static void refresh_image_widget_scales(MarkydEditor *self) {
  GtkTextIter iter;
  GtkTextIter end;
  gint max_width;

  if (!self || !self->buffer) {
    return;
  }

  max_width = get_image_max_width(self);
  gtk_text_buffer_get_bounds(self->buffer, &iter, &end);
  while (!gtk_text_iter_equal(&iter, &end)) {
    GtkTextChildAnchor *anchor = gtk_text_iter_get_child_anchor(&iter);
    if (anchor &&
        g_object_get_data(G_OBJECT(anchor), VIEWMD_IMAGE_ANCHOR_DATA) != NULL) {
      GtkWidget *image_widget =
          g_object_get_data(G_OBJECT(anchor), VIEWMD_IMAGE_WIDGET_DATA);
      if (image_widget) {
        scale_image_widget(image_widget, max_width);
      }
    }
    gtk_text_iter_forward_char(&iter);
  }
}

static void render_image_widgets(MarkydEditor *self) {
  GtkTextIter iter;
  GtkTextIter end;
  gint max_width;

  if (!self || !self->buffer || !self->text_view) {
    return;
  }

  max_width = get_image_max_width(self);
  gtk_text_buffer_get_bounds(self->buffer, &iter, &end);
  while (!gtk_text_iter_equal(&iter, &end)) {
    GtkTextChildAnchor *anchor = gtk_text_iter_get_child_anchor(&iter);
    if (anchor &&
        g_object_get_data(G_OBJECT(anchor), VIEWMD_IMAGE_ANCHOR_DATA) != NULL) {
      GtkWidget *image_widget =
          g_object_get_data(G_OBJECT(anchor), VIEWMD_IMAGE_WIDGET_DATA);
      if (!image_widget) {
        const gchar *src =
            g_object_get_data(G_OBJECT(anchor), VIEWMD_IMAGE_SRC_DATA);
        const gchar *alt =
            g_object_get_data(G_OBJECT(anchor), VIEWMD_IMAGE_ALT_DATA);
        gint requested_width = GPOINTER_TO_INT(
            g_object_get_data(G_OBJECT(anchor), VIEWMD_IMAGE_WIDTH_DATA));
        gint requested_height = GPOINTER_TO_INT(
            g_object_get_data(G_OBJECT(anchor), VIEWMD_IMAGE_HEIGHT_DATA));
        gchar *path = NULL;

        if (resolve_image_source_path(self, src, &path)) {
          GError *err = NULL;
          GdkPixbuf *orig = gdk_pixbuf_new_from_file(path, &err);
          if (orig) {
            GtkWidget *event_box = gtk_event_box_new();
            GtkWidget *image = gtk_image_new();
            gtk_event_box_set_visible_window(GTK_EVENT_BOX(event_box), FALSE);
            gtk_widget_set_halign(event_box, GTK_ALIGN_START);
            gtk_container_add(GTK_CONTAINER(event_box), image);
            g_object_set_data(G_OBJECT(event_box), "viewmd-image-widget-child",
                              image);
            g_object_set_data_full(G_OBJECT(event_box), "viewmd-image-orig-pixbuf",
                                   orig, g_object_unref);
            g_object_set_data(G_OBJECT(event_box), "viewmd-image-requested-width",
                              GINT_TO_POINTER(requested_width));
            g_object_set_data(G_OBJECT(event_box), "viewmd-image-requested-height",
                              GINT_TO_POINTER(requested_height));
            scale_image_widget(event_box, max_width);
            image_widget = event_box;
          } else if (err) {
            g_error_free(err);
          }
        }

        if (!image_widget) {
          GtkWidget *fallback = gtk_label_new(alt && alt[0] != '\0' ? alt : src);
          gtk_widget_set_halign(fallback, GTK_ALIGN_START);
          gtk_style_context_add_class(gtk_widget_get_style_context(fallback),
                                      "dim-label");
          image_widget = fallback;
        }

        gtk_text_view_add_child_at_anchor(GTK_TEXT_VIEW(self->text_view),
                                          image_widget, anchor);
        gtk_widget_show_all(image_widget);
        g_object_set_data(G_OBJECT(anchor), VIEWMD_IMAGE_WIDGET_DATA, image_widget);
        g_free(path);
      } else {
        scale_image_widget(image_widget, max_width);
      }
    }
    gtk_text_iter_forward_char(&iter);
  }
}

static void render_table_widgets(MarkydEditor *self) {
  GtkTextIter iter;
  GtkTextIter end;

  if (!self || !self->buffer || !self->text_view) {
    return;
  }

  gtk_text_buffer_get_bounds(self->buffer, &iter, &end);
  while (!gtk_text_iter_equal(&iter, &end)) {
    GtkTextChildAnchor *anchor = gtk_text_iter_get_child_anchor(&iter);
    if (anchor &&
        g_object_get_data(G_OBJECT(anchor), VIEWMD_TABLE_ANCHOR_DATA) != NULL) {
      GtkWidget *table =
          g_object_get_data(G_OBJECT(anchor), VIEWMD_TABLE_WIDGET_DATA);
      if (!table) {
        table = markdown_create_table_widget(anchor);
        if (table) {
          gtk_text_view_add_child_at_anchor(GTK_TEXT_VIEW(self->text_view), table,
                                            anchor);
          gtk_widget_show_all(table);
          g_object_set_data(G_OBJECT(anchor), VIEWMD_TABLE_WIDGET_DATA, table);
        }
      }
    }
    gtk_text_iter_forward_char(&iter);
  }
}

static gboolean apply_markdown_idle(gpointer user_data) {
  MarkydEditor *self = (MarkydEditor *)user_data;
  self->markdown_idle_id = 0;
  apply_markdown(self);
  return G_SOURCE_REMOVE;
}

static void schedule_markdown_apply(MarkydEditor *self) {
  if (!self || self->updating_tags || self->markdown_idle_id != 0) {
    return;
  }
  if (self->edit_mode) {
    return;
  }

  self->markdown_idle_id =
      g_idle_add_full(G_PRIORITY_LOW, apply_markdown_idle, self, NULL);
}

static void sync_source_from_buffer(MarkydEditor *self) {
  GtkTextIter start;
  GtkTextIter end;
  gchar *text;

  if (!self || !self->buffer) {
    return;
  }

  gtk_text_buffer_get_bounds(self->buffer, &start, &end);
  text = gtk_text_buffer_get_text(self->buffer, &start, &end, FALSE);
  g_free(self->source_content);
  self->source_content = text ? text : g_strdup("");
}

static void show_source_buffer(MarkydEditor *self) {
  if (!self || !self->buffer) {
    return;
  }

  self->updating_tags = TRUE;
  gtk_text_buffer_set_text(self->buffer,
                           self->source_content ? self->source_content : "", -1);
  self->updating_tags = FALSE;
}

static void on_buffer_changed(GtkTextBuffer *buffer, gpointer user_data) {
  MarkydEditor *self = (MarkydEditor *)user_data;
  (void)buffer;

  if (!self || self->updating_tags || !self->edit_mode) {
    return;
  }

  sync_source_from_buffer(self);
}

static gchar *get_preview_base_uri(MarkydEditor *self) {
  const gchar *current_path;
  gchar *dir;
  GFile *dir_file;
  gchar *uri;
  gchar *base_uri;

  current_path = self && self->app ? markyd_app_get_current_path(self->app) : NULL;
  if (current_path && current_path[0] != '\0') {
    dir = g_path_get_dirname(current_path);
  } else {
    dir = g_get_current_dir();
  }

  dir_file = g_file_new_for_path(dir);
  uri = g_file_get_uri(dir_file);
  base_uri = g_str_has_suffix(uri, "/") ? g_strdup(uri) : g_strconcat(uri, "/", NULL);

  g_free(uri);
  g_object_unref(dir_file);
  g_free(dir);
  return base_uri;
}

static void render_preview(MarkydEditor *self) {
  gchar *base_uri;
  gchar *html;

  if (!self || !self->preview_view) {
    return;
  }

  base_uri = get_preview_base_uri(self);
  html = markdown_render_github_html(self->source_content ? self->source_content : "",
                                     base_uri);

  g_free(self->preview_base_uri);
  self->preview_base_uri = g_strdup(base_uri);
  webkit_web_view_load_html(WEBKIT_WEB_VIEW(self->preview_view), html, base_uri);

  g_free(html);
  g_free(base_uri);
}

static gboolean preview_uri_is_same_document(MarkydEditor *self,
                                             const gchar *uri) {
  const gchar *fragment;

  if (!uri || uri[0] == '\0') {
    return TRUE;
  }
  if (uri[0] == '#') {
    return TRUE;
  }
  if (g_str_has_prefix(uri, "about:")) {
    return TRUE;
  }

  fragment = strchr(uri, '#');
  if (fragment && self && self->preview_base_uri &&
      g_str_has_prefix(uri, self->preview_base_uri)) {
    return TRUE;
  }

  return FALSE;
}

static gboolean path_is_markdown_file(const gchar *path) {
  gchar *lower;
  gboolean result;

  if (!path) {
    return FALSE;
  }

  lower = g_ascii_strdown(path, -1);
  result = g_str_has_suffix(lower, ".md") ||
           g_str_has_suffix(lower, ".markdown");
  g_free(lower);
  return result;
}

static gboolean open_local_markdown_link(MarkydEditor *self, const gchar *uri) {
  gchar *path;
  gchar *fragment;
  gboolean opened = FALSE;

  if (!self || !self->app || !uri || !g_str_has_prefix(uri, "file://")) {
    return FALSE;
  }

  path = g_filename_from_uri(uri, NULL, NULL);
  if (!path) {
    return FALSE;
  }

  fragment = strchr(path, '#');
  if (fragment) {
    *fragment = '\0';
  }

  if (path_is_markdown_file(path) && g_file_test(path, G_FILE_TEST_EXISTS)) {
    opened = markyd_app_open_file(self->app, path);
  }

  g_free(path);
  return opened;
}

static void open_preview_uri_externally(GtkWidget *widget, const gchar *uri) {
  GtkWidget *toplevel;
  GError *error = NULL;

  if (!uri || uri[0] == '\0') {
    return;
  }

  toplevel = gtk_widget_get_toplevel(widget);
  if (!gtk_show_uri_on_window(GTK_IS_WINDOW(toplevel) ? GTK_WINDOW(toplevel)
                                                      : NULL,
                              uri, GDK_CURRENT_TIME, &error)) {
    if (error) {
      g_printerr("Failed to open link '%s': %s\n", uri, error->message);
      g_clear_error(&error);
    }
  }
}

static gboolean on_preview_decide_policy(WebKitWebView *web_view,
                                         WebKitPolicyDecision *decision,
                                         WebKitPolicyDecisionType type,
                                         gpointer user_data) {
  MarkydEditor *self = (MarkydEditor *)user_data;
  WebKitNavigationPolicyDecision *navigation_decision;
  WebKitNavigationAction *action;
  WebKitURIRequest *request;
  const gchar *uri;

  if (type != WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION) {
    return FALSE;
  }

  navigation_decision = WEBKIT_NAVIGATION_POLICY_DECISION(decision);
  action = webkit_navigation_policy_decision_get_navigation_action(
      navigation_decision);
  if (!action ||
      webkit_navigation_action_get_navigation_type(action) !=
          WEBKIT_NAVIGATION_TYPE_LINK_CLICKED) {
    return FALSE;
  }

  request = webkit_navigation_action_get_request(action);
  uri = request ? webkit_uri_request_get_uri(request) : NULL;
  if (preview_uri_is_same_document(self, uri)) {
    return FALSE;
  }

  if (open_local_markdown_link(self, uri)) {
    webkit_policy_decision_ignore(decision);
    return TRUE;
  }

  open_preview_uri_externally(GTK_WIDGET(web_view), uri);
  webkit_policy_decision_ignore(decision);
  return TRUE;
}

void markyd_editor_refresh(MarkydEditor *self) {
  if (!self) {
    return;
  }
  if (self->edit_mode) {
    return;
  }
  schedule_markdown_apply(self);
}

void markyd_editor_set_edit_mode(MarkydEditor *self, gboolean edit_mode) {
  if (!self || self->edit_mode == edit_mode) {
    return;
  }

  if (edit_mode) {
    if (self->markdown_idle_id != 0) {
      g_source_remove(self->markdown_idle_id);
      self->markdown_idle_id = 0;
    }
    self->edit_mode = TRUE;
    show_source_buffer(self);
    gtk_text_view_set_editable(GTK_TEXT_VIEW(self->text_view), TRUE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(self->text_view), TRUE);
    gtk_stack_set_visible_child_name(GTK_STACK(self->widget), "source");
    gtk_widget_grab_focus(self->text_view);
  } else {
    sync_source_from_buffer(self);
    self->edit_mode = FALSE;
    gtk_text_view_set_editable(GTK_TEXT_VIEW(self->text_view), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(self->text_view), FALSE);
    gtk_stack_set_visible_child_name(GTK_STACK(self->widget), "preview");
    schedule_markdown_apply(self);
  }
}

gboolean markyd_editor_get_edit_mode(MarkydEditor *self) {
  return self ? self->edit_mode : FALSE;
}

MarkydEditor *markyd_editor_new(MarkydApp *app) {
  MarkydEditor *self = g_new0(MarkydEditor, 1);
  WebKitSettings *settings;

  self->app = app;
  self->source_content = g_strdup("");
  self->updating_tags = FALSE;
  self->markdown_idle_id = 0;
  self->edit_mode = FALSE;
  self->preview_base_uri = NULL;

  self->widget = gtk_stack_new();
  gtk_stack_set_transition_type(GTK_STACK(self->widget),
                                GTK_STACK_TRANSITION_TYPE_NONE);
  gtk_widget_set_hexpand(self->widget, TRUE);
  gtk_widget_set_vexpand(self->widget, TRUE);

  self->text_view = gtk_text_view_new();
  gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(self->text_view),
                              GTK_WRAP_WORD_CHAR);
  gtk_text_view_set_left_margin(GTK_TEXT_VIEW(self->text_view), 16);
  gtk_text_view_set_right_margin(GTK_TEXT_VIEW(self->text_view), 16);
  gtk_text_view_set_top_margin(GTK_TEXT_VIEW(self->text_view), 16);
  gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(self->text_view), 16);
  gtk_text_view_set_editable(GTK_TEXT_VIEW(self->text_view), FALSE);
  gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(self->text_view), FALSE);

  self->source_scroll = gtk_scrolled_window_new(NULL, NULL);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(self->source_scroll),
                                 GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
  gtk_container_add(GTK_CONTAINER(self->source_scroll), self->text_view);

  self->preview_view = webkit_web_view_new();
  gtk_widget_set_hexpand(self->preview_view, TRUE);
  gtk_widget_set_vexpand(self->preview_view, TRUE);
  settings = webkit_web_view_get_settings(WEBKIT_WEB_VIEW(self->preview_view));
  webkit_settings_set_enable_javascript(settings, TRUE);
  webkit_settings_set_enable_javascript_markup(settings, FALSE);
  webkit_settings_set_auto_load_images(settings, TRUE);
  g_signal_connect(self->preview_view, "decide-policy",
                   G_CALLBACK(on_preview_decide_policy), self);

  gtk_stack_add_named(GTK_STACK(self->widget), self->preview_view, "preview");
  gtk_stack_add_named(GTK_STACK(self->widget), self->source_scroll, "source");
  gtk_stack_set_visible_child_name(GTK_STACK(self->widget), "preview");

  self->buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(self->text_view));
  markdown_init_tags(self->buffer);
  g_signal_connect(self->buffer, "changed", G_CALLBACK(on_buffer_changed), self);

  gtk_widget_add_events(self->text_view, GDK_POINTER_MOTION_MASK |
                                            GDK_LEAVE_NOTIFY_MASK |
                                            GDK_BUTTON_RELEASE_MASK);
  g_signal_connect(self->text_view, "button-release-event",
                   G_CALLBACK(on_button_release), self);
  g_signal_connect(self->text_view, "motion-notify-event",
                   G_CALLBACK(on_motion_notify), self);
  g_signal_connect(self->text_view, "leave-notify-event",
                   G_CALLBACK(on_leave_notify), self);
  g_signal_connect(self->text_view, "size-allocate",
                   G_CALLBACK(on_text_view_size_allocate), self);

  return self;
}

static void on_text_view_size_allocate(GtkWidget *widget, GtkAllocation *allocation,
                                       gpointer user_data) {
  MarkydEditor *self = (MarkydEditor *)user_data;
  (void)widget;
  (void)allocation;
  refresh_image_widget_scales(self);
}

void markyd_editor_free(MarkydEditor *self) {
  if (!self) {
    return;
  }
  if (self->markdown_idle_id != 0) {
    g_source_remove(self->markdown_idle_id);
    self->markdown_idle_id = 0;
  }
  g_free(self->source_content);
  g_free(self->preview_base_uri);
  g_free(self);
}

void markyd_editor_set_content(MarkydEditor *self, const gchar *content) {
  if (!self) {
    return;
  }

  g_free(self->source_content);
  self->source_content = g_strdup(content ? content : "");
  if (self->edit_mode) {
    show_source_buffer(self);
  } else {
    schedule_markdown_apply(self);
  }
}

gchar *markyd_editor_get_content(MarkydEditor *self) {
  if (!self) {
    return g_strdup("");
  }
  if (self->edit_mode) {
    sync_source_from_buffer(self);
  }
  return g_strdup(self->source_content ? self->source_content : "");
}

GtkWidget *markyd_editor_get_widget(MarkydEditor *self) {
  return self ? self->widget : NULL;
}

void markyd_editor_focus(MarkydEditor *self) {
  GtkTextMark *insert_mark;

  if (!self) {
    return;
  }

  if (!self->edit_mode) {
    if (self->preview_view) {
      gtk_widget_grab_focus(self->preview_view);
    }
    return;
  }

  if (!self->text_view || !self->buffer) {
    return;
  }

  gtk_widget_grab_focus(self->text_view);
  insert_mark = gtk_text_buffer_get_insert(self->buffer);
  if (insert_mark) {
    gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(self->text_view), insert_mark,
                                 0.0, FALSE, 0.0, 0.0);
  }
}

void markyd_editor_scroll_top(MarkydEditor *self) {
  GtkTextIter start;

  if (!self) {
    return;
  }

  if (!self->edit_mode && self->preview_view) {
    webkit_web_view_evaluate_javascript(WEBKIT_WEB_VIEW(self->preview_view),
                                        "window.scrollTo(0, 0);", -1, NULL,
                                        NULL, NULL, NULL, NULL);
    return;
  }

  if (!self->buffer || !self->text_view) {
    return;
  }

  gtk_text_buffer_get_start_iter(self->buffer, &start);
  gtk_text_buffer_place_cursor(self->buffer, &start);
  gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(self->text_view), &start, 0.0,
                               FALSE, 0.0, 0.0);
}

void markyd_editor_find(MarkydEditor *self, const gchar *query) {
  WebKitFindController *controller;

  if (!self || self->edit_mode || !self->preview_view || !query ||
      query[0] == '\0') {
    return;
  }

  controller = webkit_web_view_get_find_controller(
      WEBKIT_WEB_VIEW(self->preview_view));
  webkit_find_controller_search(controller, query,
                                WEBKIT_FIND_OPTIONS_CASE_INSENSITIVE |
                                    WEBKIT_FIND_OPTIONS_WRAP_AROUND,
                                G_MAXUINT);
}

void markyd_editor_find_next(MarkydEditor *self, gboolean previous) {
  WebKitFindController *controller;

  if (!self || self->edit_mode || !self->preview_view) {
    return;
  }

  controller = webkit_web_view_get_find_controller(
      WEBKIT_WEB_VIEW(self->preview_view));
  if (previous) {
    webkit_find_controller_search_previous(controller);
  } else {
    webkit_find_controller_search_next(controller);
  }
}

void markyd_editor_find_clear(MarkydEditor *self) {
  WebKitFindController *controller;

  if (!self || !self->preview_view) {
    return;
  }

  controller = webkit_web_view_get_find_controller(
      WEBKIT_WEB_VIEW(self->preview_view));
  webkit_find_controller_search_finish(controller);
}

static gboolean on_button_release(GtkWidget *widget, GdkEventButton *event,
                                  gpointer user_data) {
  MarkydEditor *self = (MarkydEditor *)user_data;
  GtkTextIter iter;
  gint bx, by;
  gchar *url = NULL;
  GError *error = NULL;

  if (event->button != 1) {
    return FALSE;
  }

  gtk_text_view_window_to_buffer_coords(GTK_TEXT_VIEW(widget),
                                        GTK_TEXT_WINDOW_TEXT, (gint)event->x,
                                        (gint)event->y, &bx, &by);
  gtk_text_view_get_iter_at_location(GTK_TEXT_VIEW(widget), &iter, bx, by);

  if (!get_link_url_at_iter(self->buffer, &iter, &url)) {
    return FALSE;
  }

  if (url[0] == '#') {
    gchar *fragment = g_strdup(url + 1);
    gchar *space = strpbrk(fragment, " \t");
    if (space) {
      *space = '\0';
    }
    if (!scroll_to_markdown_anchor(self, fragment)) {
      g_printerr("Anchor not found: '%s'\n", url);
    }
    g_free(fragment);
    g_free(url);
    return TRUE;
  }

  if (g_uri_parse_scheme(url) == NULL) {
    gchar *with_scheme = g_strdup_printf("https://%s", url);
    g_free(url);
    url = with_scheme;
  }

  GtkWidget *toplevel = gtk_widget_get_toplevel(widget);
  if (!gtk_show_uri_on_window(GTK_IS_WINDOW(toplevel) ? GTK_WINDOW(toplevel)
                                                      : NULL,
                              url, GDK_CURRENT_TIME, &error)) {
    if (error) {
      g_printerr("Failed to open link '%s': %s\n", url, error->message);
      g_clear_error(&error);
    }
  }

  g_free(url);
  return TRUE;
}

static gboolean on_motion_notify(GtkWidget *widget, GdkEventMotion *event,
                                 gpointer user_data) {
  MarkydEditor *self = (MarkydEditor *)user_data;
  GtkTextIter iter;
  gint bx, by;
  gchar *url = NULL;

  gtk_text_view_window_to_buffer_coords(GTK_TEXT_VIEW(widget),
                                        GTK_TEXT_WINDOW_TEXT, (gint)event->x,
                                        (gint)event->y, &bx, &by);
  gtk_text_view_get_iter_at_location(GTK_TEXT_VIEW(widget), &iter, bx, by);

  set_editor_mouse_cursor(self, get_link_url_at_iter(self->buffer, &iter, &url));
  g_free(url);
  return FALSE;
}

static gboolean on_leave_notify(GtkWidget *widget, GdkEventCrossing *event,
                                gpointer user_data) {
  MarkydEditor *self = (MarkydEditor *)user_data;
  (void)widget;
  (void)event;
  clear_editor_mouse_cursor(self);
  return FALSE;
}
