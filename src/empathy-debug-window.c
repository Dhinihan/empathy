/*
*  Copyright (C) 2009 Collabora Ltd.
*
*  This library is free software; you can redistribute it and/or
*  modify it under the terms of the GNU Lesser General Public
*  License as published by the Free Software Foundation; either
*  version 2.1 of the License, or (at your option) any later version.
*
*  This library is distributed in the hope that it will be useful,
*  but WITHOUT ANY WARRANTY; without even the implied warranty of
*  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
*  Lesser General Public License for more details.
*
*  You should have received a copy of the GNU Lesser General Public
*  License along with this library; if not, write to the Free Software
*  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*
*  Authors: Jonny Lamb <jonny.lamb@collabora.co.uk>
*           Cosimo Cecchi <cosimo.cecchi@collabora.co.uk>
*/

#include "config.h"

#include <glib/gi18n.h>
#include <gtk/gtk.h>
#include <gio/gio.h>
#include <gdk/gdkkeysyms.h>

#define DEBUG_FLAG EMPATHY_DEBUG_OTHER
#include <libempathy/empathy-debug.h>
#include <libempathy/empathy-utils.h>

#include <libempathy-gtk/empathy-account-chooser.h>
#include <libempathy-gtk/empathy-geometry.h>
#include <libempathy-gtk/empathy-ui-utils.h>

#include <telepathy-glib/dbus.h>
#include <telepathy-glib/interfaces.h>
#include <telepathy-glib/util.h>
#include <telepathy-glib/proxy-subclass.h>
#include <telepathy-glib/account-manager.h>

#include "extensions/extensions.h"

#include "empathy-debug-window.h"

G_DEFINE_TYPE (EmpathyDebugWindow, empathy_debug_window,
    GTK_TYPE_WINDOW)

typedef enum
{
  SERVICE_TYPE_CM = 0,
  SERVICE_TYPE_CLIENT,
} ServiceType;

enum
{
  COL_DEBUG_TIMESTAMP = 0,
  COL_DEBUG_DOMAIN,
  COL_DEBUG_CATEGORY,
  COL_DEBUG_LEVEL_STRING,
  COL_DEBUG_MESSAGE,
  COL_DEBUG_LEVEL_VALUE,
  NUM_DEBUG_COLS
};

enum
{
  COL_NAME = 0,
  COL_UNIQUE_NAME,
  COL_GONE,
  COL_ACTIVE_BUFFER,
  COL_PAUSE_BUFFER,
  COL_PROXY,
  NUM_COLS
};

enum
{
  COL_LEVEL_NAME,
  COL_LEVEL_VALUE,
  NUM_COLS_LEVEL
};

#define GET_PRIV(obj) EMPATHY_GET_PRIV (obj, EmpathyDebugWindow)
typedef struct
{
  /* Toolbar items */
  GtkWidget *chooser;
  GtkToolItem *save_button;
  GtkToolItem *copy_button;
  GtkToolItem *clear_button;
  GtkToolItem *pause_button;
  GtkToolItem *level_label;
  GtkWidget *level_filter;

  /* TreeView */
  GtkTreeModel *store_filter;
  GtkWidget *view;
  GtkWidget *scrolled_win;
  GtkWidget *not_supported_label;
  gboolean view_visible;

  /* Connection */
  TpDBusDaemon *dbus;
  TpProxySignalConnection *name_owner_changed_signal;

  /* Whether NewDebugMessage will be fired */
  gboolean paused;

  /* Service (CM, Client) chooser store */
  GtkListStore *service_store;

  /* Counters on services detected and added */
  guint services_detected;
  guint name_owner_cb_count;

  /* Debug to show upon creation */
  gchar *select_name;

  /* Misc. */
  gboolean dispose_run;
  TpAccountManager *am;
  GtkListStore *all_active_buffer;
} EmpathyDebugWindowPriv;

static const gchar *
log_level_to_string (guint level)
{
  switch (level)
    {
    case TP_DEBUG_LEVEL_ERROR:
      return "Error";
      break;
    case TP_DEBUG_LEVEL_CRITICAL:
      return "Critical";
      break;
    case TP_DEBUG_LEVEL_WARNING:
      return "Warning";
      break;
    case TP_DEBUG_LEVEL_MESSAGE:
      return "Message";
      break;
    case TP_DEBUG_LEVEL_INFO:
      return "Info";
      break;
    case TP_DEBUG_LEVEL_DEBUG:
      return "Debug";
      break;
    default:
      g_assert_not_reached ();
      break;
    }
}

static gchar *
get_active_service_name (EmpathyDebugWindow *self)
{
  EmpathyDebugWindowPriv *priv = GET_PRIV (self);
  GtkTreeIter iter;
  gchar *name;

  if (!gtk_combo_box_get_active_iter (GTK_COMBO_BOX (priv->chooser), &iter))
    return NULL;

  gtk_tree_model_get (GTK_TREE_MODEL (priv->service_store), &iter,
      COL_NAME, &name, -1);

  return name;
}

static gboolean
copy_buffered_messages (GtkTreeModel *buffer,
    GtkTreePath *path,
    GtkTreeIter *iter,
    gpointer data)
{
  GtkListStore *active_buffer = data;
  GtkTreeIter active_buffer_iter;
  gdouble timestamp;
  gchar *domain, *category, *message, *level_string;
  guint level;

  gtk_tree_model_get (buffer, iter,
      COL_DEBUG_TIMESTAMP, &timestamp,
      COL_DEBUG_DOMAIN, &domain,
      COL_DEBUG_CATEGORY, &category,
      COL_DEBUG_LEVEL_STRING, &level_string,
      COL_DEBUG_MESSAGE, &message,
      COL_DEBUG_LEVEL_VALUE, &level,
      -1);
  gtk_list_store_insert_with_values (active_buffer, &active_buffer_iter, -1,
      COL_DEBUG_TIMESTAMP, timestamp,
      COL_DEBUG_DOMAIN, domain,
      COL_DEBUG_CATEGORY, category,
      COL_DEBUG_LEVEL_STRING, level_string,
      COL_DEBUG_MESSAGE, message,
      COL_DEBUG_LEVEL_VALUE, level,
      -1);

  g_free (domain);
  g_free (category);
  g_free (level_string);
  g_free (message);

  return FALSE;
}

static void
insert_values_in_buffer (GtkListStore *store,
        gdouble timestamp,
        const gchar *domain,
        const gchar *category,
        guint level,
        const gchar *string)
{
  GtkTreeIter iter;

  gtk_list_store_insert_with_values (store, &iter, -1,
      COL_DEBUG_TIMESTAMP, timestamp,
      COL_DEBUG_DOMAIN, domain,
      COL_DEBUG_CATEGORY, category,
      COL_DEBUG_LEVEL_STRING, log_level_to_string (level),
      COL_DEBUG_MESSAGE, string,
      COL_DEBUG_LEVEL_VALUE, level,
      -1); 
}

static void
debug_window_add_message (EmpathyDebugWindow *debug_window,
    TpProxy *proxy,
    gdouble timestamp,
    const gchar *domain_category,
    guint level,
    const gchar *message)
{
  EmpathyDebugWindowPriv *priv = GET_PRIV (debug_window);
  gchar *domain, *category;
  gchar *string;
  GtkListStore *active_buffer, *pause_buffer;

  if (g_strrstr (domain_category, "/"))
    {
      gchar **parts = g_strsplit (domain_category, "/", 2);
      domain = g_strdup (parts[0]);
      category = g_strdup (parts[1]);
      g_strfreev (parts);
    }
  else
    {
      domain = g_strdup (domain_category);
      category = g_strdup ("");
    }

  if (g_str_has_suffix (message, "\n"))
    string = g_strchomp (g_strdup (message));
  else
    string = g_strdup (message);

  pause_buffer = g_object_get_data (G_OBJECT (proxy), "pause-buffer");
  active_buffer = g_object_get_data (G_OBJECT (proxy), "active-buffer");

  if (priv->paused)
    {
      insert_values_in_buffer (pause_buffer, timestamp,
          domain, category, level,
          string);
    }
  else
    {
      /* Append 'this' message to this service's and All's active-buffers */
      insert_values_in_buffer (active_buffer, timestamp,
          domain, category, level,
          string);

      insert_values_in_buffer (priv->all_active_buffer, timestamp,
          domain, category, level,
          string);
    }

  g_free (string);
  g_free (domain);
  g_free (category);
}

static void
debug_window_new_debug_message_cb (TpProxy *proxy,
    gdouble timestamp,
    const gchar *domain,
    guint level,
    const gchar *message,
    gpointer user_data,
    GObject *weak_object)
{
  EmpathyDebugWindow *debug_window = (EmpathyDebugWindow *) user_data;

  debug_window_add_message (debug_window, proxy, timestamp, domain, level,
      message);
}

