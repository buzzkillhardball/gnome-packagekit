/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2007 Richard Hughes <richard@hughsie.com>
 *
 * Licensed under the GNU General Public License Version 2
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#include "config.h"

#include <glib.h>
#include <glib/gi18n.h>

#include <glade/glade.h>
#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>
#include <math.h>
#include <string.h>
#include <dbus/dbus-glib.h>
#include <locale.h>

#include <pk-debug.h>
#include <pk-client.h>
#include <pk-common.h>
#include <pk-task-list.h>
#include <pk-connection.h>
#include <pk-package-id.h>
#include <pk-package-ids.h>
#include <pk-enum-list.h>
#include "pk-common-gui.h"
#include "pk-statusbar.h"
#include "pk-cell-renderer-uri.h"

static GladeXML *glade_xml = NULL;
static GtkListStore *list_store_preview = NULL;
static GtkListStore *list_store_details = NULL;
static GtkListStore *list_store_description = NULL;
static PkClient *client = NULL;
static PkTaskList *tlist = NULL;
static gchar *package = NULL;

/* for the preview throbber */
static void pk_updates_add_preview_item (PkClient *client, const gchar *icon, const gchar *message, gboolean clear);
static int animation_timeout = 0;
static int frame_counter = 0;
static int n_frames = 0;
static GdkPixbuf **frames = NULL;

enum {
	PREVIEW_COLUMN_ICON,
	PREVIEW_COLUMN_TEXT,
	PREVIEW_COLUMN_PROGRESS,
	PREVIEW_COLUMN_LAST
};

enum {
	DESC_COLUMN_TITLE,
	DESC_COLUMN_TEXT,
	DESC_COLUMN_URI,
	DESC_COLUMN_LAST
};

enum {
	HISTORY_COLUMN_ICON,
	HISTORY_COLUMN_TEXT,
	HISTORY_COLUMN_LAST
};

enum {
	PACKAGES_COLUMN_ICON,
	PACKAGES_COLUMN_TEXT,
	PACKAGES_COLUMN_ID,
	PACKAGES_COLUMN_INFO,
	PACKAGES_COLUMN_SELECT,
	PACKAGES_COLUMN_LAST
};

typedef enum {
	PAGE_PREVIEW,
	PAGE_DETAILS,
	PAGE_PROGRESS,
	PAGE_CONFIRM,
	PAGE_ERROR,
	PAGE_LAST
} PkPageEnum;

/**
 * pk_button_help_cb:
 **/
static void
pk_button_help_cb (GtkWidget *widget, gboolean data)
{
	pk_debug ("emitting action-help");
}

/**
 * pk_updates_set_page:
 **/
static void
pk_updates_set_page (PkPageEnum page)
{
	GList *list, *l;
	GtkWidget *widget;
	guint i;

	widget = glade_xml_get_widget (glade_xml, "hbox_hidden");
	list = gtk_container_get_children (GTK_CONTAINER (widget));
	for (l=list, i=0; l; l=l->next, i++) {
		if (i == page) {
			gtk_widget_show (l->data);
		} else {
			gtk_widget_hide (l->data);
		}
	}

	/* some pages are resizeable */
	widget = glade_xml_get_widget (glade_xml, "window_updates");
	if (page == PAGE_DETAILS) {
		gtk_window_set_resizable (GTK_WINDOW (widget), TRUE);
	} else {
		gtk_window_set_resizable (GTK_WINDOW (widget), FALSE);
	}
}

/**
 * pk_updates_apply_cb:
 **/
static void
pk_updates_apply_cb (GtkWidget *widget, gpointer data)
{
	GtkTreeModel *model;
	GtkTreeIter iter;
	gboolean valid;
	gboolean update;
	gboolean selected_all = TRUE;
	gboolean selected_any = FALSE;
	gchar *package_id;
	GPtrArray *array;

	pk_debug ("Doing the system update");
	array = g_ptr_array_new ();

	widget = glade_xml_get_widget (glade_xml, "treeview_updates");
	model = gtk_tree_view_get_model (GTK_TREE_VIEW (widget));

	/* get the first iter in the list */
	valid = gtk_tree_model_get_iter_first (model, &iter);

	/* find out how many we should update */
	while (valid) {
		gtk_tree_model_get (model, &iter, PACKAGES_COLUMN_SELECT, &update,
				    PACKAGES_COLUMN_ID, &package_id, -1);

		if (!update) {
			selected_all = FALSE;
		} else {
			selected_any = TRUE;
		}

		/* do something with the data */
		if (update) {
			pk_debug ("%s", package_id);
			g_ptr_array_add (array, package_id);
		} else {
			/* need to free the one in the array later */
			g_free (package_id);
		}
		valid = gtk_tree_model_iter_next (model, &iter);
	}

	/* we have no checkboxes selected */
	if (!selected_any) {
		pk_error_modal_dialog (_("No updates selected"), _("No updates are selected"));
		return;
	}

	/* set correct view */
	pk_updates_set_page (PAGE_PROGRESS);

	/* send an singular list */
	if (!selected_all) {
		gchar **package_ids;
		gboolean ret;
		GError *error = NULL;

		package_ids = pk_package_ids_from_array (array);
		pk_client_reset (client, NULL);
		ret = pk_client_update_packages_strv (client, package_ids, &error);
		if (!ret) {
			pk_error_modal_dialog ("Individual updates failed", error->message);
			g_error_free (error);
		}
		g_strfreev (package_ids);
	}

	/* get rid of the array, and free the contents */
	g_ptr_array_free (array, TRUE);

	/* the trivial case */
	if (selected_all) {
		pk_client_reset (client, NULL);
		pk_client_update_system (client, NULL);
	}
}

/**
 * pk_updates_preview_animation_update:
 **/
static gboolean
pk_updates_preview_animation_update (gpointer data)
{
	GtkTreeModel *model = data;
	GtkTreeIter iter;

	gtk_tree_model_get_iter_first (model, &iter);
	gtk_list_store_set (GTK_LIST_STORE (model), &iter,
			    PREVIEW_COLUMN_PROGRESS, frames[frame_counter],
			    -1);

	frame_counter = (frame_counter + 1) % n_frames;

	return TRUE;
}

