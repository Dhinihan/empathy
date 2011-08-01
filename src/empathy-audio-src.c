/*
 * empathy-gst-audio-src.c - Source for EmpathyGstAudioSrc
 * Copyright (C) 2008 Collabora Ltd.
 * @author Sjoerd Simons <sjoerd.simons@collabora.co.uk>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */


#include <stdio.h>
#include <stdlib.h>

#include <pulse/pulseaudio.h>
#include <pulse/glib-mainloop.h>

#include <libempathy/empathy-utils.h>

#include "empathy-audio-src.h"

#include "src-marshal.h"

#define DEBUG_FLAG EMPATHY_DEBUG_VOIP
#include <libempathy/empathy-debug.h>

G_DEFINE_TYPE(EmpathyGstAudioSrc, empathy_audio_src, GST_TYPE_BIN)

/* signal enum */
enum
{
    PEAK_LEVEL_CHANGED,
    RMS_LEVEL_CHANGED,
    MICROPHONE_ADDED,
    MICROPHONE_REMOVED,
    LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = {0};

enum {
    PROP_VOLUME = 1,
    PROP_RMS_LEVEL,
    PROP_PEAK_LEVEL,
    PROP_MICROPHONE,
};

/* private structure */
typedef struct _EmpathyGstAudioSrcPrivate EmpathyGstAudioSrcPrivate;

struct _EmpathyGstAudioSrcPrivate
{
  gboolean dispose_has_run;
  GstElement *src;
  GstElement *volume;
  GstElement *level;

  pa_glib_mainloop *loop;
  pa_context *context;
  GQueue *operations;

  /* 0 if not known yet */
  guint source_output_idx;
  /* G_MAXUINT if not known yet */
  guint source_idx;

  gdouble peak_level;
  gdouble rms_level;

