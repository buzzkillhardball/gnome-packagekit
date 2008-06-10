/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2007-2008 Richard Hughes <richard@hughsie.com>
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
#include <gconf/gconf-client.h>
#include <libsexy/sexy-icon-entry.h>
#include <math.h>
#include <string.h>
#include <locale.h>
#include <polkit-gnome/polkit-gnome.h>

#include <pk-debug.h>
#include <pk-enum.h>
#include <pk-client.h>
#include <pk-control.h>
#include <pk-common.h>
#include <pk-connection.h>
#include <pk-package-id.h>
#include <pk-package-list.h>
#include <pk-extra.h>

#include <gpk-client.h>
#include <gpk-common.h>
#include <gpk-gnome.h>
#include <gpk-error.h>

#include "gpk-application.h"
#include "gpk-application-state.h"
#include "gpk-animated-icon.h"
#include "gpk-client-run.h"
#include "gpk-client-chooser.h"

static void     gpk_application_class_init (GpkApplicationClass *klass);
static void     gpk_application_init       (GpkApplication      *application);
static void     gpk_application_finalize   (GObject	    *object);

#define GPK_APPLICATION_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), GPK_TYPE_APPLICATION, GpkApplicationPrivate))
#define PK_STOCK_APP_ICON		"system-software-installer"

typedef enum {
	PK_SEARCH_NAME,
	PK_SEARCH_DETAILS,
	PK_SEARCH_FILE,
	PK_SEARCH_UNKNOWN
} PkSearchType;

typedef enum {
	PK_MODE_NAME_DETAILS_FILE,
	PK_MODE_GROUP,
	PK_MODE_ALL_PACKAGES,
	PK_MODE_UNKNOWN
} PkSearchMode;

typedef enum {
	PK_ACTION_NONE,
	PK_ACTION_INSTALL,
	PK_ACTION_REMOVE,
	PK_ACTION_UNKNOWN
} PkActionMode;

struct GpkApplicationPrivate
{
	GladeXML		*glade_xml;
	GConfClient		*gconf_client;
	GtkListStore		*packages_store;
	GtkListStore		*groups_store;
	PkControl		*control;
	PkClient		*client_search;
	PkClient		*client_action;
	PkClient		*client_details;
	PkClient		*client_files;
	GpkClient		*gclient;
	PkConnection		*pconnection;
	PkExtra			*extra;
	gchar			*package;
	gchar			*group;
	gchar			*url;
	GHashTable		*repos;
	PkRoleEnum		 roles;
	PkFilterEnum		 filters;
	PkGroupEnum		 groups;
	PkFilterEnum		 filters_current;
	gboolean		 has_package; /* if we got a package in the search */
	PkSearchType		 search_type;
	PkSearchMode		 search_mode;
	PkActionMode		 action;
	GPtrArray		*package_list;
};

enum {
	ACTION_CLOSE,
	LAST_SIGNAL
};

enum
{
	PACKAGES_COLUMN_IMAGE,
	PACKAGES_COLUMN_STATE,  /* state of the  */
	PACKAGES_COLUMN_CHECKBOX,  /* what we show in the checkbox */
	PACKAGES_COLUMN_CHECKBOX_ENABLE, /* sensitive */
	PACKAGES_COLUMN_TEXT,
	PACKAGES_COLUMN_ID,
	PACKAGES_COLUMN_LAST
};

enum
{
	GROUPS_COLUMN_ICON,
	GROUPS_COLUMN_NAME,
	GROUPS_COLUMN_ID,
	GROUPS_COLUMN_LAST
};

static guint	     signals [LAST_SIGNAL] = { 0 };

G_DEFINE_TYPE (GpkApplication, gpk_application, G_TYPE_OBJECT)

static gboolean gpk_application_refresh_search_results (GpkApplication *application);

/**
 * gpk_application_class_init:
 * @klass: This graph class instance
 **/
static void
gpk_application_class_init (GpkApplicationClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	object_class->finalize = gpk_application_finalize;
	g_type_class_add_private (klass, sizeof (GpkApplicationPrivate));

	signals [ACTION_CLOSE] =
		g_signal_new ("action-close",
			      G_TYPE_FROM_CLASS (object_class),
			      G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (GpkApplicationClass, action_close),
			      NULL,
			      NULL,
			      g_cclosure_marshal_VOID__VOID,
			      G_TYPE_NONE, 0);
}

/**
 * gpk_application_show:
 **/
void
gpk_application_show (GpkApplication *application)
{
	GtkWidget *widget;
	widget = glade_xml_get_widget (application->priv->glade_xml, "window_manager");
	gtk_window_present (GTK_WINDOW (widget));
}

/**
 * gpk_application_set_find_cancel_buttons:
 **/
static void
gpk_application_set_find_cancel_buttons (GpkApplication *application, gboolean find)
{
	GtkWidget *widget;

	/* if we can't do it, then just make the button insensitive */
	if (!pk_enums_contain (application->priv->roles, PK_ROLE_ENUM_CANCEL)) {
		widget = glade_xml_get_widget (application->priv->glade_xml, "button_cancel");
		gtk_widget_set_sensitive (widget, FALSE);
	}

	/* which tab to enable? */
	widget = glade_xml_get_widget (application->priv->glade_xml, "notebook_search_cancel");
	if (find) {
		gtk_notebook_set_current_page (GTK_NOTEBOOK (widget), 0);
	} else {
		gtk_notebook_set_current_page (GTK_NOTEBOOK (widget), 1);
	}
}

/**
 * gpk_application_treeview_sort_none:
 **/
static gint
gpk_application_treeview_sort_none (GtkTreeModel *model, GtkTreeIter *a, GtkTreeIter *b, gpointer user_data)
{
	return 0;
}

/**
 * gpk_application_treeview_sort_text:
 **/
static gint
gpk_application_treeview_sort_text (GtkTreeModel *model, GtkTreeIter *a, GtkTreeIter *b, gpointer user_data)
{
	gchar *a_txt;
	gchar *b_txt;
	gint ret;
	gtk_tree_model_get (model, a, PACKAGES_COLUMN_ID, &a_txt, -1);
	gtk_tree_model_get (model, b, PACKAGES_COLUMN_ID, &b_txt, -1);
	ret = strcmp (a_txt, b_txt);
	g_free (a_txt);
	g_free (b_txt);
	return ret;
}

/**
 * gpk_application_treeview_set_sorted:
 **/
static void
gpk_application_treeview_set_sorted (GpkApplication *application, gboolean sorted)
{
	if (sorted) {
		pk_debug ("sorted");
		gtk_tree_sortable_set_sort_func (GTK_TREE_SORTABLE (application->priv->packages_store),
						 PACKAGES_COLUMN_ID, gpk_application_treeview_sort_text, NULL, NULL);
		gtk_tree_sortable_set_sort_column_id (GTK_TREE_SORTABLE (application->priv->packages_store),
						      PACKAGES_COLUMN_ID, GTK_SORT_ASCENDING);
	} else {
		pk_debug ("unsorted");
		gtk_tree_sortable_set_default_sort_func (GTK_TREE_SORTABLE (application->priv->packages_store),
							 gpk_application_treeview_sort_none, NULL, NULL);
		gtk_tree_sortable_set_sort_column_id (GTK_TREE_SORTABLE (application->priv->packages_store),
						      GTK_TREE_SORTABLE_DEFAULT_SORT_COLUMN_ID, GTK_SORT_ASCENDING);
	}
}

/**
 * gpk_application_set_text_buffer:
 **/
static void
gpk_application_set_text_buffer (GtkWidget *widget, const gchar *text)
{
	GtkTextBuffer *buffer;
	buffer = gtk_text_buffer_new (NULL);
	/* ITS4: ignore, not used for allocation */
	if (pk_strzero (text) == FALSE) {
		gtk_text_buffer_set_text (buffer, text, -1);
	} else {
		/* no information */
		gtk_text_buffer_set_text (buffer, "", -1);
	}
	gtk_text_view_set_buffer (GTK_TEXT_VIEW (widget), buffer);
}

/**
 * gpk_application_menu_homepage_cb:
 **/
static void
gpk_application_menu_homepage_cb (GtkAction *action, GpkApplication *application)
{
	g_return_if_fail (PK_IS_APPLICATION (application));
	gpk_gnome_open (application->priv->url);
}

/**
 * gpk_application_modal_info:
 **/
static void
gpk_application_modal_info (GpkApplication *application, const gchar *title, const gchar *text)
{
	GtkWidget *dialog;
	GtkWidget *widget;
	GtkWidget *main_window;
	GtkWidget *scrolled_window;

	g_return_if_fail (PK_IS_APPLICATION (application));

	widget = gtk_text_view_new ();
	scrolled_window = gtk_scrolled_window_new (NULL, NULL);
	gtk_widget_set_size_request (scrolled_window, 400, 200);
	gtk_scrolled_window_add_with_viewport (GTK_SCROLLED_WINDOW (scrolled_window), widget);

	main_window = glade_xml_get_widget (application->priv->glade_xml, "window_manager");
	dialog = gtk_dialog_new_with_buttons (title, GTK_WINDOW (main_window),
					      GTK_DIALOG_DESTROY_WITH_PARENT, GTK_STOCK_OK,
					      GTK_RESPONSE_NONE, NULL);

	gpk_application_set_text_buffer (widget, text);
	gtk_container_add (GTK_CONTAINER (GTK_DIALOG (dialog)->vbox), scrolled_window);

	/* ensure that the dialog box is destroyed when the user responds. */
	g_signal_connect_swapped (dialog, "response", G_CALLBACK (gtk_widget_destroy), dialog);
	gtk_widget_show_all (dialog);
}

/**
 * gpk_application_menu_files_cb:
 **/
static void
gpk_application_menu_files_cb (GtkAction *action, GpkApplication *application)
{
	GError *error = NULL;
	gchar **files;
	gchar *text;

	g_return_if_fail (PK_IS_APPLICATION (application));

	files = gpk_client_get_file_list (application->priv->gclient, application->priv->package, &error);
	if (files == NULL) {
		pk_warning ("could not get file list: %s", error->message);
		g_error_free (error);
		return;
	}

	/* split and show */
	text = g_strjoinv ("\n", files);

	if (pk_strzero (text)) {
		g_free (text);
		text = g_strdup (_("No files"));
	}

	gpk_application_modal_info (application, _("File list"), text);

	g_free (text);
	g_strfreev (files);
}

static void gpk_application_button_install_cb (GtkWidget *widget_button, GpkApplication *application);
static void gpk_application_button_remove_cb (GtkWidget *widget_button, GpkApplication *application);

/**
 * gpk_application_menu_install_cb:
 **/
static void
gpk_application_menu_install_cb (GtkAction *action, GpkApplication *application)
{
	gpk_application_button_install_cb (NULL, application);
}

/**
 * gpk_application_menu_remove_cb:
 **/
static void
gpk_application_menu_remove_cb (GtkAction *action, GpkApplication *application)
{
	gpk_application_button_remove_cb (NULL, application);
}

/**
 * gpk_application_menu_requires_cb:
 **/
static void
gpk_application_menu_requires_cb (GtkAction *action, GpkApplication *application)
{
	GError *error = NULL;
	gboolean ret;
	PkPackageList *list;
	GtkWidget *widget;

	/* cancel any previous request */
	ret = pk_client_reset (application->priv->client_files, &error);
	if (!ret) {
		pk_warning ("failed to cancel, and adding to queue: %s", error->message);
		g_error_free (error);
		return;
	}

	/* get the requires */
	pk_client_set_synchronous (application->priv->client_files, TRUE, NULL);
	ret = pk_client_get_requires (application->priv->client_files, PK_FILTER_ENUM_NONE,
				      application->priv->package, TRUE, &error);
	pk_client_set_synchronous (application->priv->client_files, FALSE, NULL);

	if (!ret) {
		pk_warning ("failed to get requires: %s", error->message);
		g_error_free (error);
		return;
	}

	list = pk_client_get_package_list (application->priv->client_files);
	widget = glade_xml_get_widget (application->priv->glade_xml, "window_manager");
	gpk_client_chooser_show (GTK_WINDOW (widget), list, PK_ROLE_ENUM_GET_DEPENDS, _("Required by"));

	g_object_unref (list);
}

/**
 * gpk_application_menu_depends_cb:
 **/
static void
gpk_application_menu_depends_cb (GtkAction *action, GpkApplication *application)
{
	GError *error = NULL;
	gboolean ret;
	PkPackageList *list;
	GtkWidget *widget;

	/* cancel any previous request */
	ret = pk_client_reset (application->priv->client_files, &error);
	if (!ret) {
		pk_warning ("failed to cancel, and adding to queue: %s", error->message);
		g_error_free (error);
		return;
	}

	/* get the requires */
	pk_client_set_synchronous (application->priv->client_files, TRUE, NULL);
	ret = pk_client_get_depends (application->priv->client_files, PK_FILTER_ENUM_NONE,
				     application->priv->package, TRUE, &error);
	pk_client_set_synchronous (application->priv->client_files, FALSE, NULL);

	if (!ret) {
		pk_warning ("failed to get depends: %s", error->message);
		g_error_free (error);
		return;
	}

	list = pk_client_get_package_list (application->priv->client_files);
	widget = glade_xml_get_widget (application->priv->glade_xml, "window_manager");
	gpk_client_chooser_show (GTK_WINDOW (widget), list, PK_ROLE_ENUM_GET_DEPENDS, _("Depends on"));

	g_object_unref (list);
}

/**
 * gpk_application_get_full_repo_name:
 **/
static const gchar *
gpk_application_get_full_repo_name (GpkApplication *application, const gchar *data)
{
	const gchar *repo_name;

	/* if no data, we can't look up in the hash table */
	if (pk_strzero (data)) {
		pk_warning ("no ident data");
		return _("Invalid");
	}

	/* try to find in cached repo list */
	repo_name = (const gchar *) g_hash_table_lookup (application->priv->repos, data);
	if (repo_name == NULL) {
		pk_warning ("no repo name, falling back to %s", data);
		return data;
	}
	return repo_name;
}