static void
debug_window_set_enabled (TpProxy *proxy,
    gboolean enabled)
{
  GValue *val;

  g_return_if_fail (proxy != NULL);

  val = tp_g_value_slice_new_boolean (enabled);

  tp_cli_dbus_properties_call_set (proxy, -1, TP_IFACE_DEBUG,
      "Enabled", val, NULL, NULL, NULL, NULL);

  tp_g_value_slice_free (val);
}

static void
debug_window_set_toolbar_sensitivity (EmpathyDebugWindow *debug_window,
    gboolean sensitive)
{
  EmpathyDebugWindowPriv *priv = GET_PRIV (debug_window);
  GtkWidget *vbox = gtk_bin_get_child (GTK_BIN (debug_window));

  gtk_widget_set_sensitive (GTK_WIDGET (priv->save_button), sensitive);
  gtk_widget_set_sensitive (GTK_WIDGET (priv->copy_button), sensitive);
  gtk_widget_set_sensitive (GTK_WIDGET (priv->clear_button), sensitive);
  gtk_widget_set_sensitive (GTK_WIDGET (priv->pause_button), sensitive);
  gtk_widget_set_sensitive (GTK_WIDGET (priv->level_label), sensitive);
  gtk_widget_set_sensitive (GTK_WIDGET (priv->level_filter), sensitive);
  gtk_widget_set_sensitive (GTK_WIDGET (priv->view), sensitive);

  if (sensitive && !priv->view_visible)
    {
      /* Add view and remove label */
      gtk_container_remove (GTK_CONTAINER (vbox), priv->not_supported_label);
      gtk_box_pack_start (GTK_BOX (vbox), priv->scrolled_win, TRUE, TRUE, 0);
      priv->view_visible = TRUE;
    }
  else if (!sensitive && priv->view_visible)
    {
      /* Add label and remove view */
      gtk_container_remove (GTK_CONTAINER (vbox), priv->scrolled_win);
      gtk_box_pack_start (GTK_BOX (vbox), priv->not_supported_label,
          TRUE, TRUE, 0);
      priv->view_visible = FALSE;
    }
}

static gboolean
debug_window_get_iter_for_active_buffer (GtkListStore *active_buffer,
    GtkTreeIter *iter,
    EmpathyDebugWindow *debug_window)
{
  EmpathyDebugWindowPriv *priv = GET_PRIV (debug_window);
  gboolean valid_iter;
  GtkTreeModel *model = GTK_TREE_MODEL (priv->service_store);

  gtk_tree_model_get_iter_first (model, iter);
  for (valid_iter = gtk_tree_model_iter_next (model, iter);
       valid_iter;
       valid_iter = gtk_tree_model_iter_next (model, iter))
    {
      GtkListStore *stored_active_buffer;

      gtk_tree_model_get (model, iter,
          COL_ACTIVE_BUFFER, &stored_active_buffer,
          -1);
      if (active_buffer == stored_active_buffer)
        {
          g_object_unref (stored_active_buffer);
          return valid_iter;
        }
      g_object_unref (stored_active_buffer);
    }

  return valid_iter;
}

static void refresh_all_buffer (EmpathyDebugWindow *debug_window);

static void
proxy_invalidated_cb (TpProxy *proxy,
    guint domain,
    gint code,
    gchar *msg,
    gpointer user_data)
{
  EmpathyDebugWindow *self = (EmpathyDebugWindow *) user_data;
  EmpathyDebugWindowPriv *priv = GET_PRIV (self);
  GtkTreeModel *service_store = GTK_TREE_MODEL (priv->service_store);
  TpProxy *stored_proxy;
  GtkTreeIter iter;
  gboolean valid_iter;

  /* Proxy has been invalidated so we find and set it to NULL
   * in service store */
  gtk_tree_model_get_iter_first (service_store, &iter);
  for (valid_iter = gtk_tree_model_iter_next (service_store, &iter);
       valid_iter;
       valid_iter = gtk_tree_model_iter_next (service_store, &iter))
    {
      gtk_tree_model_get (service_store, &iter,
          COL_PROXY, &stored_proxy,
          -1);

      if (proxy == stored_proxy)
        gtk_list_store_set (priv->service_store, &iter,
            COL_PROXY, NULL,
            -1);
    }

  /* Also, we refresh "All" selection's active buffer since it should not
   * show messages obtained from the proxy getting destroyed above */
  refresh_all_buffer (self);
}

static void
debug_window_get_messages_cb (TpProxy *proxy,
    const GPtrArray *messages,
    const GError *error,
    gpointer user_data,
    GObject *weak_object)
{
  EmpathyDebugWindow *debug_window = (EmpathyDebugWindow *) user_data;
  EmpathyDebugWindowPriv *priv = GET_PRIV (debug_window);
  gchar *active_service_name;
  guint i;
  GtkListStore *active_buffer;
  gboolean valid_iter;
  GtkTreeIter iter;
  gchar *proxy_service_name;

  active_buffer = g_object_get_data (G_OBJECT (proxy), "active-buffer");
  valid_iter = debug_window_get_iter_for_active_buffer (active_buffer, &iter,
      debug_window);
  gtk_tree_model_get (GTK_TREE_MODEL (priv->service_store), &iter,
      COL_NAME, &proxy_service_name,
      -1);

  active_service_name = get_active_service_name (debug_window);
  if (error != NULL)
    {
      DEBUG ("GetMessages failed: %s", error->message);

      /* We want to set the window sensitivity to false only when proxy for the
       * selected service is unable to fetch debug messages */
      if (!tp_strdiff (active_service_name, proxy_service_name))
        debug_window_set_toolbar_sensitivity (debug_window, FALSE);

      /* We created the proxy for GetMessages call. Now destroy it. */
      tp_clear_object (&proxy);
      return;
    }

  DEBUG ("Retrieved debug messages for %s", active_service_name);
  g_free (active_service_name);
  debug_window_set_toolbar_sensitivity (debug_window, TRUE);


  for (i = 0; i < messages->len; i++)
    {
      GValueArray *values = g_ptr_array_index (messages, i);

      debug_window_add_message (debug_window, proxy,
          g_value_get_double (g_value_array_get_nth (values, 0)),
          g_value_get_string (g_value_array_get_nth (values, 1)),
          g_value_get_uint (g_value_array_get_nth (values, 2)),
          g_value_get_string (g_value_array_get_nth (values, 3)));
    }

  /* Now we save this precious proxy in the service_store along its service */
  if (valid_iter)
    {
      DEBUG ("Proxy for service: %s was successful in fetching debug"
          " messages. Saving it.", proxy_service_name);

      gtk_list_store_set (priv->service_store, &iter,
          COL_PROXY, proxy,
          -1);
    }

  g_free (proxy_service_name);

  /* Connect to "invalidated" signal */
  g_signal_connect (proxy, "invalidated",
      G_CALLBACK (proxy_invalidated_cb), debug_window);

 /* Connect to NewDebugMessage */
  emp_cli_debug_connect_to_new_debug_message (
      proxy, debug_window_new_debug_message_cb, debug_window,
      NULL, NULL, NULL);

  /* Now that active-buffer is up to date, we can see which messages are
   * to be visible */
  gtk_tree_model_filter_refilter (GTK_TREE_MODEL_FILTER (priv->store_filter));

  /* Set the proxy to signal for new debug messages */
  debug_window_set_enabled (proxy, TRUE);
}

