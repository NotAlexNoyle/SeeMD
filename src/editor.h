#ifndef MARKYD_EDITOR_H
#define MARKYD_EDITOR_H

#include <gtk/gtk.h>

typedef struct _MarkydApp MarkydApp;

typedef struct _MarkydEditor {
  GtkWidget *widget;
  GtkWidget *source_scroll;
  GtkWidget *text_view;
  GtkWidget *preview_view;
  GtkTextBuffer *buffer;
  MarkydApp *app;

  /* Original markdown content loaded into the viewer. */
  gchar *source_content;

  /* Prevent recursive tag application. */
  gboolean updating_tags;

  /* Coalesce markdown re-rendering to idle to avoid invalidating GTK iterators. */
  guint markdown_idle_id;

  /* TRUE when the buffer is showing editable markdown source instead of preview. */
  gboolean edit_mode;

  gchar *preview_base_uri;
} MarkydEditor;

/* Lifecycle */
MarkydEditor *markyd_editor_new(MarkydApp *app);
void markyd_editor_free(MarkydEditor *editor);

/* Content management */
void markyd_editor_set_content(MarkydEditor *editor, const gchar *content);
gchar *markyd_editor_get_content(MarkydEditor *editor);

/* Source editing mode */
void markyd_editor_set_edit_mode(MarkydEditor *editor, gboolean edit_mode);
gboolean markyd_editor_get_edit_mode(MarkydEditor *editor);

/* Widget access */
GtkWidget *markyd_editor_get_widget(MarkydEditor *editor);
void markyd_editor_focus(MarkydEditor *editor);
void markyd_editor_scroll_top(MarkydEditor *editor);

/* Preview search */
void markyd_editor_find(MarkydEditor *editor, const gchar *query);
void markyd_editor_find_next(MarkydEditor *editor, gboolean previous);
void markyd_editor_find_clear(MarkydEditor *editor);

/* Force a refresh of markdown styling/rendering (e.g., after settings change). */
void markyd_editor_refresh(MarkydEditor *editor);

#endif /* MARKYD_EDITOR_H */