/**
 * gpk_application_details_cb:
 **/
static void
gpk_application_details_cb (PkClient *client, const gchar *package_id,
			    const gchar *license, PkGroupEnum group,
			    const gchar *detail, const gchar *url,
			    guint64 size, GpkApplication *application)
{
	GtkWidget *widget;
	gchar *text;
	PkPackageId *ident;
	const gchar *repo_name;
	const gchar *icon;
	gboolean valid = FALSE;
	gboolean installed;
	PkInfoEnum info;

	g_return_if_fail (PK_IS_APPLICATION (application));

	ident = pk_package_id_new_from_string (package_id);
	if (ident == NULL) {
		pk_warning ("failed to get PkPackageId for %s", package_id);
		return;
	}
	installed = pk_strequal (ident->data, "installed");

	pk_debug ("details = %s:%i:%s:%s", package_id, group, detail, url);
	widget = glade_xml_get_widget (application->priv->glade_xml, "vbox_description");
	gtk_widget_show (widget);

	/* get the icon */
	icon = pk_extra_get_icon_name (application->priv->extra, ident->name);
	if (icon != NULL) {
		/* check icon actually exists and is valid in this theme */
		valid = gpk_check_icon_valid (icon);
	}

	/* nothing in the detail database or invalid */
	if (valid == FALSE) {
		info = installed ? PK_INFO_ENUM_INSTALLED : PK_INFO_ENUM_AVAILABLE;
		icon = gpk_info_enum_to_icon_name (info);
	}
	widget = glade_xml_get_widget (application->priv->glade_xml, "image_icon");
	gtk_image_set_from_icon_name (GTK_IMAGE (widget), icon, GTK_ICON_SIZE_DIALOG);

	/* homepage button? */
	widget = glade_xml_get_widget (application->priv->glade_xml, "menuitem_homepage");
	if (pk_strzero (url) == FALSE) {
		gtk_widget_show (widget);
		g_free (application->priv->url);
		/* save the url for the button */
		application->priv->url = g_strdup (url);

		/* set the tooltip to where we are going */
		text = g_strdup_printf (_("Visit %s"), url);
		gtk_widget_set_tooltip_text (widget, text);
		g_free (text);
	} else {
		gtk_widget_hide (widget);
	}

	/* set the description */
	text = g_markup_escape_text (detail, -1);
	widget = glade_xml_get_widget (application->priv->glade_xml, "textview_description");
	gpk_application_set_text_buffer (widget, text);
	g_free (text);

	/* if non-zero, set the size */
	if (size > 0) {
		gchar *value;

		/* change the label */
		widget = glade_xml_get_widget (application->priv->glade_xml, "label_filesize_text");
		if (installed) {
			gtk_label_set_label (GTK_LABEL (widget), _("Installed size:"));
		} else {
			gtk_label_set_label (GTK_LABEL (widget), _("Download size:"));
		}

		/* set the size */
		widget = glade_xml_get_widget (application->priv->glade_xml, "label_filesize");
		value = gpk_size_to_si_size_text (size);
		gtk_label_set_label (GTK_LABEL (widget), value);
		g_free (value);

		widget = glade_xml_get_widget (application->priv->glade_xml, "hbox_filesize");
		gtk_widget_show (widget);
	} else {
		widget = glade_xml_get_widget (application->priv->glade_xml, "hbox_filesize");
		gtk_widget_hide (widget);
	}

	/* set the repo text, or hide if installed */
	if (installed) {
		widget = glade_xml_get_widget (application->priv->glade_xml, "hbox_source");
		gtk_widget_hide (widget);
	} else {
		widget = glade_xml_get_widget (application->priv->glade_xml, "hbox_source");
		gtk_widget_show (widget);
		widget = glade_xml_get_widget (application->priv->glade_xml, "label_source");

		/* see if we can get the full name of the repo from the repo_id */
		repo_name = gpk_application_get_full_repo_name (application, ident->data);
		gtk_label_set_label (GTK_LABEL (widget), repo_name);
	}
	pk_package_id_free (ident);
}

static gint
pk_ptr_array_find_string (GPtrArray *array, const gchar *string)
{
	gint i;
	gchar *item;

	g_return_val_if_fail (array != NULL, FALSE);
	g_return_val_if_fail (string != NULL, FALSE);

	for (i=0; i<array->len; i++) {
		item = (gchar *) g_ptr_array_index (array, i);
		if (pk_strequal (string, item)) {
			return i;
		}
	}
	return -1;
}

static gboolean
pk_ptr_array_remove_string (GPtrArray *array, const gchar *string)
{
	guint i;
	gchar *item;
	gboolean ret = FALSE;

	g_return_val_if_fail (array != NULL, FALSE);
	g_return_val_if_fail (string != NULL, FALSE);

	for (i=0; i<array->len; i++) {
		item = (gchar *) g_ptr_array_index (array, i);
		if (pk_strequal (string, item)) {
			g_free (item);
			g_ptr_array_remove_index (array, i);
			ret = TRUE;
		}
	}
	return ret;
}

/**
 * gpk_application_get_checkbox_enable:
 **/
static gboolean
gpk_application_get_checkbox_enable (GpkApplication *application, GpkPackageState state)
{
	gboolean enable_installed = TRUE;
	gboolean enable_available = TRUE;

	if (application->priv->action == PK_ACTION_INSTALL) {
		enable_installed = FALSE;
	} else if (application->priv->action == PK_ACTION_REMOVE) {
		enable_available = FALSE;
	}

	if (gpk_application_state_installed (state)) {
		return enable_installed;
	}
	return enable_available;
}

/**
 * gpk_application_package_cb:
 **/
static void
gpk_application_package_cb (PkClient *client, PkInfoEnum info, const gchar *package_id,
			    const gchar *summary, GpkApplication *application)
{
	GtkTreeIter iter;
	PkPackageId *ident;
	const gchar *summary_new;
	const gchar *icon = NULL;
	gchar *text;
	gint index;
	gboolean in_queue;
	gboolean installed;
	gboolean checkbox;
	gboolean enabled;
	GpkPackageState state = 0;

	g_return_if_fail (PK_IS_APPLICATION (application));

	pk_debug ("package = %s:%s:%s", pk_info_enum_to_text (info), package_id, summary);

	/* ignore progress */
	if (info != PK_INFO_ENUM_INSTALLED && info != PK_INFO_ENUM_AVAILABLE) {
		return;
	}

	/* find localised summary */
	ident = pk_package_id_new_from_string (package_id);
	if (ident == NULL) {
		pk_warning ("failed to get PkPackageId for %s", package_id);
		return;
	}
	summary_new = pk_extra_get_summary (application->priv->extra, ident->name);
	if (summary_new == NULL) {
		/* use the non-localised one */
		summary_new = summary;
	}

	/* mark as got so we don't warn */
	application->priv->has_package = TRUE;

	/* are we in the package list? */
	index = pk_ptr_array_find_string (application->priv->package_list, package_id);
	in_queue = (index != -1);
	installed = (info == PK_INFO_ENUM_INSTALLED);

	if (installed && in_queue) {
		state = GPK_STATE_INSTALLED_TO_BE_REMOVED;
	} else if (installed && !in_queue) {
		state = GPK_STATE_INSTALLED;
	} else if (!installed && in_queue) {
		state = GPK_STATE_AVAILABLE_TO_BE_INSTALLED;
	} else if (!installed && !in_queue) {
		state = GPK_STATE_AVAILABLE;
	}

	icon = gpk_application_state_get_icon (state);
	checkbox = gpk_application_state_get_checkbox (state);

	/* use two lines */
	text = gpk_package_id_format_twoline (package_id, summary);

	/* can we modify this? */
	enabled = gpk_application_get_checkbox_enable (application, state);

	gtk_list_store_append (application->priv->packages_store, &iter);
	gtk_list_store_set (application->priv->packages_store, &iter,
			    PACKAGES_COLUMN_STATE, state,
			    PACKAGES_COLUMN_CHECKBOX, installed ^ in_queue,
			    PACKAGES_COLUMN_CHECKBOX_ENABLE, enabled,
			    PACKAGES_COLUMN_TEXT, text,
			    PACKAGES_COLUMN_ID, package_id,
			    PACKAGES_COLUMN_IMAGE, icon,
			    -1);

	pk_package_id_free (ident);
	g_free (text);

	while (gtk_events_pending ()) {
		gtk_main_iteration ();
	}
}

/**
 * gpk_application_error_code_cb:
 **/
static void
gpk_application_error_code_cb (PkClient *client, PkErrorCodeEnum code, const gchar *details, GpkApplication *application)
{
	GtkWidget *widget;
	g_return_if_fail (PK_IS_APPLICATION (application));

	/* obvious message, don't tell the user */
	if (code == PK_ERROR_ENUM_TRANSACTION_CANCELLED) {
		return;
	}

	widget = glade_xml_get_widget (application->priv->glade_xml, "window_manager");
	gpk_error_dialog_modal (GTK_WINDOW (widget), gpk_error_enum_to_localised_text (code),
				gpk_error_enum_to_localised_message (code), details);
}

/**
 * gpk_application_refresh_search_results:
 **/
static gboolean
gpk_application_refresh_search_results (GpkApplication *application)
{
	GtkWidget *widget;
	gboolean ret;
	GError *error = NULL;
	PkRoleEnum role;

	/* get role -- do we actually need to do anything */
	pk_client_get_role (application->priv->client_search, &role, NULL, NULL);
	if (role == PK_ROLE_ENUM_UNKNOWN) {
		pk_debug ("no defined role, no not requeuing");
		return FALSE;
	}

	gtk_list_store_clear (application->priv->packages_store);
	ret = pk_client_requeue (application->priv->client_search, &error);
	if (!ret) {
		pk_warning ("failed to requeue the search: %s", error->message);
		g_error_free (error);
		return FALSE;
	}

	/* hide details */
	widget = glade_xml_get_widget (application->priv->glade_xml, "vbox_description");
	gtk_widget_hide (widget);
	return TRUE;
}

/**
 * gpk_application_finished_cb:
 **/
static void
gpk_application_finished_cb (PkClient *client, PkExitEnum exit, guint runtime, GpkApplication *application)
{
	GtkWidget *widget;
	PkRoleEnum role;

	g_return_if_fail (PK_IS_APPLICATION (application));

	/* get role */
	pk_client_get_role (client, &role, NULL, NULL);

	if (role == PK_ROLE_ENUM_SEARCH_NAME ||
	    role == PK_ROLE_ENUM_SEARCH_DETAILS ||
	    role == PK_ROLE_ENUM_SEARCH_GROUP ||
	    role == PK_ROLE_ENUM_GET_PACKAGES) {

		/* switch round buttons */
		gpk_application_set_find_cancel_buttons (application, TRUE);

		/* were there no entries found? */
		if (exit == PK_EXIT_ENUM_SUCCESS && !application->priv->has_package) {
			GtkTreeIter iter;
			gtk_list_store_append (application->priv->packages_store, &iter);
			gtk_list_store_set (application->priv->packages_store, &iter,
					    PACKAGES_COLUMN_STATE, FALSE,
					    PACKAGES_COLUMN_CHECKBOX, FALSE,
					    PACKAGES_COLUMN_CHECKBOX_ENABLE, FALSE,
					    PACKAGES_COLUMN_TEXT, _("No results were found"),
					    PACKAGES_COLUMN_IMAGE, "search",
					    -1);
		}

		/* focus back to the text extry */
		widget = glade_xml_get_widget (application->priv->glade_xml, "entry_text");
		gtk_widget_grab_focus (widget);

		gpk_application_treeview_set_sorted (application, TRUE);
	}

	/* do we need to update the search? */
	if (role == PK_ROLE_ENUM_INSTALL_PACKAGES ||
	    role == PK_ROLE_ENUM_REMOVE_PACKAGES) {
		/* refresh the search as the items may have changed and the filter has not changed */
		gpk_application_refresh_search_results (application);
	}
}

/**
 * gpk_application_cancel_cb:
 **/
static void
gpk_application_cancel_cb (GtkWidget *button_widget, GpkApplication *application)
{
	gboolean ret;

	g_return_if_fail (PK_IS_APPLICATION (application));

	ret = pk_client_cancel (application->priv->client_search, NULL);
	pk_debug ("canceled? %i", ret);

	/* switch buttons around */
	if (ret) {
		gpk_application_set_find_cancel_buttons (application, TRUE);
		application->priv->search_mode = PK_MODE_UNKNOWN;
	}
}

/**
 * gpk_application_perform_search_name_details_file:
 **/