/**
 * pk_updates_preview_animation_start:
 **/
static void
pk_updates_preview_animation_start (void)
{
	GtkWidget *widget;
	GdkPixbuf *pixbuf;
	GtkCellRenderer *renderer;
	GtkTreeViewColumn *column;
	GtkTreeModel *model;
	GList *list;
	gint w, h;
	gint rows, cols;
	gint r, c, i;

	pk_updates_add_preview_item (client,  NULL, _("Getting information..."), TRUE);

	if (frames == NULL) {
		/* get the process-working animation from the icon theme
		 * and split it into frames.
		 * FIXME reset frames on theme changes
		 */
		widget = glade_xml_get_widget (glade_xml, "window_updates");
		gtk_icon_size_lookup (GTK_ICON_SIZE_DIALOG, &w, &h);
		pixbuf = gtk_icon_theme_load_icon (gtk_icon_theme_get_default (),
						   "process-working",
						   w, 0, NULL);

		cols = gdk_pixbuf_get_width (pixbuf) / w;
		rows = gdk_pixbuf_get_height (pixbuf) / h;

		n_frames = rows * cols;
		frames = g_new (GdkPixbuf*, n_frames);

		for (i = 0, r = 0; r < rows; r++)
			for (c = 0; c < cols; c++, i++) {
			frames[i] = gdk_pixbuf_new_subpixbuf (pixbuf, c * w, r * h, w, h);
		}

		g_object_unref (pixbuf);
	}

	widget = glade_xml_get_widget (glade_xml, "treeview_preview");
	column = gtk_tree_view_get_column (GTK_TREE_VIEW (widget), 0);
	list = gtk_tree_view_column_get_cell_renderers (column);
	renderer = list->data;
	g_list_free (list);
	gtk_tree_view_column_clear_attributes (column, renderer);
	gtk_tree_view_column_set_attributes (column, renderer,
					     "pixbuf", PREVIEW_COLUMN_PROGRESS, NULL);

	model = gtk_tree_view_get_model (GTK_TREE_VIEW (widget));
	frame_counter = 0;

	animation_timeout = g_timeout_add (50, pk_updates_preview_animation_update, model);
	pk_updates_preview_animation_update (model);
}

/**
 * pk_updates_preview_animation_stop:
 **/
static void
pk_updates_preview_animation_stop (void)
{
	GtkWidget *widget;
	GtkCellRenderer *renderer;
	GtkTreeViewColumn *column;
	GList *list;

	if (animation_timeout == 0)
		return;

	g_source_remove (animation_timeout);
	animation_timeout = 0;

	widget = glade_xml_get_widget (glade_xml, "treeview_preview");
	column = gtk_tree_view_get_column (GTK_TREE_VIEW (widget), 0);
	list = gtk_tree_view_column_get_cell_renderers (column);
	renderer = list->data;
	g_list_free (list);
	gtk_tree_view_column_clear_attributes (column, renderer);
	gtk_tree_view_column_set_attributes (column, renderer,
					     "icon-name", PREVIEW_COLUMN_ICON, NULL);
}

/**
 * pk_updates_refresh_cb:
 **/
static void
pk_updates_refresh_cb (GtkWidget *widget, gboolean data)
{
	gboolean ret;

	/* clear existing list */
	gtk_list_store_clear (list_store_details);
	pk_updates_preview_animation_start ();

	/* make the refresh button non-clickable */
	gtk_widget_set_sensitive (widget, FALSE);

	/* make the apply button non-clickable until we get completion */
	widget = glade_xml_get_widget (glade_xml, "button_apply");
	gtk_widget_set_sensitive (widget, FALSE);
	widget = glade_xml_get_widget (glade_xml, "button_apply2");
	gtk_widget_set_sensitive (widget, FALSE);
	widget = glade_xml_get_widget (glade_xml, "button_review");
	gtk_widget_set_sensitive (widget, FALSE);

	/* we can't click this if we havn't finished */
	pk_client_reset (client, NULL);
	ret = pk_client_refresh_cache (client, TRUE, NULL);
	if (ret == FALSE) {
		pk_warning ("failed to refresh cache");
	}
}

/**
 * pk_button_cancel_cb:
 **/
static void
pk_button_cancel_cb (GtkWidget *widget, gpointer data)
{
	/* we might have a transaction running */
	pk_client_cancel (client, NULL);
}

/**
 * pk_button_close_cb:
 **/
static void
pk_button_close_cb (GtkWidget *widget, gpointer data)
{
	GMainLoop *loop = (GMainLoop *) data;

	/* we might have a transaction running */
	pk_client_cancel (client, NULL);

	g_main_loop_quit (loop);
	pk_debug ("emitting action-close");
}

/**
 * pk_button_review_cb:
 **/
static void
pk_button_review_cb (GtkWidget *widget, gpointer data)
{
	GtkWidget *treeview;
	GtkTreeSelection *selection;
	GtkTreeIter iter;

	treeview = glade_xml_get_widget (glade_xml, "treeview_updates");
	selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (treeview));
	if (!gtk_tree_selection_get_selected (selection, NULL, NULL)) {
		if (gtk_tree_model_get_iter_first (gtk_tree_view_get_model (GTK_TREE_VIEW (treeview)),
						   &iter))
			gtk_tree_selection_select_iter (selection, &iter);
	}
	/* set correct view */
	pk_updates_set_page (PAGE_DETAILS);
}

/**
 * pk_button_overview_cb:
 **/
static void
pk_button_overview_cb (GtkWidget *widget, gpointer data)
{
	/* set correct view */
	pk_updates_set_page (PAGE_PREVIEW);
}

/**
 * pk_updates_package_cb:
 **/