static void
create_proxy_to_get_messages (EmpathyDebugWindow *debug_window,
    GtkTreeIter *iter,
    TpDBusDaemon *dbus)
{
  EmpathyDebugWindowPriv *priv = GET_PRIV (debug_window);
  gchar *bus_name, *name = NULL;
  TpProxy *new_proxy, *stored_proxy = NULL;
  GtkTreeModel *pause_buffer, *active_buffer;
  gboolean gone;

  gtk_tree_model_get (GTK_TREE_MODEL (priv->service_store), iter,
      COL_NAME, &name,
      COL_GONE, &gone,
      COL_ACTIVE_BUFFER, &active_buffer,
      COL_PAUSE_BUFFER, &pause_buffer,
      COL_PROXY, &stored_proxy,
      -1);

  /* If the stored_proxy is not NULL then messages have been obtained and
   * new-debug-message-signal has been set on it. Also, the proxy is valid.
   * If the service is gone, we still display the messages-cached till now. */
  if (gone ||
      (!gone && stored_proxy != NULL))
    {
      /* Nothing needs to be done. The associated active-buffer has already
       * been set as view's model */
      goto finally;
    }

  DEBUG ("Preparing proxy to obtain messages for service %s", name);

  gtk_tree_model_get (GTK_TREE_MODEL (priv->service_store), iter,
      COL_UNIQUE_NAME, &bus_name, -1);
  new_proxy = g_object_new (TP_TYPE_PROXY,
      "bus-name", bus_name,
      "dbus-daemon", dbus,
      "object-path", DEBUG_OBJECT_PATH,
      NULL);
  g_free (bus_name);

  g_object_set_data (G_OBJECT (new_proxy), "active-buffer", active_buffer);
  g_object_set_data (G_OBJECT (new_proxy), "pause-buffer", pause_buffer);

  /* Now we call GetMessages with fresh proxy.
   * The old proxy is NULL due to one of the following -
   * * Wasn't saved as last GetMessages call failed
   * * The service has newly arrived and no proxy has been prepared yet for it
   * * A service with the same name has reappeared but the owner maybe new */
  tp_proxy_add_interface_by_id (new_proxy, emp_iface_quark_debug ());

  emp_cli_debug_call_get_messages (new_proxy, -1,
      debug_window_get_messages_cb, debug_window, NULL, NULL);

finally:
  g_free (name);
  tp_clear_object (&stored_proxy);
  g_object_unref (active_buffer);
  g_object_unref (pause_buffer);
}

static GtkListStore*
new_list_store_for_service (void)
{
  return gtk_list_store_new (NUM_DEBUG_COLS,
             G_TYPE_DOUBLE, /* COL_DEBUG_TIMESTAMP */
             G_TYPE_STRING, /* COL_DEBUG_DOMAIN */
             G_TYPE_STRING, /* COL_DEBUG_CATEGORY */
             G_TYPE_STRING, /* COL_DEBUG_LEVEL_STRING */
             G_TYPE_STRING, /* COL_DEBUG_MESSAGE */
             G_TYPE_UINT);  /* COL_DEBUG_LEVEL_VALUE */
}

static gboolean
debug_window_visible_func (GtkTreeModel *model,
    GtkTreeIter *iter,
    gpointer user_data)
{
  EmpathyDebugWindow *debug_window = (EmpathyDebugWindow *) user_data;
  EmpathyDebugWindowPriv *priv = GET_PRIV (debug_window);
  guint filter_value, level;
  GtkTreeModel *filter_model;
  GtkTreeIter filter_iter;

  filter_model = gtk_combo_box_get_model (GTK_COMBO_BOX (priv->level_filter));
  gtk_combo_box_get_active_iter (GTK_COMBO_BOX (priv->level_filter),
      &filter_iter);

  gtk_tree_model_get (model, iter, COL_DEBUG_LEVEL_VALUE, &level, -1);
  gtk_tree_model_get (filter_model, &filter_iter,
      COL_LEVEL_VALUE, &filter_value, -1);

  if (level <= filter_value)
    return TRUE;

  return FALSE;
}

static gboolean
tree_view_search_equal_func_cb (GtkTreeModel *model,
    gint column,
    const gchar *key,

    GtkTreeIter *iter,
    gpointer search_data)
{
  gchar *str;
  gint key_len;
  gint len;
  gint i;
  gboolean ret = TRUE; /* The return value is counter-intuitive */

  gtk_tree_model_get (model, iter, column, &str, -1);

  key_len = strlen (key);
  len = strlen (str) - key_len;

  for (i = 0; i <= len; ++i)
    {
      if (!g_ascii_strncasecmp (key, str + i, key_len))
        {
          ret = FALSE;
          break;
        }
    }

  g_free (str);
  return ret;
}

static void
update_store_filter (EmpathyDebugWindow *debug_window,
    GtkListStore *active_buffer)
{
  EmpathyDebugWindowPriv *priv = GET_PRIV (debug_window);
  debug_window_set_toolbar_sensitivity (debug_window, FALSE);

  tp_clear_object (&priv->store_filter);
  priv->store_filter = gtk_tree_model_filter_new (
      GTK_TREE_MODEL (active_buffer), NULL);

  gtk_tree_model_filter_set_visible_func (
      GTK_TREE_MODEL_FILTER (priv->store_filter),
      debug_window_visible_func, debug_window, NULL);
  gtk_tree_view_set_model (GTK_TREE_VIEW (priv->view),
      priv->store_filter);

  /* Since view's model has changed, reset the search column and
   * search_equal_func */
  gtk_tree_view_set_search_column (GTK_TREE_VIEW (priv->view),
      COL_DEBUG_MESSAGE);
  gtk_tree_view_set_search_equal_func (GTK_TREE_VIEW (priv->view),
      tree_view_search_equal_func_cb, NULL, NULL);

  debug_window_set_toolbar_sensitivity (debug_window, TRUE);
}

static void
refresh_all_buffer (EmpathyDebugWindow *debug_window)
{
  EmpathyDebugWindowPriv *priv = GET_PRIV (debug_window);
  gboolean valid_iter;
  GtkTreeIter iter;
  GtkTreeModel *service_store = GTK_TREE_MODEL (priv->service_store);

  /* Clear All's active-buffer */
  gtk_list_store_clear (priv->all_active_buffer);

  /* Skipping the first service store iter which is reserved for "All" */
  gtk_tree_model_get_iter_first (service_store, &iter);
  for (valid_iter = gtk_tree_model_iter_next (service_store, &iter);
       valid_iter;
       valid_iter = gtk_tree_model_iter_next (service_store, &iter))
    {
      TpProxy *proxy = NULL;
      GtkListStore *service_active_buffer;
      gboolean gone;

      gtk_tree_model_get (service_store, &iter,
          COL_GONE, &gone,
          COL_PROXY, &proxy,
          COL_ACTIVE_BUFFER, &service_active_buffer,
          -1);

      if (gone)
        {
          gtk_tree_model_foreach (GTK_TREE_MODEL (service_active_buffer),
              copy_buffered_messages, priv->all_active_buffer);
        }
      else
        {
          if (proxy != NULL)
            {
              if (service_active_buffer == NULL)
                break;

              /* Copy the debug messages to all_active_buffer */
              gtk_tree_model_foreach (GTK_TREE_MODEL (service_active_buffer),
                  copy_buffered_messages, priv->all_active_buffer);
            }
          else
            {
              GError *error = NULL;
              TpDBusDaemon *dbus = tp_dbus_daemon_dup (&error);

              if (error != NULL)
                {
                  DEBUG ("Failed at duping the dbus daemon: %s", error->message);
                  g_error_free (error);
                }

              create_proxy_to_get_messages (debug_window, &iter, dbus);

              g_object_unref (dbus);
            }
        }

      g_object_unref (service_active_buffer);
      tp_clear_object (&proxy);
    }
}

static void
debug_window_service_chooser_changed_cb (GtkComboBox *chooser,
    EmpathyDebugWindow *debug_window)
{
  EmpathyDebugWindowPriv *priv = GET_PRIV (debug_window);
  TpDBusDaemon *dbus;
  GError *error = NULL;
  GtkListStore *stored_active_buffer = NULL;
  gchar *name = NULL;
  GtkTreeIter iter;
  gboolean gone;

  if (!gtk_combo_box_get_active_iter (chooser, &iter))
    {
      DEBUG ("No CM is selected");
      if (gtk_tree_model_iter_n_children (
          GTK_TREE_MODEL (priv->service_store), NULL) > 0)
        {
          gtk_combo_box_set_active (chooser, 0);
        }
      return;
    }

  debug_window_set_toolbar_sensitivity (debug_window, TRUE);

  gtk_tree_model_get (GTK_TREE_MODEL (priv->service_store), &iter,
      COL_NAME, &name,
      COL_GONE, &gone,
      COL_ACTIVE_BUFFER, &stored_active_buffer,
      -1);

  DEBUG ("Service chosen: %s", name);

  if (tp_strdiff (name, "All") && stored_active_buffer == NULL)
    {
      DEBUG ("No list store assigned to service %s", name);
      goto finally;
    }

  if (!tp_strdiff (name, "All"))
    {
      update_store_filter (debug_window, priv->all_active_buffer);
      goto finally;
    }

  update_store_filter (debug_window, stored_active_buffer);

  dbus = tp_dbus_daemon_dup (&error);

  if (error != NULL)
    {
      DEBUG ("Failed at duping the dbus daemon: %s", error->message);
    }

  create_proxy_to_get_messages (debug_window, &iter, dbus);

  g_object_unref (dbus);

finally:
  g_free (name);
  tp_clear_object (&stored_active_buffer);
}

typedef struct
{
  const gchar *name;
  gboolean found;
  gboolean use_name;
  GtkTreeIter **found_iter;
} CmInModelForeachData;