static gboolean
gpk_application_perform_search_name_details_file (GpkApplication *application)
{
	GtkWidget *widget;
	const gchar *package;
	GError *error = NULL;
	gboolean ret;

	widget = glade_xml_get_widget (application->priv->glade_xml, "entry_text");
	package = gtk_entry_get_text (GTK_ENTRY (widget));

	/* have we got input? */
	if (pk_strzero (package)) {
		pk_debug ("no input");
		return FALSE;
	}

	ret = pk_strvalidate (package);
	if (!ret) {
		pk_debug ("invalid input text, will fail");
		/* TODO - make the dialog turn red... */
		widget = glade_xml_get_widget (application->priv->glade_xml, "window_manager");
		gpk_error_dialog_modal (GTK_WINDOW (widget), _("Invalid search text"),
					_("The search text contains invalid characters"), NULL);
		return FALSE;
	}
	pk_debug ("find %s", package);

	/* reset */
	ret = pk_client_reset (application->priv->client_search, &error);
	if (!ret) {
		pk_warning ("failed to reset client: %s", error->message);
		g_error_free (error);
		return FALSE;
	}

	/* do the search */
	if (application->priv->search_type == PK_SEARCH_NAME) {
		ret = pk_client_search_name (application->priv->client_search, application->priv->filters_current, package, &error);
	} else if (application->priv->search_type == PK_SEARCH_DETAILS) {
		ret = pk_client_search_details (application->priv->client_search, application->priv->filters_current, package, &error);
	} else if (application->priv->search_type == PK_SEARCH_FILE) {
		ret = pk_client_search_file (application->priv->client_search, application->priv->filters_current, package, &error);
	} else {
		pk_warning ("invalid search type");
		return FALSE;
	}

	if (!ret) {
		widget = glade_xml_get_widget (application->priv->glade_xml, "window_manager");
		gpk_error_dialog_modal (GTK_WINDOW (widget), _("The search could not be completed"),
					_("Running the transaction failed"), error->message);
		g_error_free (error);
		return FALSE;
	}

	/* unsorted */
	gpk_application_treeview_set_sorted (application, FALSE);

	/* clear existing list */
	gtk_list_store_clear (application->priv->packages_store);
	application->priv->has_package = FALSE;

	/* hide details */
	widget = glade_xml_get_widget (application->priv->glade_xml, "vbox_description");
	gtk_widget_hide (widget);

	/* switch around buttons */
	gpk_application_set_find_cancel_buttons (application, FALSE);

	widget = glade_xml_get_widget (application->priv->glade_xml, "notebook_search_cancel");
	gtk_notebook_set_current_page (GTK_NOTEBOOK (widget), 1);

	return TRUE;
}

/**
 * gpk_application_perform_search_others:
 **/
static gboolean
gpk_application_perform_search_others (GpkApplication *application)
{
	gboolean ret;
	GtkWidget *widget;
	GError *error = NULL;

	g_return_val_if_fail (PK_IS_APPLICATION (application), FALSE);
	g_return_val_if_fail (application->priv->group != NULL, FALSE);

	/* cancel this, we don't care about old results that are pending */
	ret = pk_client_reset (application->priv->client_search, &error);
	if (!ret) {
		pk_warning ("failed to reset client: %s", error->message);
		g_error_free (error);
		return FALSE;
	}

	/* unsorted */
	gpk_application_treeview_set_sorted (application, FALSE);

	/* refresh the search as the items may have changed */
	gtk_list_store_clear (application->priv->packages_store);

	if (application->priv->search_mode == PK_MODE_GROUP) {
		ret = pk_client_search_group (application->priv->client_search,
					      application->priv->filters_current,
					      application->priv->group, &error);
	} else {
		ret = pk_client_get_packages (application->priv->client_search,
					      application->priv->filters_current, &error);
	}

	if (ret) {
		/* switch around buttons */
		gpk_application_set_find_cancel_buttons (application, FALSE);
	} else {
		widget = glade_xml_get_widget (application->priv->glade_xml, "window_manager");
		gpk_error_dialog_modal (GTK_WINDOW (widget), _("The group could not be queried"),
					_("Running the transaction failed"), error->message);
		g_error_free (error);
	}
	return ret;
}

/**
 * gpk_application_perform_search:
 **/
static gboolean
gpk_application_perform_search (GpkApplication *application)
{
	gboolean ret = FALSE;

	if (application->priv->search_mode == PK_MODE_NAME_DETAILS_FILE) {
		ret = gpk_application_perform_search_name_details_file (application);
	} else if (application->priv->search_mode == PK_MODE_GROUP ||
		   application->priv->search_mode == PK_MODE_ALL_PACKAGES) {
		ret = gpk_application_perform_search_others (application);
	} else {
		pk_debug ("doing nothing");
	}
	return ret;
}

/**
 * gpk_application_find_cb:
 **/
static void
gpk_application_find_cb (GtkWidget *button_widget, GpkApplication *application)
{
	g_return_if_fail (PK_IS_APPLICATION (application));

	application->priv->search_mode = PK_MODE_NAME_DETAILS_FILE;
	gpk_application_perform_search (application);
}

/**
 * gpk_application_quit:
 * @event: The event type, unused.
 **/
static gboolean
gpk_application_quit (GpkApplication *application)
{
	gboolean ret;
	GError *error = NULL;

	g_return_val_if_fail (PK_IS_APPLICATION (application), FALSE);

	/* we might have visual stuff running, close them down */
	ret = pk_client_cancel (application->priv->client_search, &error);
	if (!ret) {
		pk_warning ("failed to cancel client: %s", error->message);
		g_error_free (error);
		error = NULL;
	}
	ret = pk_client_cancel (application->priv->client_details, &error);
	if (!ret) {
		pk_warning ("failed to cancel client: %s", error->message);
		g_error_free (error);
		error = NULL;
	}
	ret = pk_client_cancel (application->priv->client_files, &error);
	if (!ret) {
		pk_warning ("failed to cancel client: %s", error->message);
		g_error_free (error);
		error = NULL;
	}

	pk_debug ("emitting action-close");
	g_signal_emit (application, signals [ACTION_CLOSE], 0);
	return TRUE;
}

/**
 * gpk_application_delete_event_cb:
 * @event: The event type, unused.
 **/
static gboolean
gpk_application_delete_event_cb (GtkWidget	*widget,
				GdkEvent	*event,
				GpkApplication	*application)
{
	g_return_val_if_fail (PK_IS_APPLICATION (application), FALSE);

	gpk_application_quit (application);
	return FALSE;
}

/**
 * gpk_application_text_changed_cb:
 **/
static gboolean
gpk_application_text_changed_cb (GtkEntry *entry, GdkEventKey *event, GpkApplication *application)
{
	gboolean valid;
	GtkWidget *widget;
	const gchar *package;
	GtkTreeSelection *selection;

	g_return_val_if_fail (PK_IS_APPLICATION (application), FALSE);

	widget = glade_xml_get_widget (application->priv->glade_xml, "entry_text");
	package = gtk_entry_get_text (GTK_ENTRY (widget));

	/* clear group selection if we have the tab */
	if (pk_enums_contain (application->priv->roles, PK_ROLE_ENUM_SEARCH_GROUP)) {
		widget = glade_xml_get_widget (application->priv->glade_xml, "treeview_groups");
		selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (widget));
		gtk_tree_selection_unselect_all (selection);
	}

	/* check for invalid chars */
	valid = pk_strvalidate (package);

	widget = glade_xml_get_widget (application->priv->glade_xml, "button_find");
	if (valid == FALSE || pk_strzero (package)) {
		gtk_widget_set_sensitive (widget, FALSE);
	} else {
		gtk_widget_set_sensitive (widget, TRUE);
	}
	return FALSE;
}

/**
 * gpk_application_set_buttons_apply_clear:
 **/
static void
gpk_application_set_buttons_apply_clear (GpkApplication *application)
{
	GtkWidget *widget;
	GtkTreeView *treeview;
	gboolean valid;
	GtkTreeIter iter;
	GtkTreeModel *model;
	GpkPackageState state;
	gboolean enabled;

	g_return_if_fail (PK_IS_APPLICATION (application));

	/* okay to apply? */
	if (application->priv->package_list->len == 0) {
		widget = glade_xml_get_widget (application->priv->glade_xml, "toolbutton_apply");
		gtk_widget_set_sensitive (widget, FALSE);
		widget = glade_xml_get_widget (application->priv->glade_xml, "toolbutton_clear");
		gtk_widget_set_sensitive (widget, FALSE);
		application->priv->action = PK_ACTION_NONE;
	} else {
		widget = glade_xml_get_widget (application->priv->glade_xml, "toolbutton_apply");
		gtk_widget_set_sensitive (widget, TRUE);
		widget = glade_xml_get_widget (application->priv->glade_xml, "toolbutton_clear");
		gtk_widget_set_sensitive (widget, TRUE);
	}

	/* correct the enabled state */
	widget = glade_xml_get_widget (application->priv->glade_xml, "treeview_packages");
	treeview = GTK_TREE_VIEW (widget);
	model = gtk_tree_view_get_model (treeview);
	valid = gtk_tree_model_get_iter_first (model, &iter);

	/* for all current items, reset the state if in the list */
	while (valid) {
		gtk_tree_model_get (model, &iter, PACKAGES_COLUMN_STATE, &state, -1);
		enabled = gpk_application_get_checkbox_enable (application, state);
		gtk_list_store_set (GTK_LIST_STORE (model), &iter, PACKAGES_COLUMN_CHECKBOX_ENABLE, enabled, -1);
		valid = gtk_tree_model_iter_next (model, &iter);
	}
}

#if 0
/**
 * gpk_application_packages_add_selection:
 **/
static void
gpk_application_packages_add_selection (GpkApplication *application, GtkTreeModel *model, GtkTreeIter iter)
{
	gboolean installed;
	gboolean selected;
	gboolean is_in_list = FALSE;
	gboolean ret;
	gboolean checkbox;
	gchar *package_id = NULL;
	gchar *message;
	const gchar *icon;
	GtkWidget *widget;
	GpkPackageState state;

	gtk_tree_model_get (model, &iter,
			    PACKAGES_COLUMN_STATE, &state,
			    PACKAGES_COLUMN_CHECKBOX, &checkbox,
			    PACKAGES_COLUMN_ID, &package_id, -1);

	if (application->priv->action == PK_ACTION_REMOVE && !checkbox) {
		/* is it in the package list and we want to deselect it? */
		ret = pk_ptr_array_remove_string (application->priv->package_list, package_id);
		if (ret) {
			pk_debug ("removed %s from package list", package_id);
			goto set_new_value;
		}
		/* wrong action type */
		message = g_strdup_printf ("%s\n%s\n%s",
					   _("There are already packages queued to be removed."),
					   _("Click 'Clear list' to remove the previous selection or "
					     "'Remove packages' to complete the previous action."),
					   _("After completing the action new packages can be selected to be installed."));
		widget = glade_xml_get_widget (application->priv->glade_xml, "window_manager");
		gpk_error_dialog_modal (GTK_WINDOW (widget), _("Already selected packages to be removed"), message, NULL);
		g_free (message);
		pk_warning ("ignoring action as ACTION=REMOVE and not in list");
		goto out;
	}

	if (application->priv->action == PK_ACTION_INSTALL && checkbox) {
		/* is it in the package list and we want to deselect it? */
		ret = pk_ptr_array_remove_string (application->priv->package_list, package_id);
		if (ret) {
			pk_debug ("removed %s from package list", package_id);
			goto set_new_value;
		}
		/* wrong action type */
		message = g_strdup_printf ("%s\n%s\n%s",
					   _("There are already packages queued to be installed."),
					   _("Click 'Clear list' to remove the previous selection or "
					     "'Install packages' to complete the previous action."),
					   _("After completing the action new packages can be selected to be removed."));
		widget = glade_xml_get_widget (application->priv->glade_xml, "window_manager");
		gpk_error_dialog_modal (GTK_WINDOW (widget), _("Already selected packages to be installed"), message, NULL);
		g_free (message);
		pk_warning ("ignoring action as ACTION=INSTALL");
		goto out;
	}

	/* set new action if undecided */
	if (application->priv->action == PK_ACTION_NONE && checkbox) {
		application->priv->action = PK_ACTION_REMOVE;
		gpk_application_set_buttons_apply_clear (application);
	}
	if (application->priv->action == PK_ACTION_NONE && !checkbox) {
		application->priv->action = PK_ACTION_INSTALL;
		gpk_application_set_buttons_apply_clear (application);
	}

	if (application->priv->action == PK_ACTION_REMOVE) {
		pk_debug ("add to remove list %s", package_id);
		g_ptr_array_add (application->priv->package_list, g_strdup (package_id));
		is_in_list = TRUE;
	}
	if (application->priv->action == PK_ACTION_INSTALL) {
		pk_debug ("add to install list %s", package_id);
		g_ptr_array_add (application->priv->package_list, g_strdup (package_id));
		is_in_list = TRUE;
	}

set_new_value:
	/* do something with the value */
	selected ^= 1;

	/* get the new icon */
	icon = g2pk_application_get_icon (state);

	/* set new value */
	gtk_list_store_set (GTK_LIST_STORE (model), &iter,
			    PACKAGES_COLUMN_STATE, selected,
			    PACKAGES_COLUMN_CHECKBOX, installed ^ selected,
			    PACKAGES_COLUMN_IMAGE, icon,
			    -1);

	if (application->priv->package_list->len == 0) {
		application->priv->action = PK_ACTION_NONE;
		gpk_application_set_buttons_apply_clear (application);
	}
out:
	g_free (package_id);
}
#endif

/**
 * gpk_application_packages_checkbox_invert:
 **/
static void
gpk_application_packages_checkbox_invert (GpkApplication *application)
{
	GtkTreeView *treeview;
	GtkTreeModel *model;
	GtkTreeIter iter;
	GtkTreeSelection *selection;
	GtkWidget *widget;
	const gchar *icon;
	gboolean checkbox;
	GpkPackageState state;

	/* get the selection and add */
	widget = glade_xml_get_widget (application->priv->glade_xml, "treeview_packages");
	treeview = GTK_TREE_VIEW (widget);
	selection = gtk_tree_view_get_selection (treeview);
	gtk_tree_selection_get_selected (selection, &model, &iter);

	gtk_tree_model_get (model, &iter, PACKAGES_COLUMN_STATE, &state, -1);

	/* do something with the value */
	gpk_application_state_invert (&state);

	/* get the new icon */
	icon = gpk_application_state_get_icon (state);
	checkbox = gpk_application_state_get_checkbox (state);

	/* set new value */
	gtk_list_store_set (GTK_LIST_STORE (model), &iter,
			    PACKAGES_COLUMN_STATE, state,
			    PACKAGES_COLUMN_CHECKBOX, checkbox,
			    PACKAGES_COLUMN_IMAGE, icon,
			    -1);
}

/**
 * gpk_application_allow_install:
 **/