static void
pk_updates_package_cb (PkClient *client, PkInfoEnum info, const gchar *package_id,
		       const gchar *summary, gpointer data)
{
	GtkTreeIter iter;
	gchar *text;
	PkRoleEnum role;
	const gchar *icon_name;
	GtkWidget *widget;

	pk_client_get_role (client, &role, NULL, NULL);
	pk_debug ("role = %s, package = %s:%s:%s", pk_role_enum_to_text (role),
		  pk_info_enum_to_text (info), package_id, summary);

	if (role == PK_ROLE_ENUM_GET_UPDATES) {
		text = pk_package_id_pretty (package_id, summary);
		icon_name = pk_info_enum_to_icon_name (info);
		gtk_list_store_append (list_store_details, &iter);
		gtk_list_store_set (list_store_details, &iter,
				    PACKAGES_COLUMN_TEXT, text,
				    PACKAGES_COLUMN_ID, package_id,
				    PACKAGES_COLUMN_ICON, icon_name,
				    PACKAGES_COLUMN_INFO, info,
				    PACKAGES_COLUMN_SELECT, TRUE,
				    -1);
		g_free (text);
		return;
	}

	if (role == PK_ROLE_ENUM_UPDATE_SYSTEM ||
	    role == PK_ROLE_ENUM_UPDATE_PACKAGES) {
		text = pk_package_id_pretty_oneline (package_id, summary);
		widget = glade_xml_get_widget (glade_xml, "progress_package_label");
		gtk_label_set_markup (GTK_LABEL (widget), text);

		g_free (text);
		return;
	}
}

/**
 * pk_updates_add_description_item:
 **/
static void
pk_updates_add_description_item (const gchar *title, const gchar *text, const gchar *uri)
{
	gchar *markup;
	GtkWidget *tree_view;
	GtkTreeIter iter;
	GtkTreeSelection *selection;

	/* format */
	markup = g_strdup_printf ("<b>%s:</b>", title);

	pk_debug ("%s %s %s", markup, text, uri);
	gtk_list_store_append (list_store_description, &iter);
	gtk_list_store_set (list_store_description, &iter,
			    DESC_COLUMN_TITLE, markup,
			    DESC_COLUMN_TEXT, text,
			    DESC_COLUMN_URI, uri,
			    -1);

	g_free (markup);

	tree_view = glade_xml_get_widget (glade_xml, "treeview_description");
	selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (tree_view));
	gtk_tree_selection_set_mode (selection, GTK_SELECTION_NONE);
}

/**
 * pk_updates_add_description_link_item:
 **/
static void
pk_updates_add_description_link_item (const gchar *title, const gchar *url_string)
{
	const gchar *text;
	const gchar *uri;
	gchar *title_num;
	gchar **urls;
	guint length;
	gint i;

	urls = g_strsplit (url_string, ";", 0);
	length = g_strv_length (urls);
	for (i = 0; urls[i]; i += 2) {
		uri = urls[i];
		text = urls[i+1];
		if (pk_strzero (text)) {
			text = uri;
		}
		/* no suffix needed */
		if (length == 2) {
			pk_updates_add_description_item (title, text, uri);
		} else {
			title_num = g_strdup_printf ("%s (%i)", title, (i/2) + 1);
			pk_updates_add_description_item (title_num, text, uri);
			g_free (title_num);
		}
	}
	g_strfreev (urls);
}

/**
 * pk_updates_update_detail_cb:
 **/
static void
pk_updates_update_detail_cb (PkClient *client, const gchar *package_id,
			     const gchar *updates, const gchar *obsoletes,
			     const gchar *vendor_url, const gchar *bugzilla_url,
			     const gchar *cve_url, PkRestartEnum restart,
			     const gchar *update_text, gpointer data)
{
	GtkWidget *widget;
	PkPackageId *ident;
	GtkTreeSelection *selection;
	GtkTreeModel *model;
	GtkTreeIter treeiter;
	gchar *package_pretty;
	gchar *updates_pretty;
	gchar *obsoletes_pretty;
	const gchar *info_text;
	PkInfoEnum info;

	/* clear existing list */
	gtk_list_store_clear (list_store_description);

	/* initially we are hidden */
	widget = glade_xml_get_widget (glade_xml, "scrolledwindow_description");
	gtk_widget_show (widget);

	widget = glade_xml_get_widget (glade_xml, "treeview_updates");
	gtk_widget_set_size_request (GTK_WIDGET (widget), 500, 200);

	widget = glade_xml_get_widget (glade_xml, "treeview_description");
	gtk_widget_set_size_request (GTK_WIDGET (widget), 500, 200);

	/* get info  */
	widget = glade_xml_get_widget (glade_xml, "treeview_updates");
	selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (widget));
	if (gtk_tree_selection_get_selected (selection, &model, &treeiter)) {
		gtk_tree_model_get (model, &treeiter,
				    PACKAGES_COLUMN_INFO, &info, -1);
	} else {
		info = PK_INFO_ENUM_NORMAL;
	}

	info_text = pk_info_enum_to_localised_text (info);
	/* translators: this is the update type, e.g. security */
	pk_updates_add_description_item (_("Type"), info_text, NULL);

	package_pretty = pk_package_id_name_version (package_id);
	/* translators: this is the package version */
	pk_updates_add_description_item (_("Version"), package_pretty, NULL);
	g_free (package_pretty);

	if (!pk_strzero (updates)) {
		updates_pretty = pk_package_id_name_version (updates);
		/* translators: this is a list of packages that are updated */
		pk_updates_add_description_item (_("Updates"), updates_pretty, NULL);
		g_free (updates_pretty);
	}

	if (!pk_strzero (obsoletes)) {
		obsoletes_pretty = pk_package_id_name_version (obsoletes);
		/* translators: this is a list of packages that are obsoleted */
		pk_updates_add_description_item (_("Obsoletes"), obsoletes_pretty, NULL);
		g_free (obsoletes_pretty);
	}

	ident = pk_package_id_new_from_string (package_id);
	/* translators: this is the repository the package has come from */
	pk_updates_add_description_item (_("Repository"), ident->data, NULL);

	if (!pk_strzero (update_text)) {
		/* translators: this is the package description */
		pk_updates_add_description_item (_("Description"), update_text, NULL);
	}

	/* add all the links */
	if (!pk_strzero (vendor_url)) {
		/* translators: this is a list of vendor URLs */
		pk_updates_add_description_link_item (_("Vendor"), vendor_url);
	}
	if (!pk_strzero (bugzilla_url)) {
		/* translators: this is a list of bugzilla URLs */
		pk_updates_add_description_link_item (_("Bugzilla"), bugzilla_url);
	}
	if (!pk_strzero (cve_url)) {
		/* translators: this is a list of CVE (security) URLs */
		pk_updates_add_description_link_item (_("CVE"), cve_url);
	}

	/* reboot */
	if (restart == PK_RESTART_ENUM_SESSION ||
	    restart == PK_RESTART_ENUM_SYSTEM) {
		widget = glade_xml_get_widget (glade_xml, "hbox_reboot");
		gtk_widget_show (widget);
	} else {
		widget = glade_xml_get_widget (glade_xml, "hbox_reboot");
		gtk_widget_hide (widget);
	}
}