static gboolean
debug_window_service_foreach (GtkTreeModel *model,
    GtkTreePath *path,
    GtkTreeIter *iter,
    gpointer user_data)
{
  CmInModelForeachData *data = (CmInModelForeachData *) user_data;
  gchar *store_name;

  gtk_tree_model_get (model, iter,
      (data->use_name ? COL_NAME : COL_UNIQUE_NAME),
      &store_name,
      -1);

  if (!tp_strdiff (store_name, data->name))
    {
      data->found = TRUE;

      if (data->found_iter != NULL)
        *(data->found_iter) = gtk_tree_iter_copy (iter);
    }

  g_free (store_name);

  return data->found;
}

static gboolean
debug_window_service_is_in_model (EmpathyDebugWindow *debug_window,
    const gchar *name,
    GtkTreeIter **iter,
    gboolean use_name)
{
  EmpathyDebugWindowPriv *priv = GET_PRIV (debug_window);
  CmInModelForeachData *data;
  gboolean found;

  data = g_slice_new0 (CmInModelForeachData);
  data->name = name;
  data->found = FALSE;
  data->found_iter = iter;
  data->use_name = use_name;

  gtk_tree_model_foreach (GTK_TREE_MODEL (priv->service_store),
      debug_window_service_foreach, data);

  found = data->found;

  g_slice_free (CmInModelForeachData, data);

  return found;
}

static gchar *
get_cm_display_name (EmpathyDebugWindow *self,
    const char *cm_name)
{
  EmpathyDebugWindowPriv *priv = GET_PRIV (self);
  GHashTable *protocols = g_hash_table_new (g_str_hash, g_str_equal);
  GList *accounts, *ptr;
  char *retval;

  accounts = tp_account_manager_get_valid_accounts (priv->am);

  for (ptr = accounts; ptr != NULL; ptr = ptr->next)
    {
      TpAccount *account = TP_ACCOUNT (ptr->data);

      if (!tp_strdiff (tp_account_get_connection_manager (account), cm_name))
        {
          g_hash_table_insert (protocols,
              (char *) tp_account_get_protocol (account),
              GUINT_TO_POINTER (TRUE));
        }
    }

  g_list_free (accounts);

  if (g_hash_table_size (protocols) > 0)
    {
      GHashTableIter iter;
      char **protocolsv;
      char *key, *str;
      guint i;

      protocolsv = g_new0 (char *, g_hash_table_size (protocols) + 1);

      g_hash_table_iter_init (&iter, protocols);
      for (i = 0; g_hash_table_iter_next (&iter, (gpointer) &key, NULL); i++)
        {
          protocolsv[i] = key;
        }

      str = g_strjoinv (", ", protocolsv);
      retval = g_strdup_printf ("%s (%s)", cm_name, str);

      g_free (protocolsv);
      g_free (str);
    }
  else
    {
      retval = g_strdup (cm_name);
    }

  g_hash_table_unref (protocols);

  return retval;
}

typedef struct
{
  EmpathyDebugWindow *debug_window;
  gchar *name;
  ServiceType type;
} FillServiceChooserData;

static FillServiceChooserData *
fill_service_chooser_data_new (EmpathyDebugWindow *window,
    const gchar *name,
    ServiceType type)
{
  FillServiceChooserData * data = g_slice_new (FillServiceChooserData);

  data->debug_window = window;
  data->name = g_strdup (name);
  data->type = SERVICE_TYPE_CM;
  return data;
}

static void
fill_service_chooser_data_free (FillServiceChooserData *data)
{
  g_free (data->name);
  g_slice_free (FillServiceChooserData, data);
}

static void
debug_window_get_name_owner_cb (TpDBusDaemon *proxy,
    const gchar *out,
    const GError *error,
    gpointer user_data,
    GObject *weak_object)
{
  FillServiceChooserData *data = (FillServiceChooserData *) user_data;
  EmpathyDebugWindow *self = EMPATHY_DEBUG_WINDOW (data->debug_window);
  EmpathyDebugWindowPriv *priv = GET_PRIV (data->debug_window);
  GtkTreeIter iter;

  priv->name_owner_cb_count++;

  if (error != NULL)
    {
      DEBUG ("GetNameOwner failed: %s", error->message);
      goto OUT;
    }

  if (!debug_window_service_is_in_model (data->debug_window, out, NULL, FALSE))
    {
      char *name;
      GtkListStore *active_buffer, *pause_buffer;

      DEBUG ("Adding %s to list: %s at unique name: %s",
          data->type == SERVICE_TYPE_CM? "CM": "Client",
          data->name, out);

      if (data->type == SERVICE_TYPE_CM)
        name = get_cm_display_name (self, data->name);
      else
        name = g_strdup (data->name);

      active_buffer = new_list_store_for_service ();
      pause_buffer = new_list_store_for_service ();

      gtk_list_store_insert_with_values (priv->service_store, &iter, -1,
          COL_NAME, name,
          COL_UNIQUE_NAME, out,
          COL_GONE, FALSE,
          COL_ACTIVE_BUFFER, active_buffer,
          COL_PAUSE_BUFFER, pause_buffer,
          COL_PROXY, NULL,
          -1);

      g_object_unref (active_buffer);
      g_object_unref (pause_buffer);

      if (priv->select_name != NULL &&
          !tp_strdiff (name, priv->select_name))
        {
          gtk_combo_box_set_active_iter (GTK_COMBO_BOX (priv->chooser), &iter);
          tp_clear_pointer (&priv->select_name, g_free);
        }

      g_free (name);
    }

    if (priv->services_detected == priv->name_owner_cb_count)
      {
        /* Time to add "All" selection to service_store */
        gtk_list_store_insert_with_values (priv->service_store, &iter, 0,
            COL_NAME, "All",
            COL_ACTIVE_BUFFER, NULL,
            -1);

        priv->all_active_buffer = new_list_store_for_service ();

        /* Populate active buffers for all services */
        refresh_all_buffer (self);

        gtk_combo_box_set_active (GTK_COMBO_BOX (priv->chooser), 0);
      }

OUT:
  fill_service_chooser_data_free (data);
}

static void
debug_window_list_connection_names_cb (const gchar * const *names,
    gsize n,
    const gchar * const *cms,
    const gchar * const *protocols,
    const GError *error,
    gpointer user_data,
    GObject *weak_object)
{
  EmpathyDebugWindow *debug_window = (EmpathyDebugWindow *) user_data;
  EmpathyDebugWindowPriv *priv = GET_PRIV (debug_window);
  guint i;
  TpDBusDaemon *dbus;
  GError *error2 = NULL;

  if (error != NULL)
    {
      DEBUG ("list_connection_names failed: %s", error->message);
      return;
    }

  dbus = tp_dbus_daemon_dup (&error2);

  if (error2 != NULL)
    {
      DEBUG ("Failed to dup TpDBusDaemon.");
      g_error_free (error2);
      return;
    }

  for (i = 0; cms[i] != NULL; i++)
    {
      FillServiceChooserData *data = fill_service_chooser_data_new (
          debug_window, cms[i], SERVICE_TYPE_CM);

      tp_cli_dbus_daemon_call_get_name_owner (dbus, -1,
          names[i], debug_window_get_name_owner_cb,
          data, NULL, NULL);

      priv->services_detected ++;
    }

  g_object_unref (dbus);
}

