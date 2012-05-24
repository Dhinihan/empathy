/*
 * gedit-close-button.c
 * This file is part of gedit
 *
 * Copyright (C) 2010 - Paolo Borelli
 * Copyright (C) 2011 - Ignacio Casal Quinteiro
 *
 * gedit is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * gedit is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "gedit-close-button.h"

struct _GeditCloseButtonClassPrivate
{
	GtkCssProvider *css;
};

G_DEFINE_TYPE_WITH_CODE (GeditCloseButton, gedit_close_button, GTK_TYPE_BUTTON,
                         g_type_add_class_private (g_define_type_id, sizeof (GeditCloseButtonClassPrivate)))

static void
gedit_close_button_class_init (GeditCloseButtonClass *klass)
{
}

static void
gedit_close_button_init (GeditCloseButton *button)
{
	GtkWidget *image;

	image = gtk_image_new_from_stock (GTK_STOCK_CLOSE,
	                                  GTK_ICON_SIZE_MENU);
	gtk_widget_show (image);

	gtk_container_add (GTK_CONTAINER (button), image);

	gtk_widget_set_name (GTK_WIDGET (button), "empathy-tab-close-button");
}

GtkWidget *
gedit_close_button_new ()
{
	return GTK_WIDGET (g_object_new (GEDIT_TYPE_CLOSE_BUTTON,
	                                 "relief", GTK_RELIEF_NONE,
	                                 "focus-on-click", FALSE,
	                                 NULL));
}

/* ex:set ts=8 noet: */