/**
 * pk_updates_status_changed_cb:
 **/
static void
pk_updates_status_changed_cb (PkClient *client, PkStatusEnum status, gpointer data)
{
	GtkWidget *widget;
	gchar *text;

	widget = glade_xml_get_widget (glade_xml, "progress_part_label");
	text = g_strdup_printf ("<b>%s</b>", pk_status_enum_to_localised_text (status));
	gtk_label_set_markup (GTK_LABEL (widget), text);
	g_free (text);
}

/**
 * pk_window_delete_event_cb:
 * @event: The event type, unused.
 **/
static gboolean
pk_window_delete_event_cb (GtkWidget	*widget,
			    GdkEvent	*event,
			    gpointer    data)
{
	GMainLoop *loop = (GMainLoop *) data;

	/* we might have a transaction running */
	pk_client_cancel (client, NULL);

	g_main_loop_quit (loop);
	return FALSE;
}

/**
 * pk_treeview_update_toggled:
 **/
static void
pk_treeview_update_toggled (GtkCellRendererToggle *cell, gchar *path_str, gpointer data)
{
	GtkTreeModel *model = (GtkTreeModel *) data;
	GtkTreeIter iter;
	GtkTreePath *path = gtk_tree_path_new_from_string (path_str);
	gboolean update;
	gchar *package_id;

	/* get toggled iter */
	gtk_tree_model_get_iter (model, &iter, path);
	gtk_tree_model_get (model, &iter, PACKAGES_COLUMN_SELECT, &update,
			    PACKAGES_COLUMN_ID, &package_id, -1);

	/* unstage */
	update ^= 1;

	pk_debug ("update %s[%i]", package_id, update);
	g_free (package_id);

	/* set new value */
	gtk_list_store_set (GTK_LIST_STORE (model), &iter, PACKAGES_COLUMN_SELECT, update, -1);

	/* clean up */
	gtk_tree_path_free (path);
}

/**
 * pk_treeview_add_columns:
 **/
static void
pk_treeview_add_columns (GtkTreeView *treeview)
{
	GtkCellRenderer *renderer;
	GtkTreeViewColumn *column;

	/* image */
	renderer = gtk_cell_renderer_pixbuf_new ();
	g_object_set (renderer, "stock-size", GTK_ICON_SIZE_DIALOG, NULL);
	column = gtk_tree_view_column_new_with_attributes (_("Severity"), renderer,
							   "icon-name", PACKAGES_COLUMN_ICON, NULL);
	gtk_tree_view_column_set_sort_column_id (column, PACKAGES_COLUMN_INFO);
	gtk_tree_view_append_column (treeview, column);

	/* column for text */
	renderer = gtk_cell_renderer_text_new ();
	column = gtk_tree_view_column_new_with_attributes (_("Software"), renderer,
							   "markup", PACKAGES_COLUMN_TEXT, NULL);
	gtk_tree_view_column_set_sort_column_id (column, PACKAGES_COLUMN_TEXT);
	gtk_tree_view_append_column (treeview, column);
}

/**
 * pk_treeview_renderer_clicked:
 **/
static void
pk_treeview_renderer_clicked (GtkCellRendererToggle *cell, gchar *uri, gpointer data)
{
	pk_debug ("clicked %s", uri);
	pk_execute_url (uri);
}

/**
 * pk_treeview_add_columns_description:
 **/
static void
pk_treeview_add_columns_description (GtkTreeView *treeview)
{
	GtkCellRenderer *renderer;
	GtkTreeViewColumn *column;

	/* image */
	renderer = gtk_cell_renderer_text_new ();
	column = gtk_tree_view_column_new_with_attributes (_("Title"), renderer,
							   "markup", DESC_COLUMN_TITLE, NULL);
	gtk_tree_view_append_column (treeview, column);

	/* column for uris */
	renderer = pk_cell_renderer_uri_new ();
	g_signal_connect (renderer, "clicked", G_CALLBACK (pk_treeview_renderer_clicked), NULL);
	column = gtk_tree_view_column_new_with_attributes (_("Text"), renderer,
							   "text", DESC_COLUMN_TEXT,
							   "uri", DESC_COLUMN_URI, NULL);
	gtk_tree_view_append_column (treeview, column);
}

/**
 * pk_treeview_add_columns_update:
 **/
static void
pk_treeview_add_columns_update (GtkTreeView *treeview)
{
	GtkCellRenderer *renderer;
	GtkTreeViewColumn *column;
	GtkTreeModel *model;

	/* column for select toggle */
	renderer = gtk_cell_renderer_toggle_new ();
	model = gtk_tree_view_get_model (treeview);
	g_signal_connect (renderer, "toggled", G_CALLBACK (pk_treeview_update_toggled), model);
	column = gtk_tree_view_column_new_with_attributes ("Update", renderer, "active", PACKAGES_COLUMN_SELECT, NULL);

	/* set this column to a fixed sizing (of 50 pixels) */
	gtk_tree_view_column_set_sizing (GTK_TREE_VIEW_COLUMN (column), GTK_TREE_VIEW_COLUMN_FIXED);
	gtk_tree_view_column_set_fixed_width (GTK_TREE_VIEW_COLUMN (column), 20);
	gtk_tree_view_append_column (treeview, column);

	/* usual suspects */
	pk_treeview_add_columns (treeview);
}