static void
gpk_application_allow_install (GpkApplication *application, gboolean allow)
{
	GtkWidget *widget;

	widget = glade_xml_get_widget (application->priv->glade_xml, "toolbutton_install");
	gtk_widget_set_sensitive (widget, allow);
	widget = glade_xml_get_widget (application->priv->glade_xml, "menuitem_install");
	gtk_widget_set_sensitive (widget, allow);
}

/**
 * gpk_application_allow_remove:
 **/
static void
gpk_application_allow_remove (GpkApplication *application, gboolean allow)
{
	GtkWidget *widget;

	widget = glade_xml_get_widget (application->priv->glade_xml, "toolbutton_remove");
	gtk_widget_set_sensitive (widget, allow);
	widget = glade_xml_get_widget (application->priv->glade_xml, "menuitem_remove");
	gtk_widget_set_sensitive (widget, allow);
}

/**
 * gpk_application_button_remove_cb:
 **/
static void
gpk_application_button_remove_cb (GtkWidget *widget_button, GpkApplication *application)
{
	gboolean ret;
	guint index;

	g_return_if_fail (PK_IS_APPLICATION (application));

	/* shouldn't be possible */
	if (application->priv->package == NULL) {
		pk_warning ("no package");
		return;
	}

	/* changed mind, or wrong mode */
	if (application->priv->action == PK_ACTION_INSTALL) {
		ret = pk_ptr_array_remove_string (application->priv->package_list, application->priv->package);
		if (ret) {
			pk_debug ("removed %s from package list", application->priv->package);

			/* correct buttons */
			gpk_application_allow_install (application, TRUE);
			gpk_application_allow_remove (application, FALSE);
			gpk_application_packages_checkbox_invert (application);
			gpk_application_set_buttons_apply_clear (application);
			return;
		}
		pk_warning ("wrong mode and not in list");
		return;
	}

	/* already added */
	index = pk_ptr_array_find_string (application->priv->package_list, application->priv->package);
	if (index != -1) {
		pk_warning ("already added");
		return;
	}

	application->priv->action = PK_ACTION_REMOVE;
	g_ptr_array_add (application->priv->package_list, g_strdup (application->priv->package));

	/* correct buttons */
	gpk_application_allow_install (application, TRUE);
	gpk_application_allow_remove (application, FALSE);
	gpk_application_packages_checkbox_invert (application);
	gpk_application_set_buttons_apply_clear (application);
}

/**
 * gpk_application_button_install_cb:
 **/
static void
gpk_application_button_install_cb (GtkWidget *widget_button, GpkApplication *application)
{
	gboolean ret;
	guint index;

	g_return_if_fail (PK_IS_APPLICATION (application));

	/* shouldn't be possible */
	if (application->priv->package == NULL) {
		pk_warning ("no package");
		return;
	}

	/* changed mind, or wrong mode */
	if (application->priv->action == PK_ACTION_REMOVE) {
		ret = pk_ptr_array_remove_string (application->priv->package_list, application->priv->package);
		if (ret) {
			pk_debug ("removed %s from package list", application->priv->package);

			/* correct buttons */
			gpk_application_allow_install (application, FALSE);
			gpk_application_allow_remove (application, TRUE);
			gpk_application_packages_checkbox_invert (application);
			gpk_application_set_buttons_apply_clear (application);
			return;
		}
		pk_warning ("wrong mode and not in list");
		return;
	}

	/* already added */
	index = pk_ptr_array_find_string (application->priv->package_list, application->priv->package);
	if (index != -1) {
		pk_warning ("already added");
		return;
	}

	application->priv->action = PK_ACTION_INSTALL;
	g_ptr_array_add (application->priv->package_list, g_strdup (application->priv->package));

	/* correct buttons */
	gpk_application_allow_install (application, FALSE);
	gpk_application_allow_remove (application, TRUE);
	gpk_application_packages_checkbox_invert (application);
	gpk_application_set_buttons_apply_clear (application);
}


/**
 * gpk_application_packages_installed_clicked_cb:
 **/
static void
gpk_application_packages_installed_clicked_cb (GtkCellRendererToggle *cell, gchar *path_str, gpointer data)
{
	GpkApplication *application = (GpkApplication *) data;
	GtkTreeView *treeview;
	GtkWidget *widget;
	GtkTreeModel *model;
	GtkTreeIter iter;
	GtkTreePath *path;
	GpkPackageState state;

	g_return_if_fail (PK_IS_APPLICATION (application));

	pk_warning ("method not yet safe");

	widget = glade_xml_get_widget (application->priv->glade_xml, "treeview_packages");
	treeview = GTK_TREE_VIEW (widget);
	model = gtk_tree_view_get_model (treeview);
	path = gtk_tree_path_new_from_string (path_str);

	/* get toggled iter */
	gtk_tree_model_get_iter (model, &iter, path);
	g_free (application->priv->package);
	gtk_tree_model_get (model, &iter,
			    PACKAGES_COLUMN_STATE, &state,
			    PACKAGES_COLUMN_ID, &application->priv->package, -1);
	if (gpk_application_state_get_checkbox (state)) {
		gpk_application_button_remove_cb (NULL, application);
	} else {
		gpk_application_button_install_cb (NULL, application);
	}
	gtk_tree_path_free (path);
}

static void gpk_application_packages_treeview_clicked_cb (GtkTreeSelection *selection, GpkApplication *application);

/**
 * gpk_application_button_clear_cb:
 **/
static void
gpk_application_button_clear_cb (GtkWidget *widget_button, GpkApplication *application)
{
	GtkTreeView *treeview;
	gboolean valid;
	gboolean checkbox;
	GtkWidget *widget;
	GtkTreeIter iter;
	GtkTreeModel *model;
	GtkTreeSelection *selection;
	const gchar *icon;
	GpkPackageState state;
	gboolean ret;

	g_return_if_fail (PK_IS_APPLICATION (application));

	/* get the first iter in the list */
	widget = glade_xml_get_widget (application->priv->glade_xml, "treeview_packages");
	treeview = GTK_TREE_VIEW (widget);
	model = gtk_tree_view_get_model (treeview);
	valid = gtk_tree_model_get_iter_first (model, &iter);

	/* for all current items, reset the state if in the list */
	while (valid) {
		gtk_tree_model_get (model, &iter, PACKAGES_COLUMN_STATE, &state, -1);
		ret = gpk_application_state_unselect (&state);
		if (ret) {
			/* get the new icon */
			icon = gpk_application_state_get_icon (state);
			checkbox = gpk_application_state_get_checkbox (state);

			/* set new value */
			gtk_list_store_set (GTK_LIST_STORE (model), &iter,
					    PACKAGES_COLUMN_STATE, state,
					    PACKAGES_COLUMN_CHECKBOX, checkbox,
					    PACKAGES_COLUMN_IMAGE, icon,
					    -1);
		}
		valid = gtk_tree_model_iter_next (model, &iter);
	}

	g_ptr_array_remove_range (application->priv->package_list, 0, application->priv->package_list->len);

	/* force a button refresh */
	widget = glade_xml_get_widget (application->priv->glade_xml, "treeview_packages");
	selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (widget));
	gpk_application_packages_treeview_clicked_cb (selection, application);

	gpk_application_set_buttons_apply_clear (application);
}

/**
 * gpk_application_button_apply_cb:
 **/
static void
gpk_application_button_apply_cb (GtkWidget *widget, GpkApplication *application)
{
	gboolean ret = FALSE;
	GError *error = NULL;
	gchar **package_ids = NULL;
	gchar *exec;

	g_return_if_fail (PK_IS_APPLICATION (application));

	package_ids = pk_ptr_array_to_argv (application->priv->package_list);
	if (application->priv->action == PK_ACTION_INSTALL) {
		ret = gpk_client_install_package_ids (application->priv->gclient, package_ids, NULL);
		/* can we show the user the new application? */
		if (ret) {
			exec = gpk_client_run_show (package_ids);
			if (exec != NULL) {
				ret = g_spawn_command_line_async (exec, &error);
				if (!ret) {
					pk_warning ("failed to run: %s", error->message);
					g_error_free (error);
				}
			}
			g_free (exec);
		}
	}
	if (application->priv->action == PK_ACTION_REMOVE) {
		ret = gpk_client_remove_package_ids (application->priv->gclient, package_ids, NULL);
	}
	g_strfreev (package_ids);

	/* refresh the search as the items may have changed and the filter has not changed */
	if (ret) {
		/* clear if success */
		g_ptr_array_remove_range (application->priv->package_list, 0, application->priv->package_list->len);
		application->priv->action = PK_ACTION_NONE;
		gpk_application_set_buttons_apply_clear (application);
		gpk_application_refresh_search_results (application);
	}
}

static void
gpk_application_packages_add_columns (GpkApplication *application)
{
	GtkCellRenderer *renderer;
	GtkTreeViewColumn *column;
	GtkTreeView *treeview;
	GtkTreeModel *model;
	GtkWidget *widget;

	widget = glade_xml_get_widget (application->priv->glade_xml, "treeview_packages");
	treeview = GTK_TREE_VIEW (widget);
	model = gtk_tree_view_get_model (treeview);

	/* column for installed toggles */
	renderer = gtk_cell_renderer_toggle_new ();
	g_signal_connect (renderer, "toggled", G_CALLBACK (gpk_application_packages_installed_clicked_cb), application);
	column = gtk_tree_view_column_new_with_attributes (_("Installed"), renderer,
							   "active", PACKAGES_COLUMN_CHECKBOX,
							   "visible", PACKAGES_COLUMN_CHECKBOX_ENABLE, NULL);
	gtk_tree_view_append_column (treeview, column);


	/* column for images */
	column = gtk_tree_view_column_new ();
	renderer = gtk_cell_renderer_pixbuf_new ();
	g_object_set (renderer, "stock-size", GTK_ICON_SIZE_DIALOG, NULL);
	gtk_tree_view_column_pack_start (column, renderer, FALSE);
	gtk_tree_view_column_add_attribute (column, renderer, "icon-name", PACKAGES_COLUMN_IMAGE);
	gtk_tree_view_append_column (treeview, column);

	/* column for name */
	renderer = gtk_cell_renderer_text_new ();
	column = gtk_tree_view_column_new_with_attributes (_("Name"), renderer,
							   "markup", PACKAGES_COLUMN_TEXT, NULL);
	gtk_tree_view_column_set_sort_column_id (column, PACKAGES_COLUMN_TEXT);
	gtk_tree_view_append_column (treeview, column);
}

static void
gpk_application_groups_add_columns (GtkTreeView *treeview)
{
	GtkCellRenderer *renderer;
	GtkTreeViewColumn *column;

	column = gtk_tree_view_column_new ();
	renderer = gtk_cell_renderer_pixbuf_new ();
	g_object_set (renderer, "stock-size", GTK_ICON_SIZE_LARGE_TOOLBAR, NULL);
	gtk_tree_view_column_pack_start (column, renderer, FALSE);
	gtk_tree_view_column_add_attribute (column, renderer, "icon-name", GROUPS_COLUMN_ICON);
	gtk_tree_view_append_column (treeview, column);

	/* column for name */
	renderer = gtk_cell_renderer_text_new ();
	column = gtk_tree_view_column_new_with_attributes (_("Name"), renderer,
							   "text", GROUPS_COLUMN_NAME, NULL);
	gtk_tree_view_column_set_sort_column_id (column, GROUPS_COLUMN_NAME);
	gtk_tree_view_append_column (treeview, column);

}

/**
 * gpk_application_groups_treeview_clicked_cb:
 **/
static void
gpk_application_groups_treeview_clicked_cb (GtkTreeSelection *selection, GpkApplication *application)
{
	GtkTreeModel *model;
	GtkTreeIter iter;
	GtkWidget *widget;

	g_return_if_fail (PK_IS_APPLICATION (application));

	/* hide the details */
	widget = glade_xml_get_widget (application->priv->glade_xml, "vbox_description");
	gtk_widget_hide (widget);

	/* clear the search text if we clicked the group list */
	widget = glade_xml_get_widget (application->priv->glade_xml, "entry_text");
	gtk_entry_set_text (GTK_ENTRY (widget), "");

	/* This will only work in single or browse selection mode! */
	if (gtk_tree_selection_get_selected (selection, &model, &iter)) {
		g_free (application->priv->group);
		gtk_tree_model_get (model, &iter, GROUPS_COLUMN_ID, &application->priv->group, -1);
		pk_debug ("selected row is: %s", application->priv->group);

		/* GetPackages? */
		if (pk_strequal (application->priv->group, "all-packages")) {
			application->priv->search_mode = PK_MODE_ALL_PACKAGES;
		} else {
			application->priv->search_mode = PK_MODE_GROUP;
		}

		/* actually do the search */
		gpk_application_perform_search (application);
	}
}

/**
 * gpk_application_packages_treeview_clicked_cb:
 **/
