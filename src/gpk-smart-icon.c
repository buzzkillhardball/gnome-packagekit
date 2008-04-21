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

#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <errno.h>

#include <string.h>
#include <sys/time.h>
#include <sys/types.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif /* HAVE_UNISTD_H */
#include <glib/gi18n.h>

#include <gtk/gtk.h>
#include <libnotify/notify.h>
#include <gtk/gtkstatusicon.h>
#include <libnotify/notify.h>
#include <gconf/gconf-client.h>

#include <pk-debug.h>
#include <pk-enum.h>
#include "gpk-marshal.h"
#include "gpk-common.h"
#include "gpk-smart-icon.h"

static void     gpk_smart_icon_class_init	(GpkSmartIconClass *klass);
static void     gpk_smart_icon_init		(GpkSmartIcon      *sicon);
static void     gpk_smart_icon_finalize		(GObject       *object);

#define PK_SMART_ICON_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), PK_TYPE_SMART_ICON, GpkSmartIconPrivate))
#define PK_SMART_ICON_PERSIST_TIMEOUT	100

struct GpkSmartIconPrivate
{
	GtkStatusIcon		*status_icon;
	NotifyNotification	*dialog;
	GConfClient		*gconf_client;
	gchar			*current;
	gchar			*new;
	gchar			*notify_data;
	guint			 event_source;
	guint			 pulse_source;
	gboolean		 has_gconf_check;
	gfloat			 icon_opacity;
	gboolean		 going_down;
};

enum {
	NOTIFICATION_BUTTON,
	LAST_SIGNAL
};

G_DEFINE_TYPE (GpkSmartIcon, gpk_smart_icon, G_TYPE_OBJECT)

static guint signals [LAST_SIGNAL] = { 0 };

static PkEnumMatch enum_button_ids[] = {
	{GPK_NOTIFY_BUTTON_UNKNOWN,		"unknown"},	/* fall though value */
	{GPK_NOTIFY_BUTTON_DO_NOT_SHOW_AGAIN,	"do-not-show-again"},
	{GPK_NOTIFY_BUTTON_DO_NOT_WARN_AGAIN,	"do-not-warn-again"},
	{GPK_NOTIFY_BUTTON_CANCEL_UPDATE,	"cancel-update"},
	{GPK_NOTIFY_BUTTON_UPDATE_COMPUTER,	"update-computer"},
	{GPK_NOTIFY_BUTTON_RESTART_COMPUTER,	"restart-computer"},
	{0, NULL}
};

/**
 * gpk_smart_icon_class_init:
 * @klass: The GpkSmartIconClass
 **/
static void
gpk_smart_icon_class_init (GpkSmartIconClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	object_class->finalize = gpk_smart_icon_finalize;
	g_type_class_add_private (klass, sizeof (GpkSmartIconPrivate));
	signals [NOTIFICATION_BUTTON] =
		g_signal_new ("notification-button",
			      G_TYPE_FROM_CLASS (object_class), G_SIGNAL_RUN_LAST,
			      0, NULL, NULL, gpk_marshal_VOID__UINT_STRING,
			      G_TYPE_NONE, 2, G_TYPE_UINT, G_TYPE_STRING);
}

/**
 * gpk_smart_icon_set_pixmap_opacity:
 **/
static gboolean
gpk_smart_icon_set_pixmap_opacity (GdkPixbuf *pixbuf, gfloat adjust)
{
	gint width, height, rowstride, n_channels;
	guchar *pixels, *p;
	gint x, y;

	width = gdk_pixbuf_get_width (pixbuf);
	height = gdk_pixbuf_get_height (pixbuf);
	rowstride = gdk_pixbuf_get_rowstride (pixbuf);
	n_channels = gdk_pixbuf_get_n_channels (pixbuf);
	pixels = gdk_pixbuf_get_pixels (pixbuf);

	/* scale the opacity of each pixel */
	for (y=0; y<height-1;y++) {
		for (x=0; x<height-1;x++) {
			p = pixels + y * rowstride + x * n_channels;
			p[3] = (gfloat) p[3] * adjust;
		}
	}
	return TRUE;
}

/**
 * gpk_smart_icon_pulse_timeout_cb:
 **/