static void
debug_window_name_owner_changed_cb (TpDBusDaemon *proxy,
    const gchar *arg0,
    const gchar *arg1,
    const gchar *arg2,
    gpointer user_data,
    GObject *weak_object)
{
  EmpathyDebugWindow *self = EMPATHY_DEBUG_WINDOW (user_data);
  EmpathyDebugWindowPriv *priv = GET_PRIV (user_data);
  ServiceType type;
  const gchar *name;

  if (g_str_has_prefix (arg0, TP_CM_BUS_NAME_BASE))
    {
      type = SERVICE_TYPE_CM;
      name = arg0 + strlen (TP_CM_BUS_NAME_BASE);
    }
  else if (g_str_has_prefix (arg0, TP_CLIENT_BUS_NAME_BASE))
    {
      type = SERVICE_TYPE_CLIENT;
      name = arg0 + strlen (TP_CLIENT_BUS_NAME_BASE);
    }
  else
    {
      return;
    }

  if (EMP_STR_EMPTY (arg1) && !EMP_STR_EMPTY (arg2))
    {
      GtkTreeIter *found_at_iter = NULL;
      gchar *display_name;

      if (type == SERVICE_TYPE_CM)
        display_name = get_cm_display_name (self, name);
      else
        display_name = g_strdup (name);

      /* A service joined */
      if (!debug_window_service_is_in_model (user_data, display_name,
           &found_at_iter, TRUE))
        {
          GtkTreeIter iter;
          GtkListStore *active_buffer, *pause_buffer;

          DEBUG ("Adding new service '%s' at %s.", name, arg2);

          active_buffer = new_list_store_for_service ();
          pause_buffer = new_list_store_for_service ();

          gtk_list_store_insert_with_values (priv->service_store, &iter, -1,
              COL_NAME, display_name,
              COL_UNIQUE_NAME, arg2,
              COL_GONE, FALSE,
              COL_ACTIVE_BUFFER, active_buffer,
              COL_PAUSE_BUFFER, pause_buffer,
              COL_PROXY, NULL,
              -1);

          g_object_unref (active_buffer);
          g_object_unref (pause_buffer);
        }
      else
        {
          /* a service with the same name is already in the service_store,
           * update it and set it as re-enabled.
           */
          GtkListStore *active_buffer, *pause_buffer;
          TpProxy *stored_proxy;

          DEBUG ("Refreshing CM '%s' at '%s'.", name, arg2);

          active_buffer= new_list_store_for_service ();
          pause_buffer = new_list_store_for_service ();

          gtk_tree_model_get (GTK_TREE_MODEL (priv->service_store),
              found_at_iter, COL_PROXY, &stored_proxy, -1);

          tp_clear_object (&stored_proxy);

          gtk_list_store_set (priv->service_store, found_at_iter,
              COL_NAME, display_name,
              COL_UNIQUE_NAME, arg2,
              COL_GONE, FALSE,
              COL_ACTIVE_BUFFER, active_buffer,
              COL_PAUSE_BUFFER, pause_buffer,
              COL_PROXY, NULL,
              -1);

          g_object_unref (active_buffer);
          g_object_unref (pause_buffer);

          gtk_tree_iter_free (found_at_iter);

          debug_window_service_chooser_changed_cb
            (GTK_COMBO_BOX (priv->chooser), user_data);
        }

      /* If a new service arrives when "All" is selected, the view will
       * not show its messages which we do not want. So we refresh All's
       * active buffer.
       * Similarly for when a service with an already seen service name
       * appears. */
      refresh_all_buffer (self);

      g_free (display_name);
    }
  else if (!EMP_STR_EMPTY (arg1) && EMP_STR_EMPTY (arg2))
    {
      /* A service died */
      GtkTreeIter *iter = NULL;

      DEBUG ("Setting service disabled from %s.", arg1);

      /* set the service as disabled in the model */
      if (debug_window_service_is_in_model (user_data, arg1, &iter, FALSE))
        {
          gtk_list_store_set (priv->service_store,
              iter, COL_GONE, TRUE, -1);
          gtk_tree_iter_free (iter);
        }

      /* Refresh all's active buffer */
      refresh_all_buffer (self);
    }
}

static void
add_client (EmpathyDebugWindow *self,
    const gchar *name)
{
  EmpathyDebugWindowPriv *priv = GET_PRIV (self);
  const gchar *suffix;
  FillServiceChooserData *data;

  suffix = name + strlen (TP_CLIENT_BUS_NAME_BASE);

  data = fill_service_chooser_data_new (self, suffix, SERVICE_TYPE_CLIENT);

  tp_cli_dbus_daemon_call_get_name_owner (priv->dbus, -1,
      name, debug_window_get_name_owner_cb, data, NULL, NULL);

  priv->services_detected ++;
}

static void
list_names_cb (TpDBusDaemon *bus_daemon,
    const gchar * const *names,
    const GError *error,
    gpointer user_data,
    GObject *weak_object)
{
  EmpathyDebugWindow *self = EMPATHY_DEBUG_WINDOW (weak_object);
  guint i;

  if (error != NULL)
    {
      DEBUG ("Failed to list names: %s", error->message);
      return;
    }

  for (i = 0; names[i] != NULL; i++)
    {
      if (g_str_has_prefix (names[i], TP_CLIENT_BUS_NAME_BASE))
        {
          add_client (self, names[i]);
        }
    }
}

static void
debug_window_fill_service_chooser (EmpathyDebugWindow *debug_window)
{
  EmpathyDebugWindowPriv *priv = GET_PRIV (debug_window);
  GError *error = NULL;
  GtkTreeIter iter;
  GtkListStore *active_buffer, *pause_buffer;

  priv->dbus = tp_dbus_daemon_dup (&error);

  if (error != NULL)
    {
      DEBUG ("Failed to dup dbus daemon: %s", error->message);
      g_error_free (error);
      return;
    }

  /* Keep a count of the services detected and added */
  priv->services_detected = 0;
  priv->name_owner_cb_count = 0;

  /* Add CMs to list */
  tp_list_connection_names (priv->dbus, debug_window_list_connection_names_cb,
      debug_window, NULL, NULL);

  /* add Mission Control */
  active_buffer= new_list_store_for_service ();
  pause_buffer = new_list_store_for_service ();

  gtk_list_store_insert_with_values (priv->service_store, &iter, -1,
      COL_NAME, "mission-control",
      COL_UNIQUE_NAME, "org.freedesktop.Telepathy.MissionControl5",
      COL_GONE, FALSE,
      COL_ACTIVE_BUFFER, active_buffer,
      COL_PAUSE_BUFFER, pause_buffer,
      COL_PROXY, NULL,
      -1);
  g_object_unref (active_buffer);
  g_object_unref (pause_buffer);

  /* add clients */
  tp_dbus_daemon_list_names (priv->dbus, 2000,
      list_names_cb, NULL, NULL, G_OBJECT (debug_window));

  priv->name_owner_changed_signal =
      tp_cli_dbus_daemon_connect_to_name_owner_changed (priv->dbus,
      debug_window_name_owner_changed_cb, debug_window, NULL, NULL, NULL);
}

static void
debug_window_pause_toggled_cb (GtkToggleToolButton *pause_,
    EmpathyDebugWindow *debug_window)
{
  EmpathyDebugWindowPriv *priv = GET_PRIV (debug_window);
  GtkTreeIter iter;
  gboolean valid_iter;
  GtkTreeModel *model = GTK_TREE_MODEL (priv->service_store);

  priv->paused = gtk_toggle_tool_button_get_active (pause_);

  if (!priv->paused)
    {
      /* Pause has been released - flush all pause buffers */
      GtkTreeModel *service_store = GTK_TREE_MODEL (priv->service_store);

      /* Skipping the first iter which is reserved for "All" */
      gtk_tree_model_get_iter_first (model, &iter);
      for (valid_iter = gtk_tree_model_iter_next (model, &iter);
           valid_iter;
           valid_iter = gtk_tree_model_iter_next (model, &iter))
        {
          GtkListStore *pause_buffer, *active_buffer;

          gtk_tree_model_get (service_store, &iter,
              COL_PAUSE_BUFFER, &pause_buffer,
              COL_ACTIVE_BUFFER, &active_buffer,
              -1);

          gtk_tree_model_foreach (GTK_TREE_MODEL (pause_buffer),
              copy_buffered_messages, active_buffer);
          gtk_tree_model_foreach (GTK_TREE_MODEL (pause_buffer),
              copy_buffered_messages, priv->all_active_buffer);

          gtk_list_store_clear (pause_buffer);

          g_object_unref (active_buffer);
          g_object_unref (pause_buffer);
        }
    }
}

static void
debug_window_filter_changed_cb (GtkComboBox *filter,
    EmpathyDebugWindow *debug_window)
{
  EmpathyDebugWindowPriv *priv = GET_PRIV (debug_window);

  gtk_tree_model_filter_refilter (
      GTK_TREE_MODEL_FILTER (priv->store_filter));
}

static void
debug_window_clear_clicked_cb (GtkToolButton *clear_button,
    EmpathyDebugWindow *debug_window)
{
  EmpathyDebugWindowPriv *priv = GET_PRIV (debug_window);
  GtkTreeIter iter;
  GtkListStore *active_buffer;

  gtk_combo_box_get_active_iter (GTK_COMBO_BOX (priv->chooser), &iter);
  gtk_tree_model_get (GTK_TREE_MODEL (priv->service_store), &iter,
      COL_ACTIVE_BUFFER, &active_buffer, -1);

  gtk_list_store_clear (active_buffer);

  g_object_unref (active_buffer);
}