static void
gpk_application_packages_treeview_clicked_cb (GtkTreeSelection *selection, GpkApplication *application)
{
	GtkWidget *widget;
	GtkTreeModel *model;
	GtkTreeIter iter;
	gboolean ret;
	GError *error = NULL;
	gboolean show_install = TRUE;
	gboolean show_remove = TRUE;
	GpkPackageState state;

	g_return_if_fail (PK_IS_APPLICATION (application));

	/* reset */
	g_free (application->priv->package);
	application->priv->package = NULL;

	/* This will only work in single or browse selection mode! */
	if (!gtk_tree_selection_get_selected (selection, &model, &iter)) {
		pk_debug ("no row selected");

		/* we cannot now add it */
		gpk_application_allow_install (application, FALSE);
		gpk_application_allow_remove (application, FALSE);
		widget = glade_xml_get_widget (application->priv->glade_xml, "menuitem_selection");
		gtk_widget_hide (widget);
		widget = glade_xml_get_widget (application->priv->glade_xml, "hbox_filesize");
		gtk_widget_hide (widget);
		return;
	}

	/* show the menu item */
	widget = glade_xml_get_widget (application->priv->glade_xml, "menuitem_selection");
	gtk_widget_show (widget);

	/* get data */
	gtk_tree_model_get (model, &iter,
			    PACKAGES_COLUMN_STATE, &state,
			    PACKAGES_COLUMN_ID, &application->priv->package, -1);

	show_install = (state == GPK_STATE_AVAILABLE || state == GPK_STATE_INSTALLED_TO_BE_REMOVED);
	show_remove = (state == GPK_STATE_INSTALLED || state == GPK_STATE_AVAILABLE_TO_BE_INSTALLED);

	if (application->priv->action == PK_ACTION_INSTALL && !gpk_application_state_in_queue (state)) {
		show_remove = FALSE;
	}
	if (application->priv->action == PK_ACTION_REMOVE && !gpk_application_state_in_queue (state)) {
		show_install = FALSE;
	}

	/* only show buttons if we are in the correct mode */
	gpk_application_allow_install (application, show_install);
	gpk_application_allow_remove (application, show_remove);

	/* clear the old text */
	widget = glade_xml_get_widget (application->priv->glade_xml, "textview_description");
	gpk_application_set_text_buffer (widget, NULL);

	/* show the box */
	widget = glade_xml_get_widget (application->priv->glade_xml, "vbox_description");
	gtk_widget_show (widget);

	/* cancel any previous request */
	ret = pk_client_reset (application->priv->client_details, &error);
	if (!ret) {
		pk_warning ("failed to cancel, and adding to queue: %s", error->message);
		g_error_free (error);
		return;
	}

	/* get the details */
	ret = pk_client_get_details (application->priv->client_details,
				     application->priv->package, &error);
	if (!ret) {
		pk_warning ("failed to get details: %s", error->message);
		g_error_free (error);
	}
}

/**
 * gpk_application_connection_changed_cb:
 **/
static void
gpk_application_connection_changed_cb (PkConnection *pconnection, gboolean connected, GpkApplication *application)
{
	g_return_if_fail (PK_IS_APPLICATION (application));

	pk_debug ("connected=%i", connected);
}

/**
 * gpk_application_group_add_data:
 **/
static void
gpk_application_group_add_data (GpkApplication *application, PkGroupEnum group)
{
	GtkTreeIter iter;
	const gchar *icon_name;
	const gchar *text;

	gtk_list_store_append (application->priv->groups_store, &iter);

	text = gpk_group_enum_to_localised_text (group);
	icon_name = gpk_group_enum_to_icon_name (group);
	gtk_list_store_set (application->priv->groups_store, &iter,
			    GROUPS_COLUMN_NAME, text,
			    GROUPS_COLUMN_ID, pk_group_enum_to_text (group),
			    GROUPS_COLUMN_ICON, icon_name,
			    -1);
}

/**
 * gpk_application_create_custom_widget:
 **/
static GtkWidget *
gpk_application_create_custom_widget (GladeXML *xml, gchar *func_name, gchar *name,
				      gchar *string1, gchar *string2,
				      gint int1, gint int2, gpointer user_data)
{
	if (pk_strequal (name, "entry_text")) {
		return sexy_icon_entry_new ();
	}
	if (pk_strequal (name, "image_status")) {
		return gpk_animated_icon_new ();
	}
	pk_warning ("name unknown='%s'", name);
	return NULL;
}

/**
 * gpk_application_popup_position_menu:
 **/
static void
gpk_application_popup_position_menu (GtkMenu *menu, gint *x, gint *y, gboolean *push_in, gpointer user_data)
{
	GtkWidget     *widget;
	GtkRequisition requisition;
	gint menu_xpos = 0;
	gint menu_ypos = 0;

	widget = GTK_WIDGET (user_data);

	/* find the location */
	gdk_window_get_origin (widget->window, &menu_xpos, &menu_ypos);
	gtk_widget_size_request (GTK_WIDGET (widget), &requisition);

	/* set the position */
	*x = menu_xpos;
	*y = menu_ypos + requisition.height - 1;
	*push_in = TRUE;
}

/**
 * gpk_application_menu_search_by_name:
 **/
static void
gpk_application_menu_search_by_name (GtkMenuItem *item, gpointer data)
{
	GtkWidget *icon;
	GtkWidget *widget;
	GpkApplication *application = GPK_APPLICATION (data);

	/* change type */
	application->priv->search_type = PK_SEARCH_NAME;
	pk_debug ("set search type=%i", application->priv->search_type);

	/* set the new icon */
	widget = glade_xml_get_widget (application->priv->glade_xml, "entry_text");
	gtk_widget_set_tooltip_text (widget, _("Searching by name"));
	icon = gtk_image_new_from_stock (GTK_STOCK_FIND, GTK_ICON_SIZE_MENU);
	sexy_icon_entry_set_icon (SEXY_ICON_ENTRY (widget), SEXY_ICON_ENTRY_PRIMARY, GTK_IMAGE (icon));
}

/**
 * gpk_application_menu_search_by_description:
 **/
static void
gpk_application_menu_search_by_description (GtkMenuItem *item, gpointer data)
{
	GtkWidget *icon;
	GtkWidget *widget;
	GpkApplication *application = GPK_APPLICATION (data);

	/* set type */
	application->priv->search_type = PK_SEARCH_DETAILS;
	pk_debug ("set search type=%i", application->priv->search_type);

	/* set the new icon */
	widget = glade_xml_get_widget (application->priv->glade_xml, "entry_text");
	gtk_widget_set_tooltip_text (widget, _("Searching by description"));
	icon = gtk_image_new_from_stock (GTK_STOCK_EDIT, GTK_ICON_SIZE_MENU);
	sexy_icon_entry_set_icon (SEXY_ICON_ENTRY (widget), SEXY_ICON_ENTRY_PRIMARY, GTK_IMAGE (icon));
}

/**
 * gpk_application_menu_search_by_file:
 **/
static void
gpk_application_menu_search_by_file (GtkMenuItem *item, gpointer data)
{
	GtkWidget *icon;
	GtkWidget *widget;
	GpkApplication *application = GPK_APPLICATION (data);

	/* set type */
	application->priv->search_type = PK_SEARCH_FILE;
	pk_debug ("set search type=%i", application->priv->search_type);

	/* set the new icon */
	widget = glade_xml_get_widget (application->priv->glade_xml, "entry_text");
	gtk_widget_set_tooltip_text (widget, _("Searching by file"));
	icon = gtk_image_new_from_stock (GTK_STOCK_OPEN, GTK_ICON_SIZE_MENU);
	sexy_icon_entry_set_icon (SEXY_ICON_ENTRY (widget), SEXY_ICON_ENTRY_PRIMARY, GTK_IMAGE (icon));
}

/**
 * gpk_application_entry_text_icon_pressed_cb:
 **/
static void
gpk_application_entry_text_icon_pressed_cb (SexyIconEntry *entry, gint icon_pos, gint button, gpointer data)
{
	GtkMenu *menu = (GtkMenu*) gtk_menu_new ();
	GtkWidget *item;
	GtkWidget *image;
	GpkApplication *application = GPK_APPLICATION (data);

	g_return_if_fail (PK_IS_APPLICATION (application));

	/* only respond to left button */
	if (button != 1) {
		return;
	}
	pk_debug ("icon_pos=%i", icon_pos);

	if (pk_enums_contain (application->priv->roles, PK_ROLE_ENUM_SEARCH_NAME)) {
		item = gtk_image_menu_item_new_with_mnemonic (_("Search by name"));
		image = gtk_image_new_from_stock (GTK_STOCK_FIND, GTK_ICON_SIZE_MENU);
		gtk_image_menu_item_set_image (GTK_IMAGE_MENU_ITEM (item), image);
		g_signal_connect (G_OBJECT (item), "activate",
				  G_CALLBACK (gpk_application_menu_search_by_name), application);
		gtk_menu_shell_append (GTK_MENU_SHELL (menu), item);
	}

	if (pk_enums_contain (application->priv->roles, PK_ROLE_ENUM_SEARCH_DETAILS)) {
		item = gtk_image_menu_item_new_with_mnemonic (_("Search by description"));
		image = gtk_image_new_from_stock (GTK_STOCK_EDIT, GTK_ICON_SIZE_MENU);
		gtk_image_menu_item_set_image (GTK_IMAGE_MENU_ITEM (item), image);
		g_signal_connect (G_OBJECT (item), "activate",
				  G_CALLBACK (gpk_application_menu_search_by_description), application);
		gtk_menu_shell_append (GTK_MENU_SHELL (menu), item);
	}

	if (pk_enums_contain (application->priv->roles, PK_ROLE_ENUM_SEARCH_FILE)) {
		item = gtk_image_menu_item_new_with_mnemonic (_("Search by file name"));
		image = gtk_image_new_from_stock (GTK_STOCK_OPEN, GTK_ICON_SIZE_MENU);
		gtk_image_menu_item_set_image (GTK_IMAGE_MENU_ITEM (item), image);
		g_signal_connect (G_OBJECT (item), "activate",
				  G_CALLBACK (gpk_application_menu_search_by_file), application);
		gtk_menu_shell_append (GTK_MENU_SHELL (menu), item);
	}

	gtk_widget_show_all (GTK_WIDGET (menu));
	gtk_menu_popup (GTK_MENU (menu), NULL, NULL,
			gpk_application_popup_position_menu, entry,
			1, gtk_get_current_event_time());
}

/**
 * gpk_application_create_completion_model:
 *
 * Creates a tree model containing the completions
 **/
static GtkTreeModel *
gpk_application_create_completion_model (void)
{
	GtkListStore *store;
	GtkTreeIter iter;

	store = gtk_list_store_new (1, G_TYPE_STRING);

	/* append one word */
	gtk_list_store_append (store, &iter);
	gtk_list_store_set (store, &iter, 0, "gnome-power-manager", -1);

	/* append another word */
	gtk_list_store_append (store, &iter);
	gtk_list_store_set (store, &iter, 0, "gnome-screensaver", -1);

	/* and another word */
	gtk_list_store_append (store, &iter);
	gtk_list_store_set (store, &iter, 0, "hal", -1);

	return GTK_TREE_MODEL (store);
}


/**
 *  * gpk_application_about_dialog_url_cb:
 *   **/
static void
gpk_application_about_dialog_url_cb (GtkAboutDialog *about, const char *address, gpointer data)
{
	GError *error = NULL;
	gboolean ret;

	char *cmdline;
	GdkScreen *gscreen;
	GtkWidget *error_dialog;
	gchar *url;
	gchar *protocol = (gchar*) data;

	if (protocol != NULL)
		url = g_strconcat (protocol, address, NULL);
	else
		url = g_strdup (address);

	gscreen = gtk_window_get_screen (GTK_WINDOW (about));

	cmdline = g_strconcat ("xdg-open ", url, NULL);
	ret = gdk_spawn_command_line_on_screen (gscreen, cmdline, &error);
	g_free (cmdline);

	if (ret)
		goto out;

	g_error_free (error);
	error = NULL;
	cmdline = g_strconcat ("gnome-open ", url, NULL);
	ret = gdk_spawn_command_line_on_screen (gscreen, cmdline, &error);
	g_free (cmdline);

	if (!ret) {
		error_dialog = gtk_message_dialog_new (GTK_WINDOW (about),
						       GTK_DIALOG_MODAL,
						       GTK_MESSAGE_INFO,
						       GTK_BUTTONS_OK,
						       _("Failed to show url"));
		gtk_message_dialog_format_secondary_text (GTK_MESSAGE_DIALOG (error_dialog),
							  "%s", error->message);
		gtk_dialog_run (GTK_DIALOG (error_dialog));
		gtk_widget_destroy (error_dialog);
		g_error_free (error);
	}

out:
	g_free (url);
}

/**
 * gpk_application_menu_help_cb:
 **/
static void
gpk_application_menu_help_cb (GtkAction *action, GpkApplication *application)
{
	gpk_gnome_help ("add-remove");
}

/**
 * gpk_application_menu_about_cb:
 **/
static void
gpk_application_menu_about_cb (GtkAction *action, GpkApplication *application)
{
	static gboolean been_here = FALSE;
	GtkWidget *main_window;
	const char *authors[] = {
		"Richard Hughes <richard@hughsie.com>",
		NULL};
	const char *documenters[] = {
		"Richard Hughes <richard@hughsie.com>",
		NULL};
	const char *artists[] = {
		"Richard Hughes <richard@hughsie.com>",
		NULL};
	const char *license[] = {
		N_("Licensed under the GNU General Public License Version 2"),
		N_("PackageKit is free software; you can redistribute it and/or\n"
		   "modify it under the terms of the GNU General Public License\n"
		   "as published by the Free Software Foundation; either version 2\n"
		   "of the License, or (at your option) any later version."),
		N_("PackageKit is distributed in the hope that it will be useful,\n"
		   "but WITHOUT ANY WARRANTY; without even the implied warranty of\n"
		   "MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the\n"
		   "GNU General Public License for more details."),
		N_("You should have received a copy of the GNU General Public License\n"
		   "along with this program; if not, write to the Free Software\n"
		   "Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA\n"
		   "02110-1301, USA.")
	};
	const char  *translators = _("translator-credits");
	char	    *license_trans;

	/* Translators comment: put your own name here to appear in the about dialog. */
	if (!strcmp (translators, "translator-credits")) {
		translators = NULL;
	}

	license_trans = g_strconcat (_(license[0]), "\n\n", _(license[1]), "\n\n",
				     _(license[2]), "\n\n", _(license[3]), "\n",  NULL);

	if (!been_here) {
		been_here = TRUE;
		gtk_about_dialog_set_url_hook (gpk_application_about_dialog_url_cb, NULL, NULL);
		gtk_about_dialog_set_email_hook (gpk_application_about_dialog_url_cb, "mailto:", NULL);
	}

	/* use parent */
	main_window = glade_xml_get_widget (application->priv->glade_xml, "window_manager");

	gtk_window_set_default_icon_name (PK_STOCK_APP_ICON);
	gtk_show_about_dialog (GTK_WINDOW (main_window),
			       "version", PACKAGE_VERSION,
			       "copyright", "Copyright \xc2\xa9 2007-2008 Richard Hughes",
			       "license", license_trans,
			       "website-label", _("PackageKit Website"),
			       "website", "http://www.packagekit.org",
			       "comments", _("Package Manager for GNOME"),
			       "authors", authors,
			       "documenters", documenters,
			       "artists", artists,
			       "translator-credits", translators,
			       "logo-icon-name", PK_STOCK_APP_ICON,
			       NULL);
	g_free (license_trans);
}