static gboolean
gpk_smart_icon_pulse_timeout_cb (gpointer data)
{
	GpkSmartIcon *sicon = (GpkSmartIcon *) data;
	GdkPixbuf *pixbuf;
	GdkRectangle area;

	g_return_val_if_fail (PK_IS_SMART_ICON (sicon), FALSE);

	/* have we hidden the icon already? */
	if (sicon->priv->current == NULL || sicon->priv->new == NULL) {
		pk_debug ("not pulsing as icon cleared");
		return FALSE;
	}

	/* get pixmap the same size as the original icon */
	gtk_status_icon_get_geometry (GTK_STATUS_ICON (sicon->priv->status_icon), NULL, &area, NULL);
	pixbuf = gtk_icon_theme_load_icon (gtk_icon_theme_get_default (), sicon->priv->current, area.width, 0, NULL);

	/* set the new pixmap with the correct opacity */
	gpk_smart_icon_set_pixmap_opacity (pixbuf, sicon->priv->icon_opacity);
	gtk_status_icon_set_from_pixbuf (GTK_STATUS_ICON (sicon->priv->status_icon), pixbuf);
	g_object_unref (pixbuf);

	/* dimming down */
	if (sicon->priv->going_down) {
		sicon->priv->icon_opacity -= 0.1;
		if (sicon->priv->icon_opacity<0) {
			sicon->priv->icon_opacity = 0;
			sicon->priv->going_down = FALSE;
		}
		return TRUE;
	}

	/* dimming up */
	sicon->priv->icon_opacity += 0.1;
	if (sicon->priv->icon_opacity>1) {
		/* restore */
		gtk_status_icon_set_from_icon_name (GTK_STATUS_ICON (sicon->priv->status_icon), sicon->priv->current);
		sicon->priv->pulse_source = 0;
		return FALSE;
	}
	return TRUE;
}

/**
 * gpk_smart_icon_pulse:
 **/
gboolean
gpk_smart_icon_pulse (GpkSmartIcon *sicon)
{
	g_return_val_if_fail (PK_IS_SMART_ICON (sicon), FALSE);

	sicon->priv->icon_opacity = 0.9;
	sicon->priv->going_down = TRUE;
	if (sicon->priv->pulse_source != 0) {
		pk_warning ("already pulsing");
		return FALSE;
	}
	sicon->priv->pulse_source = g_timeout_add (20, gpk_smart_icon_pulse_timeout_cb, sicon);
	return TRUE;
}

/**
 * gpk_smart_icon_set_icon_name_cb:
 **/
static gboolean
gpk_smart_icon_set_icon_name_cb (gpointer data)
{
	GpkSmartIcon *sicon = (GpkSmartIcon *) data;

	/* no point setting the same */
	if (sicon->priv->new != NULL &&
	    sicon->priv->current != NULL &&
	    strcmp (sicon->priv->new, sicon->priv->current) == 0) {
		return FALSE;
	}
	if (sicon->priv->new == NULL &&
	    sicon->priv->current == NULL) {
		return FALSE;
	}

	/* save what we have */
	g_free (sicon->priv->current);
	sicon->priv->current = g_strdup (sicon->priv->new);

	/* set the correct thing */
	if (sicon->priv->new == NULL) {
		gtk_status_icon_set_visible (GTK_STATUS_ICON (sicon->priv->status_icon), FALSE);
	} else {
		gtk_status_icon_set_from_icon_name (GTK_STATUS_ICON (sicon->priv->status_icon), sicon->priv->new);
		gtk_status_icon_set_visible (GTK_STATUS_ICON (sicon->priv->status_icon), TRUE);
	}
	return FALSE;
}

/**
 * gpk_smart_icon_set_icon:
 **/
gboolean
gpk_smart_icon_set_icon_name (GpkSmartIcon *sicon, const gchar *icon_name)
{
	g_return_val_if_fail (PK_IS_SMART_ICON (sicon), FALSE);

	/* if we have a request pending, then cancel it in preference to this one */
	if (sicon->priv->event_source != 0) {
		g_source_remove (sicon->priv->event_source);
		sicon->priv->event_source = 0;
	}
	/* tell us what we -want- */
	g_free (sicon->priv->new);
	pk_debug ("setting icon name %s", icon_name);
	sicon->priv->new = g_strdup (icon_name);

	/* wait a little while to see if it's worth displaying the icon */
	sicon->priv->event_source = g_timeout_add (PK_SMART_ICON_PERSIST_TIMEOUT, gpk_smart_icon_set_icon_name_cb, sicon);
	return TRUE;
}

/**
 * gpk_smart_icon_sync:
 **/