/**
 * pk_packages_treeview_clicked_cb:
 **/
static void
pk_packages_treeview_clicked_cb (GtkTreeSelection *selection, gpointer data)
{
	GtkTreeModel *model;
	GtkTreeIter iter;
	gchar *package_id;
	GtkWidget *widget;

	/* hide the widgets until we have data */
	widget = glade_xml_get_widget (glade_xml, "scrolledwindow_description");
	gtk_widget_hide (widget);
	widget = glade_xml_get_widget (glade_xml, "hbox_reboot");
	gtk_widget_hide (widget);

	/* This will only work in single or browse selection mode! */
	if (gtk_tree_selection_get_selected (selection, &model, &iter)) {
		g_free (package);
		gtk_tree_model_get (model, &iter,
				    PACKAGES_COLUMN_ID, &package_id, -1);

		/* make back into package ID */
		package = g_strdup (package_id);
		g_free (package_id);
		pk_debug ("selected row is: %s", package);
		/* get the decription */
		pk_client_reset (client, NULL);
		pk_client_get_update_detail (client, package, NULL);
	} else {
		pk_debug ("no row selected");
	}
}

/**
 * pk_connection_changed_cb:
 **/
static void
pk_connection_changed_cb (PkConnection *pconnection, gboolean connected, gpointer data)
{
	pk_debug ("connected=%i", connected);
}

/**
 * pk_updates_add_preview_item:
 **/
static void
pk_updates_add_preview_item (PkClient *client, const gchar *icon, const gchar *message, gboolean clear)
{
	GtkWidget *tree_view;
	GtkTreeSelection *selection;
	GtkTreeIter iter;
	gchar *markup;

	/* clear existing list */
	if (clear == TRUE) {
		gtk_list_store_clear (list_store_preview);
	}

	markup = g_strdup_printf ("<b>%s</b>", message);
	gtk_list_store_append (list_store_preview, &iter);
	gtk_list_store_set (list_store_preview, &iter,
			    PACKAGES_COLUMN_TEXT, markup,
			    PACKAGES_COLUMN_ICON, icon,
			    -1);
	g_free (markup);

	tree_view = glade_xml_get_widget (glade_xml, "treeview_preview");
	selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (tree_view));
	gtk_tree_selection_set_mode (selection, GTK_SELECTION_NONE);
}

/**
 * pk_update_get_approx_time:
 **/
static const gchar *
pk_update_get_approx_time (guint time)
{
	if (time < 60) {
		return _("Less than a minute");
	} else if (time < 60*60) {
		return _("Less than an hour");
	} else if (time < 24*60*60) {
		return _("A few hours");
	} else if (time < 7*24*60*60) {
		return _("A few days");
	}
	return _("Over a week");
}

/**
 * pk_update_update_last_refreshed_time:
 **/
static gboolean
pk_update_update_last_refreshed_time (PkClient *client)
{
	GtkWidget *widget;
	guint time;
	const gchar *time_text;

	/* get times from the daemon */
	pk_client_get_time_since_action (client, PK_ROLE_ENUM_REFRESH_CACHE, &time, NULL);
	time_text = pk_update_get_approx_time (time);
	widget = glade_xml_get_widget (glade_xml, "label_last_refresh");
	gtk_label_set_label (GTK_LABEL (widget), time_text);
	return TRUE;
}

/**
 * pk_update_update_last_updated_time:
 **/
static gboolean
pk_update_update_last_updated_time (PkClient *client)
{
	GtkWidget *widget;
	guint time;
	guint time_new;
	const gchar *time_text;

	/* get times from the daemon */
	pk_client_get_time_since_action (client, PK_ROLE_ENUM_UPDATE_SYSTEM, &time, NULL);
	pk_client_get_time_since_action (client, PK_ROLE_ENUM_UPDATE_PACKAGES, &time_new, NULL);

	/* always use the shortest time */
	if (time_new < time) {
		time = time_new;
	}
	time_text = pk_update_get_approx_time (time);
	widget = glade_xml_get_widget (glade_xml, "label_last_update");
	gtk_label_set_label (GTK_LABEL (widget), time_text);
	return TRUE;
}

/**
 * pk_updates_finished_cb:
 **/