/**
 * gpk_application_menu_sources_cb:
 **/
static void
gpk_application_menu_sources_cb (GtkAction *action, GpkApplication *application)
{
	gboolean ret;

	g_return_if_fail (PK_IS_APPLICATION (application));

	ret = g_spawn_command_line_async ("gpk-repo", NULL);
	if (!ret) {
		pk_warning ("spawn of pk-repo failed");
	}
}

/**
 * gpk_application_menu_refresh_cb:
 **/
static void
gpk_application_menu_refresh_cb (GtkAction *action, GpkApplication *application)
{
	gpk_client_refresh_cache (application->priv->gclient, NULL);
}

/**
 * gpk_application_menu_filter_installed_cb:
 * @widget: The GtkWidget object
 **/
static void
gpk_application_menu_filter_installed_cb (GtkWidget *widget, GpkApplication *application)
{
	const gchar *name;

	g_return_if_fail (PK_IS_APPLICATION (application));

	name = gtk_widget_get_name (widget);

	/* only care about new state */
	if (!gtk_check_menu_item_get_active (GTK_CHECK_MENU_ITEM (widget))) {
		return;
	}

	/* set new filter */
	if (g_str_has_suffix (name, "_yes")) {
		pk_enums_add (application->priv->filters_current, PK_FILTER_ENUM_INSTALLED);
		pk_enums_remove (application->priv->filters_current, PK_FILTER_ENUM_NOT_INSTALLED);
	} else if (g_str_has_suffix (name, "_no")) {
		pk_enums_remove (application->priv->filters_current, PK_FILTER_ENUM_INSTALLED);
		pk_enums_add (application->priv->filters_current, PK_FILTER_ENUM_NOT_INSTALLED);
	} else {
		pk_enums_remove (application->priv->filters_current, PK_FILTER_ENUM_INSTALLED);
		pk_enums_remove (application->priv->filters_current, PK_FILTER_ENUM_NOT_INSTALLED);
	}

	/* refresh the search results */
	gpk_application_perform_search (application);
}

/**
 * gpk_application_menu_filter_devel_cb:
 * @widget: The GtkWidget object
 **/
static void
gpk_application_menu_filter_devel_cb (GtkWidget *widget, GpkApplication *application)
{
	const gchar *name;

	g_return_if_fail (PK_IS_APPLICATION (application));

	name = gtk_widget_get_name (widget);

	/* only care about new state */
	if (!gtk_check_menu_item_get_active (GTK_CHECK_MENU_ITEM (widget))) {
		return;
	}

	/* set new filter */
	if (g_str_has_suffix (name, "_yes")) {
		pk_enums_add (application->priv->filters_current, PK_FILTER_ENUM_DEVELOPMENT);
		pk_enums_remove (application->priv->filters_current, PK_FILTER_ENUM_NOT_DEVELOPMENT);
	} else if (g_str_has_suffix (name, "_no")) {
		pk_enums_remove (application->priv->filters_current, PK_FILTER_ENUM_DEVELOPMENT);
		pk_enums_add (application->priv->filters_current, PK_FILTER_ENUM_NOT_DEVELOPMENT);
	} else {
		pk_enums_remove (application->priv->filters_current, PK_FILTER_ENUM_DEVELOPMENT);
		pk_enums_remove (application->priv->filters_current, PK_FILTER_ENUM_NOT_DEVELOPMENT);
	}

	/* refresh the search results */
	gpk_application_perform_search (application);
}

/**
 * gpk_application_menu_filter_gui_cb:
 * @widget: The GtkWidget object
 **/
static void
gpk_application_menu_filter_gui_cb (GtkWidget *widget, GpkApplication *application)
{
	const gchar *name;

	g_return_if_fail (PK_IS_APPLICATION (application));

	name = gtk_widget_get_name (widget);

	/* only care about new state */
	if (!gtk_check_menu_item_get_active (GTK_CHECK_MENU_ITEM (widget))) {
		return;
	}

	/* set new filter */
	if (g_str_has_suffix (name, "_yes")) {
		pk_enums_add (application->priv->filters_current, PK_FILTER_ENUM_GUI);
		pk_enums_remove (application->priv->filters_current, PK_FILTER_ENUM_NOT_GUI);
	} else if (g_str_has_suffix (name, "_no")) {
		pk_enums_remove (application->priv->filters_current, PK_FILTER_ENUM_GUI);
		pk_enums_add (application->priv->filters_current, PK_FILTER_ENUM_NOT_GUI);
	} else {
		pk_enums_remove (application->priv->filters_current, PK_FILTER_ENUM_GUI);
		pk_enums_remove (application->priv->filters_current, PK_FILTER_ENUM_NOT_GUI);
	}

	/* refresh the search results */
	gpk_application_perform_search (application);
}

/**
 * gpk_application_menu_filter_free_cb:
 * @widget: The GtkWidget object
 **/
static void
gpk_application_menu_filter_free_cb (GtkWidget *widget, GpkApplication *application)
{
	const gchar *name;

	g_return_if_fail (PK_IS_APPLICATION (application));

	name = gtk_widget_get_name (widget);

	/* only care about new state */
	if (!gtk_check_menu_item_get_active (GTK_CHECK_MENU_ITEM (widget))) {
		return;
	}

	/* set new filter */
	if (g_str_has_suffix (name, "_yes")) {
		pk_enums_add (application->priv->filters_current, PK_FILTER_ENUM_FREE);
		pk_enums_remove (application->priv->filters_current, PK_FILTER_ENUM_NOT_FREE);
	} else if (g_str_has_suffix (name, "_no")) {
		pk_enums_remove (application->priv->filters_current, PK_FILTER_ENUM_FREE);
		pk_enums_add (application->priv->filters_current, PK_FILTER_ENUM_NOT_FREE);
	} else {
		pk_enums_remove (application->priv->filters_current, PK_FILTER_ENUM_FREE);
		pk_enums_remove (application->priv->filters_current, PK_FILTER_ENUM_NOT_FREE);
	}

	/* refresh the search results */
	gpk_application_perform_search (application);
}

/**
 * gpk_application_menu_filter_arch_cb:
 * @widget: The GtkWidget object
 **/
static void
gpk_application_menu_filter_arch_cb (GtkWidget *widget, GpkApplication *application)
{
	const gchar *name;

	g_return_if_fail (PK_IS_APPLICATION (application));

	name = gtk_widget_get_name (widget);

	/* only care about new state */
	if (!gtk_check_menu_item_get_active (GTK_CHECK_MENU_ITEM (widget))) {
		return;
	}

	/* set new filter */
	if (g_str_has_suffix (name, "_yes")) {
		pk_enums_add (application->priv->filters_current, PK_FILTER_ENUM_ARCH);
		pk_enums_remove (application->priv->filters_current, PK_FILTER_ENUM_NOT_ARCH);
	} else if (g_str_has_suffix (name, "_no")) {
		pk_enums_remove (application->priv->filters_current, PK_FILTER_ENUM_ARCH);
		pk_enums_add (application->priv->filters_current, PK_FILTER_ENUM_NOT_ARCH);
	} else {
		pk_enums_remove (application->priv->filters_current, PK_FILTER_ENUM_ARCH);
		pk_enums_remove (application->priv->filters_current, PK_FILTER_ENUM_NOT_ARCH);
	}

	/* refresh the search results */
	gpk_application_perform_search (application);
}

/**
 * gpk_application_menu_filter_source_cb:
 * @widget: The GtkWidget object
 **/
static void
gpk_application_menu_filter_source_cb (GtkWidget *widget, GpkApplication *application)
{
	const gchar *name;

	g_return_if_fail (PK_IS_APPLICATION (application));

	name = gtk_widget_get_name (widget);

	/* only care about new state */
	if (!gtk_check_menu_item_get_active (GTK_CHECK_MENU_ITEM (widget))) {
		return;
	}

	/* set new filter */
	if (g_str_has_suffix (name, "_yes")) {
		pk_enums_add (application->priv->filters_current, PK_FILTER_ENUM_SOURCE);
		pk_enums_remove (application->priv->filters_current, PK_FILTER_ENUM_NOT_SOURCE);
	} else if (g_str_has_suffix (name, "_no")) {
		pk_enums_remove (application->priv->filters_current, PK_FILTER_ENUM_SOURCE);
		pk_enums_add (application->priv->filters_current, PK_FILTER_ENUM_NOT_SOURCE);
	} else {
		pk_enums_remove (application->priv->filters_current, PK_FILTER_ENUM_SOURCE);
		pk_enums_remove (application->priv->filters_current, PK_FILTER_ENUM_NOT_SOURCE);
	}

	/* refresh the sesource results */
	gpk_application_perform_search (application);
}

/**
 * gpk_application_menu_filter_basename_cb:
 * @widget: The GtkWidget object
 **/
static void
gpk_application_menu_filter_basename_cb (GtkWidget *widget, GpkApplication *application)
{
	gboolean enabled;

	g_return_if_fail (PK_IS_APPLICATION (application));

	/* save users preference to gconf */
	enabled = gtk_check_menu_item_get_active (GTK_CHECK_MENU_ITEM (widget));
	gconf_client_set_bool (application->priv->gconf_client,
			       GPK_CONF_APPLICATION_FILTER_BASENAME, enabled, NULL);

	/* change the filter */
	if (enabled) {
		pk_enums_add (application->priv->filters_current, PK_FILTER_ENUM_BASENAME);
	} else {
		pk_enums_remove (application->priv->filters_current, PK_FILTER_ENUM_BASENAME);
	}

	/* refresh the search results */
	gpk_application_perform_search (application);
}

/**
 * gpk_application_menu_filter_newest_cb:
 * @widget: The GtkWidget object
 **/
static void
gpk_application_menu_filter_newest_cb (GtkWidget *widget, GpkApplication *application)
{
	gboolean enabled;

	g_return_if_fail (PK_IS_APPLICATION (application));

	/* save users preference to gconf */
	enabled = gtk_check_menu_item_get_active (GTK_CHECK_MENU_ITEM (widget));
	gconf_client_set_bool (application->priv->gconf_client,
			       GPK_CONF_APPLICATION_FILTER_NEWEST, enabled, NULL);

	/* change the filter */
	if (enabled) {
		pk_enums_add (application->priv->filters_current, PK_FILTER_ENUM_NEWEST);
	} else {
		pk_enums_remove (application->priv->filters_current, PK_FILTER_ENUM_NEWEST);
	}

	/* refresh the search results */
	gpk_application_perform_search (application);
}

/**
 * gpk_application_status_changed_cb:
 **/
static void
gpk_application_status_changed_cb (PkClient *client, PkStatusEnum status, GpkApplication *application)
{
	const gchar *text;
	GtkWidget *widget;

	g_return_if_fail (PK_IS_APPLICATION (application));

	widget = glade_xml_get_widget (application->priv->glade_xml, "hbox_status");
	if (status == PK_STATUS_ENUM_FINISHED) {
		gtk_widget_hide (widget);
		widget = glade_xml_get_widget (application->priv->glade_xml, "image_status");
		gpk_animated_icon_enable_animation (GPK_ANIMATED_ICON (widget), FALSE);
		return;
	}

	/* set the text and show */
	gtk_widget_show (widget);
	widget = glade_xml_get_widget (application->priv->glade_xml, "label_status");
	text = gpk_status_enum_to_localised_text (status);
	gtk_label_set_label (GTK_LABEL (widget), text);

	/* set icon */
	widget = glade_xml_get_widget (application->priv->glade_xml, "image_status");
	gpk_set_animated_icon_from_status (GPK_ANIMATED_ICON (widget), status, GTK_ICON_SIZE_LARGE_TOOLBAR);
	gtk_widget_show (widget);
}

/**
 * gpk_application_allow_cancel_cb:
 **/
static void
gpk_application_allow_cancel_cb (PkClient *client, gboolean allow_cancel, GpkApplication *application)
{
	GtkWidget *widget;

	g_return_if_fail (PK_IS_APPLICATION (application));

	widget = glade_xml_get_widget (application->priv->glade_xml, "button_cancel");
	gtk_widget_set_sensitive (widget, allow_cancel);
}

/**
 * gpk_application_package_row_activated_cb:
 **/
void
gpk_application_package_row_activated_cb (GtkTreeView *treeview, GtkTreePath *path,
					 GtkTreeViewColumn *col, GpkApplication *application)
{
	GtkTreeModel *model;
	GtkTreeIter iter;
	gboolean ret;
	GpkPackageState state;

	g_return_if_fail (PK_IS_APPLICATION (application));

	/* get selection */
	model = gtk_tree_view_get_model (treeview);
	ret = gtk_tree_model_get_iter (model, &iter, path);
	if (!ret) {
		pk_warning ("failed to get selection");
		return;
	}

	g_free (application->priv->package);
	gtk_tree_model_get (model, &iter,
			    PACKAGES_COLUMN_STATE, &state,
			    PACKAGES_COLUMN_ID, &application->priv->package, -1);
	if (gpk_application_state_get_checkbox (state)) {
		gpk_application_button_remove_cb (NULL, application);
	} else {
		gpk_application_button_install_cb (NULL, application);
	}
}