gboolean
gpk_smart_icon_sync (GpkSmartIcon *sicon)
{
	g_return_val_if_fail (PK_IS_SMART_ICON (sicon), FALSE);

	/* if we have a request pending, then cancel it in preference to this one */
	if (sicon->priv->event_source != 0) {
		g_source_remove (sicon->priv->event_source);
		sicon->priv->event_source = 0;
	}

	/* sync the icon now */
	gpk_smart_icon_set_icon_name_cb (sicon);

	/* wait until we are in the panel.
	 * We should probably use gtk_status_icon_is_embedded if it worked... */
	g_usleep (50000);

	return TRUE;
}

/**
 * gpk_smart_icon_get_status_icon:
 **/
GtkStatusIcon *
gpk_smart_icon_get_status_icon (GpkSmartIcon *sicon)
{
	g_return_val_if_fail (PK_IS_SMART_ICON (sicon), NULL);
	return sicon->priv->status_icon;
}

/**
 * gpk_smart_icon_set_tooltip:
 **/
gboolean
gpk_smart_icon_set_tooltip (GpkSmartIcon *sicon, const gchar *tooltip)
{
	g_return_val_if_fail (PK_IS_SMART_ICON (sicon), FALSE);
	gtk_status_icon_set_tooltip (GTK_STATUS_ICON (sicon->priv->status_icon), tooltip);
	return TRUE;
}

/**
 * gpk_smart_icon_notify:
 **/
gboolean
gpk_smart_icon_notify_new (GpkSmartIcon *sicon, const gchar *title, const gchar *message,
			   const gchar *icon, GpkNotifyUrgency urgency, GpkNotifyTimeout timeout)
{
	guint timeout_val = 0;

	g_return_val_if_fail (PK_IS_SMART_ICON (sicon), FALSE);

	pk_debug ("Doing notification: %s, %s, %s", title, message, icon);

	/* no gconf to check */
	sicon->priv->has_gconf_check = FALSE;

	/* default values */
	if (timeout == GPK_NOTIFY_TIMEOUT_SHORT) {
		timeout_val = 5000;
	} else if (timeout == GPK_NOTIFY_TIMEOUT_LONG) {
		timeout_val = 15000;
	}

	if (gtk_status_icon_get_visible (sicon->priv->status_icon)) {
		sicon->priv->dialog = notify_notification_new_with_status_icon (title, message, icon, sicon->priv->status_icon);
	} else {
		sicon->priv->dialog = notify_notification_new (title, message, icon, NULL);
	}
	notify_notification_set_timeout (sicon->priv->dialog, timeout_val);
	notify_notification_set_urgency (sicon->priv->dialog, (NotifyUrgency) urgency);
	return TRUE;
}

/**
 * gpk_smart_icon_libnotify_cb:
 **/
static void
gpk_smart_icon_libnotify_cb (NotifyNotification *dialog, gchar *action, GpkSmartIcon *sicon)
{
	GpkNotifyButton button;

	g_return_if_fail (PK_IS_SMART_ICON (sicon));

	/* get the value */
	button = pk_enum_find_value (enum_button_ids, action);

	/* send a signal with the type and data */
	pk_debug ("emit: %s with data %s", action, sicon->priv->notify_data);
	g_signal_emit (sicon, signals [NOTIFICATION_BUTTON], 0, button, sicon->priv->notify_data);
}

/**
 * gpk_smart_icon_notify_button:
 **/
gboolean
gpk_smart_icon_notify_button (GpkSmartIcon *sicon, GpkNotifyButton button, const gchar *data)
{
	const gchar *text = NULL;
	const gchar *id = NULL;

	g_return_val_if_fail (PK_IS_SMART_ICON (sicon), FALSE);

	/* get the id */
	id = pk_enum_find_string (enum_button_ids, button);

	/* find the localised text */
	if (button == GPK_NOTIFY_BUTTON_DO_NOT_SHOW_AGAIN) {
		text = _("Do not show this notification again");
		sicon->priv->has_gconf_check = TRUE;
	} else if (button == GPK_NOTIFY_BUTTON_DO_NOT_WARN_AGAIN) {
		text = _("Do not warn me again");
		sicon->priv->has_gconf_check = TRUE;
	} else if (button == GPK_NOTIFY_BUTTON_CANCEL_UPDATE) {
		text = _("Cancel system update");
	} else if (button == GPK_NOTIFY_BUTTON_UPDATE_COMPUTER) {
		text = _("Update computer now");
	} else if (button == GPK_NOTIFY_BUTTON_RESTART_COMPUTER) {
		text = _("Restart computer now");
	}

	/* save data privately, TODO: this really needs to be in a hashtable */
	sicon->priv->notify_data = g_strdup (data);

	/* add a button to the UI */
	notify_notification_add_action (sicon->priv->dialog, id, text, (NotifyActionCallback) gpk_smart_icon_libnotify_cb, sicon, NULL);
	return FALSE;
}