static void
pk_updates_finished_cb (PkClient *client, PkExitEnum exit, guint runtime, gpointer data)
{
	GtkWidget *widget;
	PkRoleEnum role;
	guint length;
	PkRestartEnum restart;

	pk_client_get_role (client, &role, NULL, NULL);

	/* just update the preview page */
	if (role == PK_ROLE_ENUM_REFRESH_CACHE) {
		/* update last time in the UI */
		pk_update_update_last_refreshed_time (client);
	} else if (role == PK_ROLE_ENUM_UPDATE_SYSTEM || role == PK_ROLE_ENUM_UPDATE_PACKAGES) {
		pk_update_update_last_updated_time (client);
	}

	if (role == PK_ROLE_ENUM_REFRESH_CACHE) {
		pk_client_reset (client, NULL);
		pk_client_set_use_buffer (client, TRUE, NULL);
		pk_client_get_updates (client, "basename", NULL);
		return;
	}

	/* we don't need to do anything here */
	if (role == PK_ROLE_ENUM_GET_UPDATE_DETAIL) {
		return;
	}

	pk_updates_preview_animation_stop ();

	/* make the refresh button clickable now we have completed */
	widget = glade_xml_get_widget (glade_xml, "button_apply");
	gtk_widget_set_sensitive (widget, TRUE);
	widget = glade_xml_get_widget (glade_xml, "button_apply2");
	gtk_widget_set_sensitive (widget, TRUE);
	widget = glade_xml_get_widget (glade_xml, "button_review");
	gtk_widget_set_sensitive (widget, TRUE);
	widget = glade_xml_get_widget (glade_xml, "button_refresh");
	gtk_widget_set_sensitive (widget, TRUE);

	/* hide the cancel */
	if (role == PK_ROLE_ENUM_UPDATE_SYSTEM) {
		widget = glade_xml_get_widget (glade_xml, "button_cancel");
		gtk_widget_hide (widget);

		/* go onto the success page */
		if (exit == PK_EXIT_ENUM_SUCCESS) {

			/* do we have to show any widgets? */
			restart = pk_client_get_require_restart (client);
			if (restart == PK_RESTART_ENUM_SYSTEM ||
			    restart == PK_RESTART_ENUM_SESSION) {
				pk_debug ("showing reboot widgets");
				widget = glade_xml_get_widget (glade_xml, "hbox_restart");
				gtk_widget_show (widget);
				widget = glade_xml_get_widget (glade_xml, "button_restart");
				gtk_widget_show (widget);
			}

			/* set correct view */
			pk_updates_set_page (PAGE_CONFIRM);
		}
	}

	/* we don't need to do anything here */
	if (role == PK_ROLE_ENUM_UPDATE_PACKAGES) {
		/* clear existing list */
		gtk_list_store_clear (list_store_details);

		/* get the new update list */
		pk_client_reset (client, NULL);
		pk_client_set_use_buffer (client, TRUE, NULL);
		pk_client_get_updates (client, "basename", NULL);
		return;
	}

	/* clear existing lists */
	gtk_list_store_clear (list_store_preview);

	length = pk_client_package_buffer_get_size (client);
	if (length == 0) {
		/* put a message in the listbox */
		pk_updates_add_preview_item (client, "dialog-information", _("There are no updates available!"), TRUE);

		/* if no updates then hide apply */
		widget = glade_xml_get_widget (glade_xml, "button_review");
		gtk_widget_hide (widget);
		widget = glade_xml_get_widget (glade_xml, "button_apply2");
		gtk_widget_hide (widget);
	} else {

		PkPackageItem *item;
		guint i;
		guint num_low = 0;
		guint num_normal = 0;
		guint num_important = 0;
		guint num_security = 0;
		guint num_bugfix = 0;
		guint num_enhancement = 0;
		const gchar *icon;
		gchar *text;

		for (i=0;i<length;i++) {
			item = pk_client_package_buffer_get_item (client, i);
			if (item->info == PK_INFO_ENUM_LOW) {
				num_low++;
			} else if (item->info == PK_INFO_ENUM_IMPORTANT) {
				num_important++;
			} else if (item->info == PK_INFO_ENUM_SECURITY) {
				num_security++;
			} else if (item->info == PK_INFO_ENUM_BUGFIX) {
				num_bugfix++;
			} else if (item->info == PK_INFO_ENUM_ENHANCEMENT) {
				num_enhancement++;
			} else {
				num_normal++;
			}
		}

		/* clear existing list */
		gtk_list_store_clear (list_store_preview);

		/* add to preview box in order of priority */
		if (num_security > 0) {
			icon = pk_info_enum_to_icon_name (PK_INFO_ENUM_SECURITY);
			text = pk_update_enum_to_localised_text (PK_INFO_ENUM_SECURITY, num_security);
			pk_updates_add_preview_item (client, icon, text, FALSE);
			g_free (text);
		}
		if (num_important > 0) {
			icon = pk_info_enum_to_icon_name (PK_INFO_ENUM_IMPORTANT);
			text = pk_update_enum_to_localised_text (PK_INFO_ENUM_IMPORTANT, num_important);
			pk_updates_add_preview_item (client, icon, text, FALSE);
			g_free (text);
		}
		if (num_bugfix > 0) {
			icon = pk_info_enum_to_icon_name (PK_INFO_ENUM_BUGFIX);
			text = pk_update_enum_to_localised_text (PK_INFO_ENUM_BUGFIX, num_bugfix);
			pk_updates_add_preview_item (client, icon, text, FALSE);
			g_free (text);
		}
		if (num_enhancement > 0) {
			icon = pk_info_enum_to_icon_name (PK_INFO_ENUM_ENHANCEMENT);
			text = pk_update_enum_to_localised_text (PK_INFO_ENUM_ENHANCEMENT, num_enhancement);
			pk_updates_add_preview_item (client, icon, text, FALSE);
			g_free (text);
		}
		if (num_low > 0) {
			icon = pk_info_enum_to_icon_name (PK_INFO_ENUM_LOW);
			text = pk_update_enum_to_localised_text (PK_INFO_ENUM_LOW, num_low);
			pk_updates_add_preview_item (client, icon, text, FALSE);
			g_free (text);
		}
		if (num_normal > 0) {
			icon = pk_info_enum_to_icon_name (PK_INFO_ENUM_NORMAL);
			text = pk_update_enum_to_localised_text (PK_INFO_ENUM_NORMAL, num_normal);
			pk_updates_add_preview_item (client, icon, text, FALSE);
			g_free (text);
		}

		/* set visible and sensitive */
		widget = glade_xml_get_widget (glade_xml, "button_review");
		gtk_widget_show (widget);
		widget = glade_xml_get_widget (glade_xml, "button_apply2");
		gtk_widget_show (widget);
	}
}

/**
 * pk_updates_progress_changed_cb:
 **/
static void
pk_updates_progress_changed_cb (PkClient *client, guint percentage, guint subpercentage,
				guint elapsed, guint remaining, gpointer data)
{
	GtkWidget *widget;
	widget = glade_xml_get_widget (glade_xml, "progressbar_percent");

	if (subpercentage != PK_CLIENT_PERCENTAGE_INVALID) {
		gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (widget), (gfloat) subpercentage / 100.0);
	}
}

/**
 * pk_updates_allow_cancel_cb:
 **/
static void
pk_updates_allow_cancel_cb (PkClient *client, gboolean allow_cancel, gpointer data)
{
	GtkWidget *widget;
	widget = glade_xml_get_widget (glade_xml, "button_cancel");
	gtk_widget_set_sensitive (widget, allow_cancel);
}

/**
 * pk_updates_task_list_changed_cb:
 **/