/**
 * gpk_application_group_row_separator_func:
 **/
static gboolean
gpk_application_group_row_separator_func (GtkTreeModel *model, GtkTreeIter *iter, gpointer data)
{
	gchar *name = NULL;
	gboolean ret;
	gtk_tree_model_get (model, iter, GROUPS_COLUMN_ID, &name, -1);
	ret = pk_strequal (name, "separator");
	g_free (name);
	return ret;
}

/**
 * pk_application_repo_detail_cb:
 **/
static void
pk_application_repo_detail_cb (PkClient *client, const gchar *repo_id,
			       const gchar *description, gboolean enabled,
			       GpkApplication *application)
{
	g_return_if_fail (PK_IS_APPLICATION (application));

	pk_debug ("repo = %s:%s", repo_id, description);
	/* no problem, just no point adding as we will fallback to the repo_id */
	if (description == NULL) {
		return;
	}
	g_hash_table_insert (application->priv->repos, g_strdup (repo_id), g_strdup (description));
}

/**
 * gpk_application_init:
 **/
static void
gpk_application_init (GpkApplication *application)
{
	GtkWidget *main_window;
	GtkWidget *widget;
	GtkEntryCompletion *completion;
	GtkTreeModel *completion_model;
	GtkTreeSelection *selection;
	gboolean autocomplete;
	gboolean enabled;
	gchar *locale; /* does not need to be freed */
	guint i;
	gboolean ret;
	GError *error = NULL;

	application->priv = GPK_APPLICATION_GET_PRIVATE (application);
	application->priv->package = NULL;
	application->priv->group = NULL;
	application->priv->url = NULL;
	application->priv->has_package = FALSE;
	application->priv->package_list = g_ptr_array_new ();

	application->priv->gconf_client = gconf_client_get_default ();
	application->priv->repos = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);

	application->priv->search_type = PK_SEARCH_UNKNOWN;
	application->priv->search_mode = PK_MODE_UNKNOWN;
	application->priv->filters_current = PK_FILTER_ENUM_NONE;

	/* add application specific icons to search path */
	gtk_icon_theme_append_search_path (gtk_icon_theme_get_default (),
                                           PK_DATA G_DIR_SEPARATOR_S "icons");

	application->priv->control = pk_control_new ();
	application->priv->gclient = gpk_client_new ();
	gpk_client_show_finished (application->priv->gclient, FALSE);

	application->priv->client_search = pk_client_new ();
	g_signal_connect (application->priv->client_search, "package",
			  G_CALLBACK (gpk_application_package_cb), application);
	g_signal_connect (application->priv->client_search, "error-code",
			  G_CALLBACK (gpk_application_error_code_cb), application);
	g_signal_connect (application->priv->client_search, "finished",
			  G_CALLBACK (gpk_application_finished_cb), application);
	g_signal_connect (application->priv->client_search, "status-changed",
			  G_CALLBACK (gpk_application_status_changed_cb), application);
	g_signal_connect (application->priv->client_search, "allow-cancel",
			  G_CALLBACK (gpk_application_allow_cancel_cb), application);

	application->priv->client_action = pk_client_new ();
	g_signal_connect (application->priv->client_action, "package",
			  G_CALLBACK (gpk_application_package_cb), application);
	g_signal_connect (application->priv->client_action, "error-code",
			  G_CALLBACK (gpk_application_error_code_cb), application);
	g_signal_connect (application->priv->client_action, "finished",
			  G_CALLBACK (gpk_application_finished_cb), application);
	g_signal_connect (application->priv->client_action, "status-changed",
			  G_CALLBACK (gpk_application_status_changed_cb), application);
	g_signal_connect (application->priv->client_action, "allow-cancel",
			  G_CALLBACK (gpk_application_allow_cancel_cb), application);
	g_signal_connect (application->priv->client_action, "repo-detail",
			  G_CALLBACK (pk_application_repo_detail_cb), application);

	application->priv->client_details = pk_client_new ();
	g_signal_connect (application->priv->client_details, "details",
			  G_CALLBACK (gpk_application_details_cb), application);
	g_signal_connect (application->priv->client_details, "error-code",
			  G_CALLBACK (gpk_application_error_code_cb), application);
	g_signal_connect (application->priv->client_details, "finished",
			  G_CALLBACK (gpk_application_finished_cb), application);
	g_signal_connect (application->priv->client_details, "status-changed",
			  G_CALLBACK (gpk_application_status_changed_cb), application);
	g_signal_connect (application->priv->client_details, "allow-cancel",
			  G_CALLBACK (gpk_application_allow_cancel_cb), application);

	application->priv->client_files = pk_client_new ();
	pk_client_set_use_buffer (application->priv->client_files, TRUE, NULL);
	g_signal_connect (application->priv->client_files, "error-code",
			  G_CALLBACK (gpk_application_error_code_cb), application);
	g_signal_connect (application->priv->client_files, "finished",
			  G_CALLBACK (gpk_application_finished_cb), application);
	g_signal_connect (application->priv->client_files, "status-changed",
			  G_CALLBACK (gpk_application_status_changed_cb), application);
	g_signal_connect (application->priv->client_files, "allow-cancel",
			  G_CALLBACK (gpk_application_allow_cancel_cb), application);

	/* get enums */
	application->priv->roles = pk_control_get_actions (application->priv->control);
	application->priv->filters = pk_control_get_filters (application->priv->control);
	application->priv->groups = pk_control_get_groups (application->priv->control);

	application->priv->pconnection = pk_connection_new ();
	g_signal_connect (application->priv->pconnection, "connection-changed",
			  G_CALLBACK (gpk_application_connection_changed_cb), application);

	/* get localised data from sqlite database */
	application->priv->extra = pk_extra_new ();
	ret = pk_extra_set_database (application->priv->extra, NULL);
	if (!ret) {
		pk_warning ("Failure setting database");
	}

	/* set the locale */
	locale = setlocale (LC_ALL, NULL);
	pk_extra_set_locale (application->priv->extra, locale);

	/* use custom widgets */
	glade_set_custom_handler (gpk_application_create_custom_widget, application);

	application->priv->glade_xml = glade_xml_new (PK_DATA "/gpk-application.glade", NULL, NULL);
	main_window = glade_xml_get_widget (application->priv->glade_xml, "window_manager");

	/* make GpkClient windows modal */
	gpk_client_set_parent (application->priv->gclient, GTK_WINDOW (main_window));

	/* Hide window first so that the dialogue resizes itself without redrawing */
	gtk_widget_hide (main_window);
	gtk_window_set_icon_name (GTK_WINDOW (main_window), PK_STOCK_APP_ICON);

	/* Get the main window quit */
	g_signal_connect (main_window, "delete_event",
			  G_CALLBACK (gpk_application_delete_event_cb), application);

	/* install */
	widget = glade_xml_get_widget (application->priv->glade_xml, "toolbutton_install");
	g_signal_connect (widget, "clicked",
			  G_CALLBACK (gpk_application_button_install_cb), application);
	gtk_widget_set_tooltip_text (widget, _("Add current selection"));
	gtk_widget_set_sensitive (widget, FALSE);

	/* remove */
	widget = glade_xml_get_widget (application->priv->glade_xml, "toolbutton_remove");
	g_signal_connect (widget, "clicked",
			  G_CALLBACK (gpk_application_button_remove_cb), application);
	gtk_widget_set_tooltip_text (widget, _("Remove current selection"));
	gtk_widget_set_sensitive (widget, FALSE);

	/* clear */
	widget = glade_xml_get_widget (application->priv->glade_xml, "toolbutton_clear");
	g_signal_connect (widget, "clicked",
			  G_CALLBACK (gpk_application_button_clear_cb), application);
	gtk_widget_set_tooltip_text (widget, _("Clear current selection"));

	/* install */
	widget = glade_xml_get_widget (application->priv->glade_xml, "toolbutton_apply");
	g_signal_connect (widget, "clicked",
			  G_CALLBACK (gpk_application_button_apply_cb), application);

	widget = glade_xml_get_widget (application->priv->glade_xml, "menuitem_about");
	g_signal_connect (widget, "activate",
			  G_CALLBACK (gpk_application_menu_about_cb), application);

	widget = glade_xml_get_widget (application->priv->glade_xml, "menuitem_help");
	g_signal_connect (widget, "activate",
			  G_CALLBACK (gpk_application_menu_help_cb), application);

	widget = glade_xml_get_widget (application->priv->glade_xml, "menuitem_sources");
	g_signal_connect (widget, "activate",
			  G_CALLBACK (gpk_application_menu_sources_cb), application);

	widget = glade_xml_get_widget (application->priv->glade_xml, "menuitem_refresh");
	g_signal_connect (widget, "activate",
			  G_CALLBACK (gpk_application_menu_refresh_cb), application);

	widget = glade_xml_get_widget (application->priv->glade_xml, "menuitem_homepage");
	g_signal_connect (widget, "activate",
			  G_CALLBACK (gpk_application_menu_homepage_cb), application);
	gtk_widget_set_tooltip_text (widget, _("Visit homepage for selected package"));

	widget = glade_xml_get_widget (application->priv->glade_xml, "menuitem_files");
	g_signal_connect (widget, "activate",
			  G_CALLBACK (gpk_application_menu_files_cb), application);

	widget = glade_xml_get_widget (application->priv->glade_xml, "menuitem_install");
	g_signal_connect (widget, "activate",
			  G_CALLBACK (gpk_application_menu_install_cb), application);

	widget = glade_xml_get_widget (application->priv->glade_xml, "menuitem_remove");
	g_signal_connect (widget, "activate",
			  G_CALLBACK (gpk_application_menu_remove_cb), application);

	widget = glade_xml_get_widget (application->priv->glade_xml, "menuitem_depends");
	g_signal_connect (widget, "activate",
			  G_CALLBACK (gpk_application_menu_depends_cb), application);

	widget = glade_xml_get_widget (application->priv->glade_xml, "menuitem_requires");
	g_signal_connect (widget, "activate",
			  G_CALLBACK (gpk_application_menu_requires_cb), application);

	widget = glade_xml_get_widget (application->priv->glade_xml, "menuitem_selection");
	gtk_widget_hide (widget);

	/* installed filter */
	widget = glade_xml_get_widget (application->priv->glade_xml, "menuitem_installed_yes");
	g_signal_connect (widget, "toggled",
			  G_CALLBACK (gpk_application_menu_filter_installed_cb), application);
	widget = glade_xml_get_widget (application->priv->glade_xml, "menuitem_installed_no");
	g_signal_connect (widget, "toggled",
			  G_CALLBACK (gpk_application_menu_filter_installed_cb), application);
	widget = glade_xml_get_widget (application->priv->glade_xml, "menuitem_installed_both");
	g_signal_connect (widget, "toggled",
			  G_CALLBACK (gpk_application_menu_filter_installed_cb), application);

	/* devel filter */
	widget = glade_xml_get_widget (application->priv->glade_xml, "menuitem_devel_yes");
	g_signal_connect (widget, "toggled",
			  G_CALLBACK (gpk_application_menu_filter_devel_cb), application);
	widget = glade_xml_get_widget (application->priv->glade_xml, "menuitem_devel_no");
	g_signal_connect (widget, "toggled",
			  G_CALLBACK (gpk_application_menu_filter_devel_cb), application);
	widget = glade_xml_get_widget (application->priv->glade_xml, "menuitem_devel_both");
	g_signal_connect (widget, "toggled",
			  G_CALLBACK (gpk_application_menu_filter_devel_cb), application);

	/* gui filter */
	widget = glade_xml_get_widget (application->priv->glade_xml, "menuitem_gui_yes");
	g_signal_connect (widget, "toggled",
			  G_CALLBACK (gpk_application_menu_filter_gui_cb), application);
	widget = glade_xml_get_widget (application->priv->glade_xml, "menuitem_gui_no");
	g_signal_connect (widget, "toggled",
			  G_CALLBACK (gpk_application_menu_filter_gui_cb), application);
	widget = glade_xml_get_widget (application->priv->glade_xml, "menuitem_gui_both");
	g_signal_connect (widget, "toggled",
			  G_CALLBACK (gpk_application_menu_filter_gui_cb), application);

	/* free filter */
	widget = glade_xml_get_widget (application->priv->glade_xml, "menuitem_free_yes");
	g_signal_connect (widget, "toggled",
			  G_CALLBACK (gpk_application_menu_filter_free_cb), application);
	widget = glade_xml_get_widget (application->priv->glade_xml, "menuitem_free_no");
	g_signal_connect (widget, "toggled",
			  G_CALLBACK (gpk_application_menu_filter_free_cb), application);
	widget = glade_xml_get_widget (application->priv->glade_xml, "menuitem_free_both");
	g_signal_connect (widget, "toggled",
			  G_CALLBACK (gpk_application_menu_filter_free_cb), application);

	/* arch filter */
	widget = glade_xml_get_widget (application->priv->glade_xml, "menuitem_arch_yes");
	g_signal_connect (widget, "toggled",
			  G_CALLBACK (gpk_application_menu_filter_arch_cb), application);
	widget = glade_xml_get_widget (application->priv->glade_xml, "menuitem_arch_no");
	g_signal_connect (widget, "toggled",
			  G_CALLBACK (gpk_application_menu_filter_arch_cb), application);
	widget = glade_xml_get_widget (application->priv->glade_xml, "menuitem_arch_both");
	g_signal_connect (widget, "toggled",
			  G_CALLBACK (gpk_application_menu_filter_arch_cb), application);

	/* source filter */
	widget = glade_xml_get_widget (application->priv->glade_xml, "menuitem_source_yes");
	g_signal_connect (widget, "toggled",
			  G_CALLBACK (gpk_application_menu_filter_source_cb), application);
	widget = glade_xml_get_widget (application->priv->glade_xml, "menuitem_source_no");
	g_signal_connect (widget, "toggled",
			  G_CALLBACK (gpk_application_menu_filter_source_cb), application);
	widget = glade_xml_get_widget (application->priv->glade_xml, "menuitem_source_both");
	g_signal_connect (widget, "toggled",
			  G_CALLBACK (gpk_application_menu_filter_source_cb), application);

	widget = glade_xml_get_widget (application->priv->glade_xml, "vbox_description");
	gtk_widget_hide (widget);

	widget = glade_xml_get_widget (application->priv->glade_xml, "hbox_filesize");
	gtk_widget_hide (widget);

	widget = glade_xml_get_widget (application->priv->glade_xml, "hbox_source");
	gtk_widget_hide (widget);

	/* basename filter */
	widget = glade_xml_get_widget (application->priv->glade_xml, "menuitem_basename");
	g_signal_connect (widget, "toggled",
			  G_CALLBACK (gpk_application_menu_filter_basename_cb), application);

	/* newest filter */
	widget = glade_xml_get_widget (application->priv->glade_xml, "menuitem_newest");
	g_signal_connect (widget, "toggled",
			  G_CALLBACK (gpk_application_menu_filter_newest_cb), application);

	/* Remove description/file list if needed. */
	if (pk_enums_contain (application->priv->roles, PK_ROLE_ENUM_GET_DETAILS) == FALSE) {
		widget = glade_xml_get_widget (application->priv->glade_xml, "scrolledwindow2");
		gtk_widget_hide (widget);
	}
	if (pk_enums_contain (application->priv->roles, PK_ROLE_ENUM_GET_FILES) == FALSE) {
		widget = glade_xml_get_widget (application->priv->glade_xml, "menuitem_files");
		gtk_widget_hide (widget);
	}
	if (pk_enums_contain (application->priv->roles, PK_ROLE_ENUM_GET_DEPENDS) == FALSE) {
		widget = glade_xml_get_widget (application->priv->glade_xml, "menuitem_depends");
		gtk_widget_hide (widget);
	}
	if (pk_enums_contain (application->priv->roles, PK_ROLE_ENUM_GET_REQUIRES) == FALSE) {
		widget = glade_xml_get_widget (application->priv->glade_xml, "menuitem_requires");
		gtk_widget_hide (widget);
	}

	/* hide the group selector if we don't support search-groups */
	if (pk_enums_contain (application->priv->roles, PK_ROLE_ENUM_SEARCH_GROUP) == FALSE) {
		widget = glade_xml_get_widget (application->priv->glade_xml, "scrolledwindow_groups");
		gtk_widget_hide (widget);
	}

	/* hide the refresh cache button if we can't do it */
	if (pk_enums_contain (application->priv->roles, PK_ROLE_ENUM_REFRESH_CACHE) == FALSE) {
		widget = glade_xml_get_widget (application->priv->glade_xml, "menuitem_refresh");
		gtk_widget_hide (widget);
	}

	/* hide the software-sources button if we can't do it */
	if (pk_enums_contain (application->priv->roles, PK_ROLE_ENUM_GET_REPO_LIST) == FALSE) {
		widget = glade_xml_get_widget (application->priv->glade_xml, "menuitem_sources");
		gtk_widget_hide (widget);
	}

	/* simple find button */
	widget = glade_xml_get_widget (application->priv->glade_xml, "button_find");
	g_signal_connect (widget, "clicked",
			  G_CALLBACK (gpk_application_find_cb), application);
	gtk_widget_set_tooltip_text (widget, _("Find packages"));

	/* search cancel button */
	widget = glade_xml_get_widget (application->priv->glade_xml, "button_cancel");
	g_signal_connect (widget, "clicked",
			  G_CALLBACK (gpk_application_cancel_cb), application);
	gtk_widget_set_tooltip_text (widget, _("Cancel search"));

	/* the fancy text entry widget */
	widget = glade_xml_get_widget (application->priv->glade_xml, "entry_text");

	/* autocompletion can be turned off as it's slow */
	autocomplete = gconf_client_get_bool (application->priv->gconf_client, GPK_CONF_AUTOCOMPLETE, NULL);
	if (autocomplete) {
		/* create the completion object */
		completion = gtk_entry_completion_new ();

		/* assign the completion to the entry */
		gtk_entry_set_completion (GTK_ENTRY (widget), completion);
		g_object_unref (completion);

		/* create a tree model and use it as the completion model */
		completion_model = gpk_application_create_completion_model ();
		gtk_entry_completion_set_model (completion, completion_model);
		g_object_unref (completion_model);

		/* use model column 0 as the text column */
		gtk_entry_completion_set_text_column (completion, 0);
		gtk_entry_completion_set_inline_completion (completion, TRUE);
	}

	/* set focus on entry text */
	gtk_widget_grab_focus (widget);
	gtk_widget_show (widget);
	sexy_icon_entry_set_icon_highlight (SEXY_ICON_ENTRY (widget), SEXY_ICON_ENTRY_PRIMARY, TRUE);
	g_signal_connect (widget, "activate",
			  G_CALLBACK (gpk_application_find_cb), application);
	g_signal_connect (widget, "icon-pressed",
			  G_CALLBACK (gpk_application_entry_text_icon_pressed_cb), application);

	/* coldplug icon to default to search by name*/
	gpk_application_menu_search_by_name (NULL, application);

	/* hide the filters we can't support */
	if (pk_enums_contain (application->priv->filters, PK_FILTER_ENUM_INSTALLED) == FALSE) {
		widget = glade_xml_get_widget (application->priv->glade_xml, "menuitem_installed");
		gtk_widget_hide (widget);
	}
	if (pk_enums_contain (application->priv->filters, PK_FILTER_ENUM_DEVELOPMENT) == FALSE) {
		widget = glade_xml_get_widget (application->priv->glade_xml, "menuitem_devel");
		gtk_widget_hide (widget);
	}
	if (pk_enums_contain (application->priv->filters, PK_FILTER_ENUM_GUI) == FALSE) {
		widget = glade_xml_get_widget (application->priv->glade_xml, "menuitem_gui");
		gtk_widget_hide (widget);
	}
	if (pk_enums_contain (application->priv->filters, PK_FILTER_ENUM_FREE) == FALSE) {
		widget = glade_xml_get_widget (application->priv->glade_xml, "menuitem_free");
		gtk_widget_hide (widget);
	}
	if (pk_enums_contain (application->priv->filters, PK_FILTER_ENUM_ARCH) == FALSE) {
		widget = glade_xml_get_widget (application->priv->glade_xml, "menuitem_arch");
		gtk_widget_hide (widget);
	}
	if (pk_enums_contain (application->priv->filters, PK_FILTER_ENUM_SOURCE) == FALSE) {
		widget = glade_xml_get_widget (application->priv->glade_xml, "menuitem_source");
		gtk_widget_hide (widget);
	}

	/* BASENAME, use by default, or hide */
	widget = glade_xml_get_widget (application->priv->glade_xml, "menuitem_basename");
	if (pk_enums_contain (application->priv->filters, PK_FILTER_ENUM_BASENAME)) {
		enabled = gconf_client_get_bool (application->priv->gconf_client,
						 GPK_CONF_APPLICATION_FILTER_BASENAME, NULL);
		gtk_check_menu_item_set_active (GTK_CHECK_MENU_ITEM (widget), enabled);
		/* work round a gtk2+ bug: toggled should be fired when doing gtk_check_menu_item_set_active */
		gpk_application_menu_filter_basename_cb (widget, application);
	} else {
		gtk_widget_hide (widget);
	}

	/* NEWEST, use by default, or hide */
	widget = glade_xml_get_widget (application->priv->glade_xml, "menuitem_newest");
	if (pk_enums_contain (application->priv->filters, PK_FILTER_ENUM_NEWEST)) {
		/* set from remembered state */
		enabled = gconf_client_get_bool (application->priv->gconf_client,
						 GPK_CONF_APPLICATION_FILTER_NEWEST, NULL);
		gtk_check_menu_item_set_active (GTK_CHECK_MENU_ITEM (widget), enabled);
		/* work round a gtk2+ bug: toggled should be fired when doing gtk_check_menu_item_set_active */
		gpk_application_menu_filter_newest_cb (widget, application);
	} else {
		gtk_widget_hide (widget);
	}

	widget = glade_xml_get_widget (application->priv->glade_xml, "entry_text");
	g_signal_connect (widget, "key-press-event",
			  G_CALLBACK (gpk_application_text_changed_cb), application);
	g_signal_connect (widget, "key-release-event",
			  G_CALLBACK (gpk_application_text_changed_cb), application);

	widget = glade_xml_get_widget (application->priv->glade_xml, "button_find");
	gtk_widget_set_sensitive (widget, FALSE);

	gtk_widget_set_size_request (main_window, 1000, 500);
	gtk_widget_show (main_window);

	widget = glade_xml_get_widget (application->priv->glade_xml, "treeview_packages");
	gtk_tree_view_columns_autosize (GTK_TREE_VIEW (widget));
	g_signal_connect (GTK_TREE_VIEW (widget), "row-activated",
			  G_CALLBACK (gpk_application_package_row_activated_cb), application);

	/* create list stores */
	application->priv->packages_store = gtk_list_store_new (PACKAGES_COLUMN_LAST,
							        G_TYPE_STRING,
							        G_TYPE_UINT,
							        G_TYPE_BOOLEAN,
							        G_TYPE_BOOLEAN,
							        G_TYPE_STRING,
							        G_TYPE_STRING);
	application->priv->groups_store = gtk_list_store_new (GROUPS_COLUMN_LAST,
							      G_TYPE_STRING,
							      G_TYPE_STRING,
							      G_TYPE_STRING);

	/* unsorted */
	gpk_application_treeview_set_sorted (application, FALSE);

	/* create package tree view */
	widget = glade_xml_get_widget (application->priv->glade_xml, "treeview_packages");
	gtk_tree_view_set_model (GTK_TREE_VIEW (widget),
				 GTK_TREE_MODEL (application->priv->packages_store));

	selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (widget));
	g_signal_connect (selection, "changed",
			  G_CALLBACK (gpk_application_packages_treeview_clicked_cb), application);

	/* add columns to the tree view */
	gpk_application_packages_add_columns (application);

	/* add an "all" entry if we can GetPackages */
	if (pk_enums_contain (application->priv->roles, PK_ROLE_ENUM_GET_PACKAGES)) {
		GtkTreeIter iter;
		const gchar *icon_name;
		gtk_list_store_append (application->priv->groups_store, &iter);
		icon_name = gpk_role_enum_to_icon_name (PK_ROLE_ENUM_GET_PACKAGES);
		gtk_list_store_set (application->priv->groups_store, &iter,
				    GROUPS_COLUMN_NAME, _("All packages"),
				    GROUPS_COLUMN_ID, "all-packages",
				    GROUPS_COLUMN_ICON, icon_name, -1);
		/* add a separator */
		gtk_list_store_append (application->priv->groups_store, &iter);
		gtk_list_store_set (application->priv->groups_store, &iter,
				    GROUPS_COLUMN_ID, "separator", -1);
	}

	/* create group tree view if we can search by group */
	if (pk_enums_contain (application->priv->roles, PK_ROLE_ENUM_SEARCH_GROUP)) {
		widget = glade_xml_get_widget (application->priv->glade_xml, "treeview_groups");
		gtk_tree_view_set_model (GTK_TREE_VIEW (widget),
					 GTK_TREE_MODEL (application->priv->groups_store));

		selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (widget));
		g_signal_connect (selection, "changed",
				  G_CALLBACK (gpk_application_groups_treeview_clicked_cb), application);

		/* add columns to the tree view */
		gpk_application_groups_add_columns (GTK_TREE_VIEW (widget));

		/* add all the groups supported */
		for (i=1; i<PK_GROUP_ENUM_UNKNOWN; i*=2) {
			if (pk_enums_contain (application->priv->groups, i)) {
				gpk_application_group_add_data (application, i);
			}
		}
	}

	/* use a seporator */
	gtk_tree_view_set_row_separator_func (GTK_TREE_VIEW (widget),
					      gpk_application_group_row_separator_func, NULL, NULL);

	/* get repos, so we can show the full name in the software source box */
	ret = pk_client_get_repo_list (application->priv->client_action, PK_FILTER_ENUM_NONE, &error);
	if (!ret) {
		pk_warning ("failed to get repo list: %s", error->message);
		g_error_free (error);
	}

	/* set current action */
	application->priv->action = PK_ACTION_NONE;
	gpk_application_set_buttons_apply_clear (application);
}