static void
debug_window_menu_copy_activate_cb (GtkMenuItem *menu_item,
    EmpathyDebugWindow *debug_window)
{
  EmpathyDebugWindowPriv *priv = GET_PRIV (debug_window);
  GtkTreePath *path;
  GtkTreeViewColumn *focus_column;
  GtkTreeIter iter;
  gchar *message;
  GtkClipboard *clipboard;

  gtk_tree_view_get_cursor (GTK_TREE_VIEW (priv->view),
      &path, &focus_column);

  if (path == NULL)
    {
      DEBUG ("No row is in focus");
      return;
    }

  gtk_tree_model_get_iter (priv->store_filter, &iter, path);

  gtk_tree_model_get (priv->store_filter, &iter,
      COL_DEBUG_MESSAGE, &message,
      -1);

  if (EMP_STR_EMPTY (message))
    {
      DEBUG ("Log message is empty");
      return;
    }

  clipboard = gtk_clipboard_get_for_display (
      gtk_widget_get_display (GTK_WIDGET (menu_item)),
      GDK_SELECTION_CLIPBOARD);

  gtk_clipboard_set_text (clipboard, message, -1);

  g_free (message);
}

typedef struct
{
  EmpathyDebugWindow *debug_window;
  guint button;
  guint32 time;
} MenuPopupData;

static gboolean
debug_window_show_menu (gpointer user_data)
{
  MenuPopupData *data = (MenuPopupData *) user_data;
  GtkWidget *menu, *item;
  GtkMenuShell *shell;

  menu = empathy_context_menu_new (GTK_WIDGET (data->debug_window));
  shell = GTK_MENU_SHELL (menu);

  item = gtk_image_menu_item_new_from_stock (GTK_STOCK_COPY, NULL);

  g_signal_connect (item, "activate",
      G_CALLBACK (debug_window_menu_copy_activate_cb), data->debug_window);

  gtk_menu_shell_append (shell, item);
  gtk_widget_show (item);

  gtk_menu_popup (GTK_MENU (menu), NULL, NULL, NULL, NULL,
     data->button, data->time);

  g_slice_free (MenuPopupData, user_data);

  return FALSE;
}

static gboolean
debug_window_button_press_event_cb (GtkTreeView *view,
    GdkEventButton *event,
    gpointer user_data)
{
  /* A mouse button was pressed on the tree view. */

  if (event->button == 3)
    {
      /* The tree view was right-clicked. (3 == third mouse button) */
      MenuPopupData *data;
      data = g_slice_new0 (MenuPopupData);
      data->debug_window = user_data;
      data->button = event->button;
      data->time = event->time;
      g_idle_add (debug_window_show_menu, data);
    }

  return FALSE;
}

static gchar *
debug_window_format_timestamp (gdouble timestamp)
{
  struct tm *tstruct;
  char time_str[32];
  gint ms;
  time_t sec;
  gchar *text;

  ms = (int) ((timestamp - (int) timestamp)*1e6);
  sec = (long) timestamp;
  tstruct = localtime ((time_t *) &sec);
  if (!strftime (time_str, sizeof (time_str), "%x %T", tstruct))
    {
      DEBUG ("Failed to format timestamp: %e", timestamp);
      time_str[0] = '\0';
    }

  text = g_strdup_printf ("%s.%d", time_str, ms);

  return text;
}

static void
debug_window_time_formatter (GtkTreeViewColumn *tree_column,
    GtkCellRenderer *cell,
    GtkTreeModel *tree_model,
    GtkTreeIter *iter,
    gpointer data)
{
  gdouble timestamp;
  gchar *time_str;

  gtk_tree_model_get (tree_model, iter, COL_DEBUG_TIMESTAMP, &timestamp, -1);

  time_str = debug_window_format_timestamp (timestamp);

  g_object_set (G_OBJECT (cell), "text", time_str, NULL);

  g_free (time_str);
}

static gboolean
debug_window_store_filter_foreach (GtkTreeModel *model,
    GtkTreePath *path,
    GtkTreeIter *iter,
    gpointer user_data)
{
  GFileOutputStream *output_stream = (GFileOutputStream *) user_data;
  gchar *domain, *category, *message, *level_str, *level_upper;
  gdouble timestamp;
  gchar *line, *time_str;
  GError *error = NULL;
  gboolean out = FALSE;

  gtk_tree_model_get (model, iter,
      COL_DEBUG_TIMESTAMP, &timestamp,
      COL_DEBUG_DOMAIN, &domain,
      COL_DEBUG_CATEGORY, &category,
      COL_DEBUG_LEVEL_STRING, &level_str,
      COL_DEBUG_MESSAGE, &message,
      -1);

  level_upper = g_ascii_strup (level_str, -1);

  time_str = debug_window_format_timestamp (timestamp);

  line = g_strdup_printf ("%s%s%s-%s: %s: %s\n",
      domain, EMP_STR_EMPTY (category) ? "" : "/",
      category, level_upper, time_str, message);

  g_free (time_str);

  g_output_stream_write (G_OUTPUT_STREAM (output_stream), line,
      strlen (line), NULL, &error);

  if (error != NULL)
    {
      DEBUG ("Failed to write to file: %s", error->message);
      g_error_free (error);
      out = TRUE;
    }

  g_free (line);
  g_free (level_upper);
  g_free (level_str);
  g_free (domain);
  g_free (category);
  g_free (message);

  return out;
}

static void
debug_window_save_file_chooser_response_cb (GtkDialog *dialog,
    gint response_id,
    EmpathyDebugWindow *debug_window)
{
  EmpathyDebugWindowPriv *priv = GET_PRIV (debug_window);
  gchar *filename = NULL;
  GFile *gfile = NULL;
  GFileOutputStream *output_stream = NULL;
  GError *error = NULL;

  if (response_id != GTK_RESPONSE_ACCEPT)
    goto OUT;

  filename = gtk_file_chooser_get_filename (GTK_FILE_CHOOSER (dialog));

  DEBUG ("Saving log as %s", filename);

  gfile = g_file_new_for_path (filename);
  output_stream = g_file_replace (gfile, NULL, FALSE,
      G_FILE_CREATE_NONE, NULL, &error);

  if (error != NULL)
    {
      DEBUG ("Failed to open file for writing: %s", error->message);
      g_error_free (error);
      goto OUT;
    }

  gtk_tree_model_foreach (priv->store_filter,
      debug_window_store_filter_foreach, output_stream);

OUT:
  if (gfile != NULL)
    g_object_unref (gfile);

  if (output_stream != NULL)
    g_object_unref (output_stream);

  if (filename != NULL)
    g_free (filename);

  gtk_widget_destroy (GTK_WIDGET (dialog));
}

static void
debug_window_save_clicked_cb (GtkToolButton *tool_button,
    EmpathyDebugWindow *debug_window)
{
  GtkWidget *file_chooser;
  gchar *name, *tmp = NULL;
  char time_str[32];
  time_t t;
  struct tm *tm_s;

  file_chooser = gtk_file_chooser_dialog_new (_("Save"),
      GTK_WINDOW (debug_window), GTK_FILE_CHOOSER_ACTION_SAVE,
      GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
      GTK_STOCK_SAVE, GTK_RESPONSE_ACCEPT,
      NULL);

  gtk_window_set_modal (GTK_WINDOW (file_chooser), TRUE);
  gtk_file_chooser_set_do_overwrite_confirmation (
      GTK_FILE_CHOOSER (file_chooser), TRUE);

  gtk_file_chooser_set_current_folder (GTK_FILE_CHOOSER (file_chooser),
      g_get_home_dir ());

  name = get_active_service_name (debug_window);

  t = time (NULL);
  tm_s = localtime (&t);
  if (tm_s != NULL)
    {
      if (strftime(time_str, sizeof (time_str), "%d-%m-%y_%H-%M-%S", tm_s))
        tmp = g_strdup_printf ("%s-%s.log", name, time_str);
    }

  if (tmp == NULL)
    tmp = g_strdup_printf ("%s.log", name);
  g_free (name);

  gtk_file_chooser_set_current_name (GTK_FILE_CHOOSER (file_chooser), tmp);
  g_free (tmp);

  g_signal_connect (file_chooser, "response",
      G_CALLBACK (debug_window_save_file_chooser_response_cb),
      debug_window);

  gtk_widget_show (file_chooser);
}

static gboolean
debug_window_copy_model_foreach (GtkTreeModel *model,
    GtkTreePath *path,
    GtkTreeIter *iter,
    gpointer user_data)
{
  gchar **text = (gchar **) user_data;
  gchar *tmp;
  gchar *domain, *category, *message, *level_str, *level_upper;
  gdouble timestamp;
  gchar *line, *time_str;

  gtk_tree_model_get (model, iter,
      COL_DEBUG_TIMESTAMP, &timestamp,
      COL_DEBUG_DOMAIN, &domain,
      COL_DEBUG_CATEGORY, &category,
      COL_DEBUG_LEVEL_STRING, &level_str,
      COL_DEBUG_MESSAGE, &message,
      -1);

  level_upper = g_ascii_strup (level_str, -1);

  time_str = debug_window_format_timestamp (timestamp);

  line = g_strdup_printf ("%s%s%s-%s: %s: %s\n",
      domain, EMP_STR_EMPTY (category) ? "" : "/",
      category, level_upper, time_str, message);

  g_free (time_str);

  tmp = g_strconcat (*text, line, NULL);

  g_free (*text);
  g_free (line);
  g_free (level_upper);
  g_free (level_str);
  g_free (domain);
  g_free (category);
  g_free (message);

  *text = tmp;

  return FALSE;
}