static void
pk_updates_task_list_changed_cb (PkTaskList *tlist, gpointer data)
{
	GtkWidget *widget;

	/* hide buttons if we are updating */
	if (pk_task_list_contains_role (tlist, PK_ROLE_ENUM_UPDATE_SYSTEM) == TRUE) {
		/* clear existing list */
		gtk_list_store_clear (list_store_preview);

		/* put a message in the listbox */
		pk_updates_add_preview_item (client, "dialog-information", _("There is an update already in progress!"), TRUE);

		/* if doing it then hide apply and refresh */
		widget = glade_xml_get_widget (glade_xml, "button_apply");
		gtk_widget_hide (widget);
		widget = glade_xml_get_widget (glade_xml, "button_refresh");
		gtk_widget_hide (widget);
	}
}

/**
 * pk_updates_error_code_cb:
 **/
static void
pk_updates_error_code_cb (PkClient *client, PkErrorCodeEnum code, const gchar *details, gpointer data)
{
	GtkWidget *widget;
	const gchar *title;
	gchar *title_bold;
	gchar *details_safe;

	/* set correct view */
	pk_updates_set_page (PAGE_ERROR);

	/* set bold title */
	widget = glade_xml_get_widget (glade_xml, "label_error_title");
	title = pk_error_enum_to_localised_text (code);
	title_bold = g_strdup_printf ("<b>%s</b>", title);
	gtk_label_set_label (GTK_LABEL (widget), title_bold);
	g_free (title_bold);

	widget = glade_xml_get_widget (glade_xml, "label_error_message");
	gtk_label_set_label (GTK_LABEL (widget), pk_error_enum_to_localised_message (code));

	widget = glade_xml_get_widget (glade_xml, "label_error_details");
	details_safe = g_markup_escape_text (details, -1);
	gtk_label_set_label (GTK_LABEL (widget), details_safe);
}

/**
 * main:
 **/