/**
 * gpk_application_finalize:
 * @object: This graph class instance
 **/
static void
gpk_application_finalize (GObject *object)
{
	GpkApplication *application;
	g_return_if_fail (PK_IS_APPLICATION (object));

	application = GPK_APPLICATION (object);
	application->priv = GPK_APPLICATION_GET_PRIVATE (application);

	g_object_unref (application->priv->glade_xml);
	g_object_unref (application->priv->packages_store);
	g_object_unref (application->priv->control);
	g_object_unref (application->priv->client_search);
	g_object_unref (application->priv->client_action);
	g_object_unref (application->priv->client_details);
	g_object_unref (application->priv->client_files);
	g_object_unref (application->priv->pconnection);
	g_object_unref (application->priv->extra);
	g_object_unref (application->priv->gconf_client);
	g_object_unref (application->priv->gclient);

	g_free (application->priv->url);
	g_free (application->priv->group);
	g_free (application->priv->package);
	g_hash_table_destroy (application->priv->repos);
	g_ptr_array_free (application->priv->package_list, TRUE);

	G_OBJECT_CLASS (gpk_application_parent_class)->finalize (object);
}

/**
 * gpk_application_new:
 * Return value: new GpkApplication instance.
 **/
GpkApplication *
gpk_application_new (void)
{
	GpkApplication *application;
	application = g_object_new (GPK_TYPE_APPLICATION, NULL);
	return GPK_APPLICATION (application);
}