static void
debug_window_copy_clicked_cb (GtkToolButton *tool_button,
    EmpathyDebugWindow *debug_window)
{
  EmpathyDebugWindowPriv *priv = GET_PRIV (debug_window);
  GtkClipboard *clipboard;
  gchar *text;

  text = g_strdup ("");

  gtk_tree_model_foreach (priv->store_filter,
      debug_window_copy_model_foreach, &text);

  clipboard = gtk_clipboard_get_for_display (
      gtk_widget_get_display (GTK_WIDGET (tool_button)),
      GDK_SELECTION_CLIPBOARD);

  DEBUG ("Copying text to clipboard (length: %" G_GSIZE_FORMAT ")",
      strlen (text));

  gtk_clipboard_set_text (clipboard, text, -1);

  g_free (text);
}

static gboolean
debug_window_key_press_event_cb (GtkWidget *widget,
    GdkEventKey *event,
    gpointer user_data)
{
  if ((event->state & GDK_CONTROL_MASK && event->keyval == GDK_KEY_w)
      || event->keyval == GDK_KEY_Escape)
    {
      gtk_widget_destroy (widget);
      return TRUE;
    }

  return FALSE;
}

static void
empathy_debug_window_select_name (EmpathyDebugWindow *self,
    const gchar *name)
{
  EmpathyDebugWindowPriv *priv = GET_PRIV (self);
  GtkTreeModel *model = GTK_TREE_MODEL (priv->service_store);
  GtkTreeIter iter;
  gchar *iter_name;
  gboolean valid, found = FALSE;

  for (valid = gtk_tree_model_get_iter_first (model, &iter);
       valid;
       valid = gtk_tree_model_iter_next (model, &iter))
    {
      gtk_tree_model_get (model, &iter,
          COL_NAME, &iter_name,
          -1);

      if (!tp_strdiff (name, iter_name))
        found = TRUE;

      g_free (iter_name);

      if (found)
        break;
    }

  if (found)
    gtk_combo_box_set_active_iter (GTK_COMBO_BOX (priv->chooser), &iter);
}

static void
am_prepared_cb (GObject *am,
    GAsyncResult *res,
    gpointer user_data)
{
  GObject *object = user_data;
  EmpathyDebugWindowPriv *priv = GET_PRIV (object);
  GtkWidget *vbox;
  GtkWidget *toolbar;
  GtkWidget *image;
  GtkWidget *label;
  GtkToolItem *item;
  GtkCellRenderer *renderer;
  GtkListStore *level_store;
  GtkTreeIter iter;
  GError *error = NULL;

  if (!tp_proxy_prepare_finish (am, res, &error))
    {
      g_warning ("Failed to prepare AM: %s", error->message);
      g_clear_error (&error);
    }

  gtk_window_set_title (GTK_WINDOW (object), _("Debug Window"));
  gtk_window_set_default_size (GTK_WINDOW (object), 800, 400);
  empathy_geometry_bind (GTK_WINDOW (object), "debug-window");

  g_signal_connect (object, "key-press-event",
      G_CALLBACK (debug_window_key_press_event_cb), NULL);

  vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_container_add (GTK_CONTAINER (object), vbox);
  gtk_widget_show (vbox);

  toolbar = gtk_toolbar_new ();
  gtk_toolbar_set_style (GTK_TOOLBAR (toolbar), GTK_TOOLBAR_BOTH_HORIZ);
  gtk_toolbar_set_show_arrow (GTK_TOOLBAR (toolbar), TRUE);
  gtk_toolbar_set_icon_size (GTK_TOOLBAR (toolbar),
      GTK_ICON_SIZE_SMALL_TOOLBAR);
  gtk_style_context_add_class (gtk_widget_get_style_context (toolbar),
			       GTK_STYLE_CLASS_PRIMARY_TOOLBAR);
  gtk_widget_show (toolbar);

  gtk_box_pack_start (GTK_BOX (vbox), toolbar, FALSE, FALSE, 0);

  /* CM */
  priv->chooser = gtk_combo_box_text_new ();
  priv->service_store = gtk_list_store_new (NUM_COLS,
      G_TYPE_STRING,  /* COL_NAME */
      G_TYPE_STRING,  /* COL_UNIQUE_NAME */
      G_TYPE_BOOLEAN, /* COL_GONE */
      G_TYPE_OBJECT,  /* COL_ACTIVE_BUFFER */
      G_TYPE_OBJECT,  /* COL_PAUSE_BUFFER */
      TP_TYPE_PROXY); /* COL_PROXY */
  gtk_combo_box_set_model (GTK_COMBO_BOX (priv->chooser),
      GTK_TREE_MODEL (priv->service_store));
  gtk_widget_show (priv->chooser);

  item = gtk_tool_item_new ();
  gtk_widget_show (GTK_WIDGET (item));
  gtk_container_add (GTK_CONTAINER (item), priv->chooser);
  gtk_toolbar_insert (GTK_TOOLBAR (toolbar), item, -1);
  g_signal_connect (priv->chooser, "changed",
      G_CALLBACK (debug_window_service_chooser_changed_cb), object);
  gtk_widget_show (GTK_WIDGET (priv->chooser));

  item = gtk_separator_tool_item_new ();
  gtk_widget_show (GTK_WIDGET (item));
  gtk_toolbar_insert (GTK_TOOLBAR (toolbar), item, -1);

  /* Save */
  priv->save_button = gtk_tool_button_new_from_stock (GTK_STOCK_SAVE);
  g_signal_connect (priv->save_button, "clicked",
      G_CALLBACK (debug_window_save_clicked_cb), object);
  gtk_widget_show (GTK_WIDGET (priv->save_button));
  gtk_tool_item_set_is_important (GTK_TOOL_ITEM (priv->save_button), TRUE);
  gtk_toolbar_insert (GTK_TOOLBAR (toolbar), priv->save_button, -1);

  /* Copy */
  priv->copy_button = gtk_tool_button_new_from_stock (GTK_STOCK_COPY);
  g_signal_connect (priv->copy_button, "clicked",
      G_CALLBACK (debug_window_copy_clicked_cb), object);
  gtk_widget_show (GTK_WIDGET (priv->copy_button));
  gtk_tool_item_set_is_important (GTK_TOOL_ITEM (priv->copy_button), TRUE);
  gtk_toolbar_insert (GTK_TOOLBAR (toolbar), priv->copy_button, -1);

  /* Clear */
  priv->clear_button = gtk_tool_button_new_from_stock (GTK_STOCK_CLEAR);
  g_signal_connect (priv->clear_button, "clicked",
      G_CALLBACK (debug_window_clear_clicked_cb), object);
  gtk_widget_show (GTK_WIDGET (priv->clear_button));
  gtk_tool_item_set_is_important (GTK_TOOL_ITEM (priv->clear_button), TRUE);
  gtk_toolbar_insert (GTK_TOOLBAR (toolbar), priv->clear_button, -1);

  item = gtk_separator_tool_item_new ();
  gtk_widget_show (GTK_WIDGET (item));
  gtk_toolbar_insert (GTK_TOOLBAR (toolbar), item, -1);

  /* Pause */
  priv->paused = FALSE;
  image = gtk_image_new_from_stock (GTK_STOCK_MEDIA_PAUSE,
      GTK_ICON_SIZE_MENU);
  gtk_widget_show (image);
  priv->pause_button = gtk_toggle_tool_button_new ();
  gtk_toggle_tool_button_set_active (
      GTK_TOGGLE_TOOL_BUTTON (priv->pause_button), priv->paused);
  g_signal_connect (priv->pause_button, "toggled",
      G_CALLBACK (debug_window_pause_toggled_cb), object);
  gtk_widget_show (GTK_WIDGET (priv->pause_button));
  gtk_tool_item_set_is_important (GTK_TOOL_ITEM (priv->pause_button), TRUE);
  gtk_tool_button_set_label (GTK_TOOL_BUTTON (priv->pause_button), _("Pause"));
  gtk_tool_button_set_icon_widget (
      GTK_TOOL_BUTTON (priv->pause_button), image);
  gtk_toolbar_insert (GTK_TOOLBAR (toolbar), priv->pause_button, -1);

  item = gtk_separator_tool_item_new ();
  gtk_widget_show (GTK_WIDGET (item));
  gtk_toolbar_insert (GTK_TOOLBAR (toolbar), item, -1);

  /* Level */
  priv->level_label = gtk_tool_item_new ();
  gtk_widget_show (GTK_WIDGET (priv->level_label));
  label = gtk_label_new (_("Level "));
  gtk_widget_show (label);
  gtk_container_add (GTK_CONTAINER (priv->level_label), label);
  gtk_toolbar_insert (GTK_TOOLBAR (toolbar), priv->level_label, -1);

  priv->level_filter = gtk_combo_box_text_new ();
  gtk_widget_show (priv->level_filter);

  item = gtk_tool_item_new ();
  gtk_widget_show (GTK_WIDGET (item));
  gtk_container_add (GTK_CONTAINER (item), priv->level_filter);
  gtk_toolbar_insert (GTK_TOOLBAR (toolbar), item, -1);

  level_store = gtk_list_store_new (NUM_COLS_LEVEL,
      G_TYPE_STRING, G_TYPE_UINT);
  gtk_combo_box_set_model (GTK_COMBO_BOX (priv->level_filter),
      GTK_TREE_MODEL (level_store));

  gtk_list_store_insert_with_values (level_store, &iter, -1,
      COL_LEVEL_NAME, _("Debug"),
      COL_LEVEL_VALUE, TP_DEBUG_LEVEL_DEBUG,
      -1);

  gtk_list_store_insert_with_values (level_store, &iter, -1,
      COL_LEVEL_NAME, _("Info"),
      COL_LEVEL_VALUE, TP_DEBUG_LEVEL_INFO,
      -1);

  gtk_list_store_insert_with_values (level_store, &iter, -1,
      COL_LEVEL_NAME, _("Message"),
      COL_LEVEL_VALUE, TP_DEBUG_LEVEL_MESSAGE,
      -1);

  gtk_list_store_insert_with_values (level_store, &iter, -1,
      COL_LEVEL_NAME, _("Warning"),
      COL_LEVEL_VALUE, TP_DEBUG_LEVEL_WARNING,
      -1);

  gtk_list_store_insert_with_values (level_store, &iter, -1,
      COL_LEVEL_NAME, _("Critical"),
      COL_LEVEL_VALUE, TP_DEBUG_LEVEL_CRITICAL,
      -1);

  gtk_list_store_insert_with_values (level_store, &iter, -1,
      COL_LEVEL_NAME, _("Error"),
      COL_LEVEL_VALUE, TP_DEBUG_LEVEL_ERROR,
      -1);

  gtk_combo_box_set_active (GTK_COMBO_BOX (priv->level_filter), 0);
  g_signal_connect (priv->level_filter, "changed",
      G_CALLBACK (debug_window_filter_changed_cb), object);

  /* Debug treeview */
  priv->view = gtk_tree_view_new ();
  gtk_tree_view_set_rules_hint (GTK_TREE_VIEW (priv->view), TRUE);

  g_signal_connect (priv->view, "button-press-event",
      G_CALLBACK (debug_window_button_press_event_cb), object);

  renderer = gtk_cell_renderer_text_new ();
  g_object_set (renderer, "yalign", 0, NULL);

  gtk_tree_view_insert_column_with_data_func (GTK_TREE_VIEW (priv->view),
      -1, _("Time"), renderer,
      (GtkTreeCellDataFunc) debug_window_time_formatter, NULL, NULL);
  gtk_tree_view_insert_column_with_attributes (GTK_TREE_VIEW (priv->view),
      -1, _("Domain"), renderer, "text", COL_DEBUG_DOMAIN, NULL);
  gtk_tree_view_insert_column_with_attributes (GTK_TREE_VIEW (priv->view),
      -1, _("Category"), renderer, "text", COL_DEBUG_CATEGORY, NULL);
  gtk_tree_view_insert_column_with_attributes (GTK_TREE_VIEW (priv->view),
      -1, _("Level"), renderer, "text", COL_DEBUG_LEVEL_STRING, NULL);

  renderer = gtk_cell_renderer_text_new ();
  g_object_set (renderer, "family", "Monospace", NULL);
  gtk_tree_view_insert_column_with_attributes (GTK_TREE_VIEW (priv->view),
      -1, _("Message"), renderer, "text", COL_DEBUG_MESSAGE, NULL);

  priv->store_filter = NULL;

  gtk_tree_view_set_model (GTK_TREE_VIEW (priv->view), priv->store_filter);

  /* Scrolled window */
  priv->scrolled_win = g_object_ref (gtk_scrolled_window_new (NULL, NULL));
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (priv->scrolled_win),
      GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);

  gtk_widget_show (priv->view);
  gtk_container_add (GTK_CONTAINER (priv->scrolled_win), priv->view);

  gtk_widget_show (priv->scrolled_win);

  /* Not supported label */
  priv->not_supported_label = g_object_ref (gtk_label_new (
          _("The selected connection manager does not support the remote "
              "debugging extension.")));
  gtk_widget_show (priv->not_supported_label);
  gtk_box_pack_start (GTK_BOX (vbox), priv->not_supported_label,
      TRUE, TRUE, 0);

  priv->view_visible = FALSE;

  priv->all_active_buffer = NULL;

  debug_window_set_toolbar_sensitivity (EMPATHY_DEBUG_WINDOW (object), FALSE);
  debug_window_fill_service_chooser (EMPATHY_DEBUG_WINDOW (object));
  gtk_widget_show (GTK_WIDGET (object));
}