int
main (int argc, char *argv[])
{
	GMainLoop *loop;
	gboolean verbose = FALSE;
	gboolean program_version = FALSE;
	GOptionContext *context;
	GtkWidget *main_window;
	GtkWidget *widget;
	GtkTreeSelection *selection;
	PkConnection *pconnection;
	PkEnumList *role_list;
	PkRoleEnum role;

	const GOptionEntry options[] = {
		{ "verbose", 'v', 0, G_OPTION_ARG_NONE, &verbose,
		  N_("Show extra debugging information"), NULL },
		{ "version", '\0', 0, G_OPTION_ARG_NONE, &program_version,
		  N_("Show the program version and exit"), NULL },
		{ NULL}
	};

	setlocale (LC_ALL, "");

	bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
	bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
	textdomain (GETTEXT_PACKAGE);

	if (! g_thread_supported ()) {
		g_thread_init (NULL);
	}
	dbus_g_thread_init ();
	g_type_init ();

	context = g_option_context_new (NULL);
	g_option_context_set_summary (context, _("Software Update Viewer"));
	g_option_context_add_main_entries (context, options, NULL);
	g_option_context_parse (context, &argc, &argv, NULL);
	g_option_context_free (context);

	if (program_version == TRUE) {
		g_print (VERSION "\n");
		return 0;
	}

	pk_debug_init (verbose);
	gtk_init (&argc, &argv);

	/* add application specific icons to search path */
	gtk_icon_theme_append_search_path (gtk_icon_theme_get_default (),
					   PK_DATA G_DIR_SEPARATOR_S "icons");

	loop = g_main_loop_new (NULL, FALSE);

	client = pk_client_new ();
	pk_client_set_use_buffer (client, TRUE, NULL);
	g_signal_connect (client, "package",
			  G_CALLBACK (pk_updates_package_cb), NULL);
	g_signal_connect (client, "finished",
			  G_CALLBACK (pk_updates_finished_cb), NULL);
	g_signal_connect (client, "progress-changed",
			  G_CALLBACK (pk_updates_progress_changed_cb), NULL);
	g_signal_connect (client, "update-detail",
			  G_CALLBACK (pk_updates_update_detail_cb), NULL);
	g_signal_connect (client, "status-changed",
			  G_CALLBACK (pk_updates_status_changed_cb), NULL);
	g_signal_connect (client, "error-code",
			  G_CALLBACK (pk_updates_error_code_cb), NULL);
	g_signal_connect (client, "allow-cancel",
			  G_CALLBACK (pk_updates_allow_cancel_cb), NULL);

	/* get actions */
	role_list = pk_client_get_actions (client);

	pconnection = pk_connection_new ();
	g_signal_connect (pconnection, "connection-changed",
			  G_CALLBACK (pk_connection_changed_cb), NULL);

	/* we need to grey out all the buttons if we are in progress */
	tlist = pk_task_list_new ();
	g_signal_connect (tlist, "task-list-changed",
			  G_CALLBACK (pk_updates_task_list_changed_cb), NULL);
	pk_updates_task_list_changed_cb (tlist, NULL);

	glade_xml = glade_xml_new (PK_DATA "/pk-update-viewer.glade", NULL, NULL);
	main_window = glade_xml_get_widget (glade_xml, "window_updates");

	/* Hide window first so that the dialogue resizes itself without redrawing */
	gtk_widget_hide (main_window);

	/* hide until we have updates */
	widget = glade_xml_get_widget (glade_xml, "hbox_reboot");
	gtk_widget_hide (widget);

	/* hide from finished page until we have updates */
	widget = glade_xml_get_widget (glade_xml, "hbox_restart");
	gtk_widget_hide (widget);
	widget = glade_xml_get_widget (glade_xml, "button_restart");
	gtk_widget_hide (widget);

	/* hide until we have reboot notifier */
	widget = glade_xml_get_widget (glade_xml, "button_review");
	gtk_widget_hide (widget);

	/* Get the main window quit */
	g_signal_connect (main_window, "delete_event",
			  G_CALLBACK (pk_window_delete_event_cb), loop);

	widget = glade_xml_get_widget (glade_xml, "button_close");
	g_signal_connect (widget, "clicked",
			  G_CALLBACK (pk_button_close_cb), loop);
	gtk_widget_set_tooltip_text(widget, _("Close without updating"));
	widget = glade_xml_get_widget (glade_xml, "button_close2");
	g_signal_connect (widget, "clicked",
			  G_CALLBACK (pk_button_close_cb), loop);
	widget = glade_xml_get_widget (glade_xml, "button_close3");
	g_signal_connect (widget, "clicked",
			  G_CALLBACK (pk_button_close_cb), loop);
	widget = glade_xml_get_widget (glade_xml, "button_close4");
	g_signal_connect (widget, "clicked",
			  G_CALLBACK (pk_button_close_cb), loop);
	widget = glade_xml_get_widget (glade_xml, "button_close5");
	g_signal_connect (widget, "clicked",
			  G_CALLBACK (pk_button_close_cb), loop);
	widget = glade_xml_get_widget (glade_xml, "button_cancel");
	g_signal_connect (widget, "clicked",
			  G_CALLBACK (pk_button_cancel_cb), loop);
	gtk_widget_set_sensitive (widget, FALSE);

	/* can we ever do the action? */
	if (pk_enum_list_contains (role_list, PK_ROLE_ENUM_CANCEL) == FALSE) {
		gtk_widget_hide (widget);
	}

	widget = glade_xml_get_widget (glade_xml, "button_review");
	g_signal_connect (widget, "clicked",
			  G_CALLBACK (pk_button_review_cb), loop);
	gtk_widget_set_tooltip_text(widget, _("Review the update list"));

	widget = glade_xml_get_widget (glade_xml, "button_overview");
	g_signal_connect (widget, "clicked",
			  G_CALLBACK (pk_button_overview_cb), loop);
	gtk_widget_set_tooltip_text(widget, _("Back to overview"));

	widget = glade_xml_get_widget (glade_xml, "button_apply");
	g_signal_connect (widget, "clicked",
			  G_CALLBACK (pk_updates_apply_cb), loop);
	gtk_widget_set_tooltip_text(widget, _("Apply all updates"));
	gtk_widget_set_sensitive (widget, FALSE);

	widget = glade_xml_get_widget (glade_xml, "button_apply2");
	g_signal_connect (widget, "clicked",
			  G_CALLBACK (pk_updates_apply_cb), loop);
	gtk_widget_set_tooltip_text(widget, _("Apply all updates"));
	gtk_widget_set_sensitive (widget, FALSE);

	widget = glade_xml_get_widget (glade_xml, "button_refresh");
	g_signal_connect (widget, "clicked",
			  G_CALLBACK (pk_updates_refresh_cb), NULL);
	gtk_widget_set_tooltip_text(widget, _("Refreshing is not normally required but will retrieve the latest application and update lists"));
	widget = glade_xml_get_widget (glade_xml, "button_help");
	g_signal_connect (widget, "clicked",
			  G_CALLBACK (pk_button_help_cb), NULL);
	/* we have no yelp file yet */
	gtk_widget_hide (widget);

	/* create list stores */
	list_store_details = gtk_list_store_new (PACKAGES_COLUMN_LAST, G_TYPE_STRING,
						 G_TYPE_STRING, G_TYPE_STRING, G_TYPE_INT, G_TYPE_BOOLEAN);
	list_store_preview = gtk_list_store_new (PREVIEW_COLUMN_LAST, G_TYPE_STRING, G_TYPE_STRING, GDK_TYPE_PIXBUF);
	list_store_description = gtk_list_store_new (DESC_COLUMN_LAST, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);

	/* sorted */
	gtk_tree_sortable_set_sort_column_id (GTK_TREE_SORTABLE (list_store_details),
					      PACKAGES_COLUMN_TEXT, GTK_SORT_ASCENDING);

	/* create preview tree view */
	widget = glade_xml_get_widget (glade_xml, "treeview_preview");
	gtk_tree_view_set_model (GTK_TREE_VIEW (widget),
				 GTK_TREE_MODEL (list_store_preview));

	/* add columns to the tree view */
	pk_treeview_add_columns (GTK_TREE_VIEW (widget));
	gtk_tree_view_columns_autosize (GTK_TREE_VIEW (widget));

	/* create description tree view */
	widget = glade_xml_get_widget (glade_xml, "treeview_description");
	gtk_tree_view_set_model (GTK_TREE_VIEW (widget),
				 GTK_TREE_MODEL (list_store_description));

	/* add columns to the tree view */
	pk_treeview_add_columns_description (GTK_TREE_VIEW (widget));
	gtk_tree_view_columns_autosize (GTK_TREE_VIEW (widget));

	/* create package tree view */
	widget = glade_xml_get_widget (glade_xml, "treeview_updates");
	gtk_tree_view_set_model (GTK_TREE_VIEW (widget),
				 GTK_TREE_MODEL (list_store_details));

	selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (widget));
	gtk_tree_selection_set_mode (selection, GTK_SELECTION_BROWSE);
	g_signal_connect (selection, "changed",
			  G_CALLBACK (pk_packages_treeview_clicked_cb), NULL);

	/* add columns to the tree view */
	pk_treeview_add_columns_update (GTK_TREE_VIEW (widget));
	gtk_tree_view_columns_autosize (GTK_TREE_VIEW (widget));

	/* make the refresh button non-clickable until we get completion */
	widget = glade_xml_get_widget (glade_xml, "button_refresh");
	gtk_widget_set_sensitive (widget, FALSE);

	pk_updates_preview_animation_start ();

	/* set the last updated text */
	pk_update_update_last_refreshed_time (client);
	pk_update_update_last_updated_time (client);

	/* get the update list */
	pk_client_get_updates (client, "basename", NULL);
	gtk_widget_show (main_window);

	g_main_loop_run (loop);
	g_main_loop_unref (loop);

	/* we might have visual stuff running, close it down */
	pk_client_get_role (client, &role, NULL, NULL);
	if (role == PK_ROLE_ENUM_GET_UPDATES ||
	    role == PK_ROLE_ENUM_GET_UPDATE_DETAIL) {
		pk_client_cancel (client, NULL);
	}

	g_object_unref (glade_xml);
	g_object_unref (list_store_preview);
	g_object_unref (list_store_description);
	g_object_unref (list_store_details);
	g_object_unref (client);
	g_object_unref (pconnection);
	g_object_unref (role_list);
	g_free (package);

	return 0;
}