/**
 * gpk_smart_icon_notify_show:
 * Return value: if the notification is being displayed
 *
 * This will show the notification previously setup with gpk_smart_icon_notify_new() and
 * gpk_smart_icon_notify_button().
 *
 * If you set a key using %GPK_NOTIFY_BUTTON_DO_NOT_SHOW_AGAIN or
 * %GPK_NOTIFY_BUTTON_DO_NOT_WARN_AGAIN then this key will be checked before the notification is
 * shown.
 **/
gboolean
gpk_smart_icon_notify_show (GpkSmartIcon *sicon)
{
	GError *error = NULL;
	gboolean value;

	g_return_val_if_fail (PK_IS_SMART_ICON (sicon), FALSE);
	g_return_val_if_fail (sicon->priv->dialog != NULL, FALSE);

	/* check the gconf key isn't set to ignore */
	if (sicon->priv->has_gconf_check) {
		pk_debug ("key is %s", sicon->priv->notify_data);
		/* are we accepting notifications */
		value = gconf_client_get_bool (sicon->priv->gconf_client, sicon->priv->notify_data, NULL);
		if (!value) {
			pk_debug ("not showing notification as prevented in gconf with %s", sicon->priv->notify_data);
			return FALSE;
		}
	}

	notify_notification_close (sicon->priv->dialog, NULL);
	notify_notification_show (sicon->priv->dialog, &error);
	if (error != NULL) {
		pk_warning ("error: %s", error->message);
		g_error_free (error);
		return FALSE;
	}
	return TRUE;
}

/**
 * gpk_smart_icon_notify_close:
 **/
gboolean
gpk_smart_icon_notify_close (GpkSmartIcon *sicon)
{
	g_return_val_if_fail (PK_IS_SMART_ICON (sicon), FALSE);
	notify_notification_close (sicon->priv->dialog, NULL);
	return TRUE;
}

/**
 * gpk_smart_icon_init:
 * @smart_icon: This class instance
 **/
static void
gpk_smart_icon_init (GpkSmartIcon *sicon)
{
	sicon->priv = PK_SMART_ICON_GET_PRIVATE (sicon);
	sicon->priv->status_icon = gtk_status_icon_new ();
	sicon->priv->new = NULL;
	sicon->priv->current = NULL;
	sicon->priv->dialog = NULL;
	sicon->priv->notify_data = NULL;
	sicon->priv->event_source = 0;
	sicon->priv->pulse_source = 0;
	sicon->priv->has_gconf_check = FALSE;
	sicon->priv->gconf_client = gconf_client_get_default ();

	/* signal we are here... */
	notify_init ("packagekit");

	gtk_status_icon_set_visible (GTK_STATUS_ICON (sicon->priv->status_icon), FALSE);
}

/**
 * gpk_smart_icon_finalize:
 * @object: The object to finalize
 **/
static void
gpk_smart_icon_finalize (GObject *object)
{
	GpkSmartIcon *sicon;

	g_return_if_fail (PK_IS_SMART_ICON (object));

	sicon = PK_SMART_ICON (object);
	g_return_if_fail (sicon->priv != NULL);

	/* remove any timers that may be firing */
	if (sicon->priv->event_source != 0) {
		g_source_remove (sicon->priv->event_source);
	}
	if (sicon->priv->pulse_source != 0) {
		g_source_remove (sicon->priv->pulse_source);
	}

	g_free (sicon->priv->new);
	g_free (sicon->priv->current);
	g_object_unref (sicon->priv->gconf_client);

	g_object_unref (sicon->priv->status_icon);
	if (sicon->priv->dialog != NULL) {
		notify_notification_close (sicon->priv->dialog, NULL);
		g_object_unref (sicon->priv->dialog);
	}
	if (sicon->priv->notify_data != NULL) {
		g_free (sicon->priv->notify_data);
	}

	G_OBJECT_CLASS (gpk_smart_icon_parent_class)->finalize (object);
}

/**
 * gpk_smart_icon_new:
 *
 * Return value: a new GpkSmartIcon object.
 **/
GpkSmartIcon *
gpk_smart_icon_new (void)
{
	GpkSmartIcon *sicon;
	sicon = g_object_new (PK_TYPE_SMART_ICON, NULL);
	return PK_SMART_ICON (sicon);
}