static void
debug_window_constructed (GObject *object)
{
  EmpathyDebugWindowPriv *priv = GET_PRIV (object);

  priv->am = tp_account_manager_dup ();
  tp_proxy_prepare_async (priv->am, NULL, am_prepared_cb, object);
}

static void
empathy_debug_window_init (EmpathyDebugWindow *empathy_debug_window)
{
  EmpathyDebugWindowPriv *priv =
      G_TYPE_INSTANCE_GET_PRIVATE (empathy_debug_window,
      EMPATHY_TYPE_DEBUG_WINDOW, EmpathyDebugWindowPriv);

  empathy_debug_window->priv = priv;

  priv->dispose_run = FALSE;
}

static void
debug_window_set_property (GObject *object,
    guint prop_id,
    const GValue *value,
    GParamSpec *pspec)
{
  switch (prop_id)
    {
      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
        break;
    }
}

static void
debug_window_get_property (GObject *object,
    guint prop_id,
    GValue *value,
    GParamSpec *pspec)
{
  switch (prop_id)
    {
      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
        break;
    }
}

static void
debug_window_finalize (GObject *object)
{
  EmpathyDebugWindowPriv *priv = GET_PRIV (object);

  g_free (priv->select_name);

  (G_OBJECT_CLASS (empathy_debug_window_parent_class)->finalize) (object);
}

static void
debug_window_dispose (GObject *object)
{
  EmpathyDebugWindow *selector = EMPATHY_DEBUG_WINDOW (object);
  EmpathyDebugWindowPriv *priv = GET_PRIV (selector);

  if (priv->dispose_run)
    return;

  priv->dispose_run = TRUE;

  if (priv->name_owner_changed_signal != NULL)
    tp_proxy_signal_connection_disconnect (priv->name_owner_changed_signal);

  if (priv->service_store != NULL)
    g_object_unref (priv->service_store);

  if (priv->dbus != NULL)
    g_object_unref (priv->dbus);

  if (priv->am != NULL)
    {
      g_object_unref (priv->am);
      priv->am = NULL;
    }

  tp_clear_object (&priv->all_active_buffer);

  (G_OBJECT_CLASS (empathy_debug_window_parent_class)->dispose) (object);
}

static void
empathy_debug_window_class_init (EmpathyDebugWindowClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->constructed = debug_window_constructed;
  object_class->dispose = debug_window_dispose;
  object_class->finalize = debug_window_finalize;
  object_class->set_property = debug_window_set_property;
  object_class->get_property = debug_window_get_property;
  g_type_class_add_private (klass, sizeof (EmpathyDebugWindowPriv));
}

/* public methods */

GtkWidget *
empathy_debug_window_new (GtkWindow *parent)
{
  g_return_val_if_fail (parent == NULL || GTK_IS_WINDOW (parent), NULL);

  return GTK_WIDGET (g_object_new (EMPATHY_TYPE_DEBUG_WINDOW,
      "transient-for", parent, NULL));
}

void
empathy_debug_window_show (EmpathyDebugWindow *self,
    const gchar *name)
{
  EmpathyDebugWindowPriv *priv = GET_PRIV (self);

  if (priv->service_store != NULL)
    {
      empathy_debug_window_select_name (self, name);
    }
  else
    {
      g_free (priv->select_name);
      priv->select_name = g_strdup (name);
    }
}