  GMutex *lock;
  guint idle_id;
};

#define EMPATHY_GST_AUDIO_SRC_GET_PRIVATE(o) \
  (G_TYPE_INSTANCE_GET_PRIVATE ((o), EMPATHY_TYPE_GST_AUDIO_SRC, \
  EmpathyGstAudioSrcPrivate))

static gboolean
empathy_audio_src_supports_changing_mic (EmpathyGstAudioSrc *self)
{
  EmpathyGstAudioSrcPrivate *priv = EMPATHY_GST_AUDIO_SRC_GET_PRIVATE (self);
  GObjectClass *object_class;

  object_class = G_OBJECT_GET_CLASS (priv->src);

  return (g_object_class_find_property (object_class,
          "source-output-index") != NULL);
}

typedef void (*OperationFunc) (EmpathyGstAudioSrc *, GSimpleAsyncResult *);

typedef struct
{
  OperationFunc func;
  GSimpleAsyncResult *result;
} Operation;

static Operation *
operation_new (OperationFunc func,
    GSimpleAsyncResult *result)
{
  Operation *o = g_slice_new0 (Operation);

  o->func = func;
  o->result = result;

  return o;
}

static void
operation_free (Operation *o,
    gboolean cancelled)
{
  if (cancelled)
    {
      g_simple_async_result_set_error (o->result,
          G_IO_ERROR, G_IO_ERROR_CANCELLED,
          "The audio source was disposed");
      g_simple_async_result_complete (o->result);
      g_object_unref (o->result);
    }

  g_slice_free (Operation, o);
}

static void
operation_get_microphones_free (gpointer data)
{
  GQueue *queue = data;
  GList *l;

  for (l = queue->head; l != NULL; l = l->next)
    {
      EmpathyAudioSrcMicrophone *mic = l->data;

      g_free (mic->name);
      g_free (mic->description);
      g_slice_free (EmpathyAudioSrcMicrophone, mic);
    }

  g_queue_free (queue);
}

static void
operation_get_microphones_cb (pa_context *context,
    const pa_source_info *info,
    int eol,
    void *userdata)
{
  GSimpleAsyncResult *result = userdata;
  EmpathyAudioSrcMicrophone *mic;
  GQueue *queue;

  if (eol)
    {
      g_simple_async_result_complete (result);
      g_object_unref (result);
      return;
    }

  mic = g_slice_new0 (EmpathyAudioSrcMicrophone);
  mic->index = info->index;
  mic->name = g_strdup (info->name);
  mic->description = g_strdup (info->description);
  mic->is_monitor = (info->monitor_of_sink != PA_INVALID_INDEX);

  /* add it to the queue */
  queue = g_simple_async_result_get_op_res_gpointer (result);
  g_queue_push_tail (queue, mic);
}

static void
operation_get_microphones (EmpathyGstAudioSrc *self,
    GSimpleAsyncResult *result)
{
  EmpathyGstAudioSrcPrivate *priv = EMPATHY_GST_AUDIO_SRC_GET_PRIVATE (self);

  g_assert_cmpuint (pa_context_get_state (priv->context), ==, PA_CONTEXT_READY);

  g_simple_async_result_set_op_res_gpointer (result, g_queue_new (),
      operation_get_microphones_free);

  pa_context_get_source_info_list (priv->context,
      operation_get_microphones_cb, result);
}

static void
operation_change_microphone_cb (pa_context *context,
    int success,
    void *userdata)
{
  GSimpleAsyncResult *result = userdata;

  if (!success)
    {
      g_simple_async_result_set_error (result, G_IO_ERROR, G_IO_ERROR_FAILED,
          "Failed to change microphone. Reason unknown.");
    }

  g_simple_async_result_complete (result);
  g_object_unref (result);
}

static void
operation_change_microphone (EmpathyGstAudioSrc *self,
    GSimpleAsyncResult *result)
{
  EmpathyGstAudioSrcPrivate *priv = EMPATHY_GST_AUDIO_SRC_GET_PRIVATE (self);
  guint source_output_idx, microphone;

  g_object_get (priv->src, "source-output-index", &source_output_idx, NULL);

  g_assert_cmpuint (pa_context_get_state (priv->context), ==, PA_CONTEXT_READY);
  g_assert_cmpuint (source_output_idx, !=, PA_INVALID_INDEX);

  microphone = GPOINTER_TO_UINT (
      g_simple_async_result_get_op_res_gpointer (result));

  pa_context_move_source_output_by_index (priv->context, source_output_idx, microphone,
      operation_change_microphone_cb, result);
}

static void
operations_run (EmpathyGstAudioSrc *self)
{
  EmpathyGstAudioSrcPrivate *priv = EMPATHY_GST_AUDIO_SRC_GET_PRIVATE (self);
  pa_context_state_t state = pa_context_get_state (priv->context);
  GList *l;

  if (state != PA_CONTEXT_READY)
    return;

  for (l = priv->operations->head; l != NULL; l = l->next)
    {
      Operation *o = l->data;

      o->func (self, o->result);

      operation_free (o, FALSE);
    }

  g_queue_clear (priv->operations);
}

static void
empathy_audio_src_source_output_info_cb (pa_context *context,
    const pa_source_output_info *info,
    int eol,
    void *userdata)
{
  EmpathyGstAudioSrc *self = userdata;
  EmpathyGstAudioSrcPrivate *priv = EMPATHY_GST_AUDIO_SRC_GET_PRIVATE (self);

  if (eol)
    return;

  /* There should only be one call here. */

  if (priv->source_idx == info->source)
    return;

  priv->source_idx = info->source;
  g_object_notify (G_OBJECT (self), "microphone");
}

static void
empathy_audio_src_source_info_cb (pa_context *context,
    const pa_source_info *info,
    int eol,
    void *userdata)
{
  EmpathyGstAudioSrc *self = userdata;
  gboolean is_monitor;

  if (eol)
    return;

  is_monitor = (info->monitor_of_sink != PA_INVALID_INDEX);

  g_signal_emit (self, signals[MICROPHONE_ADDED], 0,
      info->index, info->name, info->description, is_monitor);
}

static void
empathy_audio_src_pa_event_cb (pa_context *context,
    pa_subscription_event_type_t type,
    uint32_t idx,
    void *userdata)
{
  EmpathyGstAudioSrc *self = userdata;
  EmpathyGstAudioSrcPrivate *priv = EMPATHY_GST_AUDIO_SRC_GET_PRIVATE (self);

  if ((type & PA_SUBSCRIPTION_EVENT_FACILITY_MASK) == PA_SUBSCRIPTION_EVENT_SOURCE_OUTPUT
      && (type & PA_SUBSCRIPTION_EVENT_TYPE_MASK) == PA_SUBSCRIPTION_EVENT_CHANGE
      && idx == priv->source_output_idx)
    {
      /* Microphone in the source output has changed */
      pa_context_get_source_output_info (context, idx,
          empathy_audio_src_source_output_info_cb, self);
    }
  else if ((type & PA_SUBSCRIPTION_EVENT_FACILITY_MASK) == PA_SUBSCRIPTION_EVENT_SOURCE
      && (type & PA_SUBSCRIPTION_EVENT_TYPE_MASK) == PA_SUBSCRIPTION_EVENT_REMOVE)
    {
      /* A mic has been removed */
      g_signal_emit (self, signals[MICROPHONE_REMOVED], 0, idx);
    }
  else if ((type & PA_SUBSCRIPTION_EVENT_FACILITY_MASK) == PA_SUBSCRIPTION_EVENT_SOURCE
      && (type & PA_SUBSCRIPTION_EVENT_TYPE_MASK) == PA_SUBSCRIPTION_EVENT_NEW)
    {
      /* A mic has been plugged in */
      pa_context_get_source_info_by_index (context, idx,
          empathy_audio_src_source_info_cb, self);
    }
}

static void
empathy_audio_src_pa_subscribe_cb (pa_context *context,
    int success,
    void *userdata)
{
  if (!success)
    DEBUG ("Failed to subscribe to PulseAudio events");
}

static void
empathy_audio_src_pa_state_change_cb (pa_context *context,
    void *userdata)
{
  EmpathyGstAudioSrc *self = userdata;
  EmpathyGstAudioSrcPrivate *priv = EMPATHY_GST_AUDIO_SRC_GET_PRIVATE (self);
  pa_context_state_t state = pa_context_get_state (priv->context);

  if (state == PA_CONTEXT_READY)
    {
      /* Listen to pulseaudio events so we know when sources are
       * added and when the microphone is changed. */
      pa_context_set_subscribe_callback (priv->context,
          empathy_audio_src_pa_event_cb, self);
      pa_context_subscribe (priv->context,
          PA_SUBSCRIPTION_MASK_SOURCE | PA_SUBSCRIPTION_MASK_SOURCE_OUTPUT,
          empathy_audio_src_pa_subscribe_cb, NULL);

      operations_run (self);
    }
}

static void
empathy_audio_src_source_output_index_notify (GObject *object,
    GParamSpec *pspec,
    EmpathyGstAudioSrc *self)
{
  EmpathyGstAudioSrcPrivate *priv = EMPATHY_GST_AUDIO_SRC_GET_PRIVATE (self);
  guint source_output_idx = PA_INVALID_INDEX;

  g_object_get (priv->src, "source-output-index", &source_output_idx, NULL);

  if (source_output_idx == PA_INVALID_INDEX)
    return;

  if (priv->source_output_idx == source_output_idx)
    return;

  /* It's actually changed. */
  priv->source_output_idx = source_output_idx;

  pa_context_get_source_output_info (priv->context, source_output_idx,
      empathy_audio_src_source_output_info_cb, self);
}

static void
empathy_audio_src_init (EmpathyGstAudioSrc *obj)
{
  EmpathyGstAudioSrcPrivate *priv = EMPATHY_GST_AUDIO_SRC_GET_PRIVATE (obj);
  GstPad *ghost, *src;
  const gchar *src_element;

  priv->peak_level = -G_MAXDOUBLE;
  priv->lock = g_mutex_new ();

  src_element = g_getenv ("EMPATHY_AUDIO_SRC");
  if (src_element == NULL)
    src_element = "pulsesrc";

  priv->src = gst_element_factory_make (src_element, NULL);
  gst_bin_add (GST_BIN (obj), priv->src);

  priv->volume = gst_element_factory_make ("volume", NULL);
  g_object_ref (priv->volume);

  gst_bin_add (GST_BIN (obj), priv->volume);
  gst_element_link (priv->src, priv->volume);

  priv->level = gst_element_factory_make ("level", NULL);
  gst_bin_add (GST_BIN (obj), priv->level);
  gst_element_link (priv->volume, priv->level);

  src = gst_element_get_static_pad (priv->level, "src");

  ghost = gst_ghost_pad_new ("src", src);
  gst_element_add_pad (GST_ELEMENT (obj), ghost);

  gst_object_unref (G_OBJECT (src));

  /* PulseAudio stuff: We need to create a dummy pa_glib_mainloop* so
   * Pulse can use the mainloop that GTK has created for us. */
  priv->loop = pa_glib_mainloop_new (NULL);
  priv->context = pa_context_new (pa_glib_mainloop_get_api (priv->loop),
      "EmpathyAudioSrc");

  /* Listen to changes to GstPulseSrc:source-output-index so we know when
   * it's no longer PA_INVALID_INDEX (starting for the first time) or if it
   * changes (READY->NULL->READY...) */
  g_signal_connect (priv->src, "notify::source-output-index",
      G_CALLBACK (empathy_audio_src_source_output_index_notify),
      obj);

  /* Finally listen for state changes so we know when we've
   * connected. */
  pa_context_set_state_callback (priv->context,
      empathy_audio_src_pa_state_change_cb, obj);
  pa_context_connect (priv->context, NULL, 0, NULL);

  priv->operations = g_queue_new ();
}

static void empathy_audio_src_dispose (GObject *object);
static void empathy_audio_src_finalize (GObject *object);
static void empathy_audio_src_handle_message (GstBin *bin,
  GstMessage *message);

static gboolean empathy_audio_src_levels_updated (gpointer user_data);

static void
empathy_audio_src_set_property (GObject *object,
  guint property_id, const GValue *value, GParamSpec *pspec)
{
  switch (property_id)
    {
      case PROP_VOLUME:
        empathy_audio_src_set_volume (EMPATHY_GST_AUDIO_SRC (object),
          g_value_get_double (value));
        break;
      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    }
}

static void
empathy_audio_src_get_property (GObject *object,
  guint property_id, GValue *value, GParamSpec *pspec)
{
  EmpathyGstAudioSrc *self = EMPATHY_GST_AUDIO_SRC (object);
  EmpathyGstAudioSrcPrivate *priv = EMPATHY_GST_AUDIO_SRC_GET_PRIVATE (self);

  switch (property_id)
    {
      case PROP_VOLUME:
        g_value_set_double (value,
          empathy_audio_src_get_volume (self));
        break;
      case PROP_PEAK_LEVEL:
        g_mutex_lock (priv->lock);
        g_value_set_double (value, priv->peak_level);
        g_mutex_unlock (priv->lock);
        break;
      case PROP_RMS_LEVEL:
        g_mutex_lock (priv->lock);
        g_value_set_double (value, priv->rms_level);
        g_mutex_unlock (priv->lock);
        break;
      case PROP_MICROPHONE:
        g_value_set_uint (value, priv->source_idx);
        break;
      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    }
}

static void
empathy_audio_src_class_init (EmpathyGstAudioSrcClass
  *empathy_audio_src_class)
{
  GObjectClass *object_class = G_OBJECT_CLASS (empathy_audio_src_class);
  GstBinClass *gstbin_class = GST_BIN_CLASS (empathy_audio_src_class);
  GParamSpec *param_spec;

  g_type_class_add_private (empathy_audio_src_class,
    sizeof (EmpathyGstAudioSrcPrivate));

  object_class->dispose = empathy_audio_src_dispose;
  object_class->finalize = empathy_audio_src_finalize;

  object_class->set_property = empathy_audio_src_set_property;
  object_class->get_property = empathy_audio_src_get_property;

  gstbin_class->handle_message =
    GST_DEBUG_FUNCPTR (empathy_audio_src_handle_message);

  param_spec = g_param_spec_double ("volume", "Volume", "volume contol",
    0.0, 5.0, 1.0,
    G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_VOLUME, param_spec);

  param_spec = g_param_spec_double ("peak-level", "peak level", "peak level",
    -G_MAXDOUBLE, G_MAXDOUBLE, 0,
    G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_PEAK_LEVEL, param_spec);

  param_spec = g_param_spec_uint ("microphone", "microphone", "microphone",
    0, G_MAXUINT, G_MAXUINT,
    G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_MICROPHONE, param_spec);

  signals[PEAK_LEVEL_CHANGED] = g_signal_new ("peak-level-changed",
    G_TYPE_FROM_CLASS (empathy_audio_src_class),
    G_SIGNAL_RUN_LAST,
    0,
    NULL, NULL,
    g_cclosure_marshal_VOID__DOUBLE,
    G_TYPE_NONE, 1, G_TYPE_DOUBLE);

  param_spec = g_param_spec_double ("rms-level", "RMS level", "RMS level",
    -G_MAXDOUBLE, G_MAXDOUBLE, 0,
    G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_RMS_LEVEL, param_spec);


  signals[RMS_LEVEL_CHANGED] = g_signal_new ("rms-level-changed",
    G_TYPE_FROM_CLASS (empathy_audio_src_class),
    G_SIGNAL_RUN_LAST,
    0,
    NULL, NULL,
    g_cclosure_marshal_VOID__DOUBLE,
    G_TYPE_NONE, 1, G_TYPE_DOUBLE);

  signals[MICROPHONE_ADDED] = g_signal_new ("microphone-added",
    G_TYPE_FROM_CLASS (empathy_audio_src_class),
    G_SIGNAL_RUN_LAST,
    0,
    NULL, NULL,
    _src_marshal_VOID__UINT_STRING_STRING_BOOLEAN,
    G_TYPE_NONE, 4, G_TYPE_UINT, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_BOOLEAN);

  signals[MICROPHONE_REMOVED] = g_signal_new ("microphone-removed",
    G_TYPE_FROM_CLASS (empathy_audio_src_class),
    G_SIGNAL_RUN_LAST,
    0,
    NULL, NULL,
    g_cclosure_marshal_VOID__UINT,
    G_TYPE_NONE, 1, G_TYPE_UINT);
}

void
empathy_audio_src_dispose (GObject *object)
{
  EmpathyGstAudioSrc *self = EMPATHY_GST_AUDIO_SRC (object);
  EmpathyGstAudioSrcPrivate *priv = EMPATHY_GST_AUDIO_SRC_GET_PRIVATE (self);

  if (priv->dispose_has_run)
    return;

  priv->dispose_has_run = TRUE;

  if (priv->idle_id != 0)
    g_source_remove (priv->idle_id);

  priv->idle_id = 0;

  if (priv->context != NULL)
    pa_context_unref (priv->context);
  priv->context = NULL;

  if (priv->loop != NULL)
    pa_glib_mainloop_free (priv->loop);
  priv->loop = NULL;

  /* release any references held by the object here */

  if (G_OBJECT_CLASS (empathy_audio_src_parent_class)->dispose)
    G_OBJECT_CLASS (empathy_audio_src_parent_class)->dispose (object);
}

void
empathy_audio_src_finalize (GObject *object)
{
  EmpathyGstAudioSrc *self = EMPATHY_GST_AUDIO_SRC (object);
  EmpathyGstAudioSrcPrivate *priv = EMPATHY_GST_AUDIO_SRC_GET_PRIVATE (self);

  /* free any data held directly by the object here */
  g_mutex_free (priv->lock);

  g_queue_foreach (priv->operations, (GFunc) operation_free,
      GUINT_TO_POINTER (TRUE));
  g_queue_free (priv->operations);

  G_OBJECT_CLASS (empathy_audio_src_parent_class)->finalize (object);
}

static gboolean
empathy_audio_src_levels_updated (gpointer user_data)
{
  EmpathyGstAudioSrc *self = EMPATHY_GST_AUDIO_SRC (user_data);
  EmpathyGstAudioSrcPrivate *priv = EMPATHY_GST_AUDIO_SRC_GET_PRIVATE (self);

  g_mutex_lock (priv->lock);

  g_signal_emit (self, signals[PEAK_LEVEL_CHANGED], 0, priv->peak_level);
  g_signal_emit (self, signals[RMS_LEVEL_CHANGED], 0, priv->rms_level);
  priv->idle_id = 0;

  g_mutex_unlock (priv->lock);

  return FALSE;
}

static void
empathy_audio_src_handle_message (GstBin *bin, GstMessage *message)
{
  EmpathyGstAudioSrc *self = EMPATHY_GST_AUDIO_SRC (bin);
  EmpathyGstAudioSrcPrivate *priv = EMPATHY_GST_AUDIO_SRC_GET_PRIVATE (self);

  if  (GST_MESSAGE_TYPE (message) == GST_MESSAGE_ELEMENT &&
        GST_MESSAGE_SRC (message) == GST_OBJECT (priv->level))
    {
      const GstStructure *s;
      const gchar *name;
      const GValue *list;
      guint i, len;
      gdouble peak = -G_MAXDOUBLE;
      gdouble rms = -G_MAXDOUBLE;

      s = gst_message_get_structure (message);
      name = gst_structure_get_name (s);

      if (g_strcmp0 ("level", name) != 0)
        goto out;

      list = gst_structure_get_value (s, "peak");
      len = gst_value_list_get_size (list);

      for (i =0 ; i < len; i++)
        {
          const GValue *value;
          gdouble db;

          value = gst_value_list_get_value (list, i);
          db = g_value_get_double (value);
          peak = MAX (db, peak);
        }

      list = gst_structure_get_value (s, "rms");
      len = gst_value_list_get_size (list);

      for (i =0 ; i < len; i++)
        {
          const GValue *value;
          gdouble db;

          value = gst_value_list_get_value (list, i);
          db = g_value_get_double (value);
          rms = MAX (db, rms);
        }

      g_mutex_lock (priv->lock);

      priv->peak_level = peak;
      priv->rms_level = rms;
      if (priv->idle_id == 0)
        priv->idle_id = g_idle_add (empathy_audio_src_levels_updated, self);

      g_mutex_unlock (priv->lock);
    }

out:
   GST_BIN_CLASS (empathy_audio_src_parent_class)->handle_message (bin,
    message);
}

GstElement *
empathy_audio_src_new (void)
{
  static gboolean registered = FALSE;

  if (!registered) {
    if (!gst_element_register (NULL, "empathyaudiosrc",
            GST_RANK_NONE, EMPATHY_TYPE_GST_AUDIO_SRC))
      return NULL;
    registered = TRUE;
  }
  return gst_element_factory_make ("empathyaudiosrc", NULL);
}

void
empathy_audio_src_set_volume (EmpathyGstAudioSrc *src, gdouble volume)
{
  EmpathyGstAudioSrcPrivate *priv = EMPATHY_GST_AUDIO_SRC_GET_PRIVATE (src);
  GParamSpec *pspec;
  GParamSpecDouble *pspec_double;

  pspec = g_object_class_find_property (G_OBJECT_GET_CLASS (priv->volume),
    "volume");

  g_assert (pspec != NULL);

  pspec_double = G_PARAM_SPEC_DOUBLE (pspec);

  volume = CLAMP (volume, pspec_double->minimum, pspec_double->maximum);

  g_object_set (G_OBJECT (priv->volume), "volume", volume, NULL);
}

gdouble
empathy_audio_src_get_volume (EmpathyGstAudioSrc *src)
{
  EmpathyGstAudioSrcPrivate *priv = EMPATHY_GST_AUDIO_SRC_GET_PRIVATE (src);
  gdouble volume;

  g_object_get (G_OBJECT (priv->volume), "volume", &volume, NULL);

  return volume;
}

void
empathy_audio_src_get_microphones_async (EmpathyGstAudioSrc *src,
    GAsyncReadyCallback callback,
    gpointer user_data)
{
  EmpathyGstAudioSrcPrivate *priv = EMPATHY_GST_AUDIO_SRC_GET_PRIVATE (src);
  Operation *operation;
  GSimpleAsyncResult *simple;

  simple = g_simple_async_result_new (G_OBJECT (src), callback, user_data,
      empathy_audio_src_get_microphones_async);

  /* If we can't change mic let's not pretend we can by returning the
   * list of available mics. */
  if (!empathy_audio_src_supports_changing_mic (src))
    {
      g_simple_async_result_set_error (simple, G_IO_ERROR, G_IO_ERROR_FAILED,
          "pulsesrc is not new enough to support changing microphone");
      g_simple_async_result_complete_in_idle (simple);
      g_object_unref (simple);
      return;
    }

  operation = operation_new (operation_get_microphones, simple);
  g_queue_push_tail (priv->operations, operation);

  /* gogogogo */
  operations_run (src);
}

const GList *
empathy_audio_src_get_microphones_finish (EmpathyGstAudioSrc *src,
    GAsyncResult *result,
    GError **error)
{
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (result);
  GQueue *queue;

  if (g_simple_async_result_propagate_error (simple, error))
      return NULL;

  g_return_val_if_fail (g_simple_async_result_is_valid (result,
          G_OBJECT (src), empathy_audio_src_get_microphones_async),
      NULL);

  queue = g_simple_async_result_get_op_res_gpointer (simple);
  return queue->head;
}

guint
empathy_audio_src_get_microphone (EmpathyGstAudioSrc *src)
{
  EmpathyGstAudioSrcPrivate *priv = EMPATHY_GST_AUDIO_SRC_GET_PRIVATE (src);

  return priv->source_idx;
}

void
empathy_audio_src_change_microphone_async (EmpathyGstAudioSrc *src,
    guint microphone,
    GAsyncReadyCallback callback,
    gpointer user_data)
{
  EmpathyGstAudioSrcPrivate *priv = EMPATHY_GST_AUDIO_SRC_GET_PRIVATE (src);
  guint source_output_idx;
  GSimpleAsyncResult *simple;
  Operation *operation;

  simple = g_simple_async_result_new (G_OBJECT (src), callback, user_data,
      empathy_audio_src_change_microphone_async);

  if (!empathy_audio_src_supports_changing_mic (src))
    {
      g_simple_async_result_set_error (simple, G_IO_ERROR, G_IO_ERROR_FAILED,
          "pulsesrc is not new enough to support changing microphone");
      g_simple_async_result_complete_in_idle (simple);
      g_object_unref (simple);
      return;
    }

  g_object_get (priv->src, "source-output-index", &source_output_idx, NULL);

  if (source_output_idx == PA_INVALID_INDEX)
    {
      g_simple_async_result_set_error (simple, G_IO_ERROR, G_IO_ERROR_FAILED,
          "pulsesrc is not yet PLAYING");
      g_simple_async_result_complete_in_idle (simple);
      g_object_unref (simple);
      return;
    }

  g_simple_async_result_set_op_res_gpointer (simple,
      GUINT_TO_POINTER (microphone), NULL);

  operation = operation_new (operation_change_microphone, simple);
  g_queue_push_tail (priv->operations, operation);

  /* gogogogo */
  operations_run (src);
}

gboolean
empathy_audio_src_change_microphone_finish (EmpathyGstAudioSrc *src,
    GAsyncResult *result,
    GError **error)
{
  empathy_implement_finish_void (src,
      empathy_audio_src_change_microphone_async);
}
