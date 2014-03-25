/*
 * (C) Copyright 2013 Kurento (http://kurento.org/)
 *
 * All rights reserved. This program and the accompanying materials
 * are made available under the terms of the GNU Lesser General Public License
 * (LGPL) version 2.1 which accompanies this distribution, and is available at
 * http://www.gnu.org/licenses/lgpl-2.1.html
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 */

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include "kmsagnosticbin.h"
#include "kmsagnosticcaps.h"
#include "kmsutils.h"
#include "kmsloop.h"

#define PLUGIN_NAME "agnosticbin"

static GstStaticCaps static_audio_caps =
GST_STATIC_CAPS (KMS_AGNOSTIC_AUDIO_CAPS);
static GstStaticCaps static_video_caps =
GST_STATIC_CAPS (KMS_AGNOSTIC_VIDEO_CAPS);
static GstStaticCaps static_raw_audio_caps =
GST_STATIC_CAPS (KMS_AGNOSTIC_RAW_AUDIO_CAPS);
static GstStaticCaps static_raw_video_caps =
GST_STATIC_CAPS (KMS_AGNOSTIC_RAW_VIDEO_CAPS);

#define GST_CAT_DEFAULT kms_agnostic_bin2_debug
GST_DEBUG_CATEGORY_STATIC (GST_CAT_DEFAULT);

#define kms_agnostic_bin2_parent_class parent_class
G_DEFINE_TYPE (KmsAgnosticBin2, kms_agnostic_bin2, GST_TYPE_BIN);

#define KMS_AGNOSTIC_BIN2_GET_PRIVATE(obj) ( \
  G_TYPE_INSTANCE_GET_PRIVATE (              \
    (obj),                                   \
    KMS_TYPE_AGNOSTIC_BIN2,                  \
    KmsAgnosticBin2Private                   \
  )                                          \
)

#define KMS_AGNOSTIC_BIN2_GET_LOCK(obj) (       \
  &KMS_AGNOSTIC_BIN2 (obj)->priv->thread_mutex  \
)

#define KMS_AGNOSTIC_BIN2_GET_COND(obj) (       \
  &KMS_AGNOSTIC_BIN2 (obj)->priv->thread_cond   \
)

#define KMS_AGNOSTIC_BIN2_LOCK(obj) (                   \
  g_mutex_lock (KMS_AGNOSTIC_BIN2_GET_LOCK (obj))       \
)

#define KMS_AGNOSTIC_BIN2_UNLOCK(obj) (                 \
  g_mutex_unlock (KMS_AGNOSTIC_BIN2_GET_LOCK (obj))     \
)

struct _KmsAgnosticBin2Private
{
  GHashTable *tees;
  GQueue *pads_to_link;
  GMutex probe_mutex;
  GCond probe_cond;
  gulong block_probe;

  GMutex thread_mutex;

  GstElement *main_tee;
  GstElement *current_tee;
  GstPad *sink;
  GstCaps *current_caps;
  GstCaps *last_caps;
  guint pad_count;
  gboolean started;

  KmsLoop *loop;
};

/* the capabilities of the inputs and outputs. */
static GstStaticPadTemplate sink_factory = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (KMS_AGNOSTIC_CAPS_CAPS)
    );

static GstStaticPadTemplate src_factory = GST_STATIC_PAD_TEMPLATE ("src_%u",
    GST_PAD_SRC,
    GST_PAD_REQUEST,
    GST_STATIC_CAPS (KMS_AGNOSTIC_CAPS_CAPS)
    );

static gboolean
is_raw_caps (GstCaps * caps)
{
  gboolean ret;
  GstCaps *raw_caps = gst_caps_from_string (KMS_AGNOSTIC_RAW_CAPS);

  ret = gst_caps_is_always_compatible (caps, raw_caps);

  gst_caps_unref (raw_caps);
  return ret;
}

static void
send_force_key_unit_event (GstPad * pad)
{
  GstStructure *s;
  GstEvent *force_key_unit_event;
  GstCaps *caps = gst_pad_get_current_caps (pad);

  if (caps == NULL)
    caps = gst_pad_get_allowed_caps (pad);

  if (caps == NULL)
    return;

  if (is_raw_caps (caps))
    goto end;

  s = gst_structure_new ("GstForceKeyUnit",
      "all-headers", G_TYPE_BOOLEAN, TRUE, NULL);
  force_key_unit_event = gst_event_new_custom (GST_EVENT_CUSTOM_UPSTREAM, s);

  if (GST_PAD_DIRECTION (pad) == GST_PAD_SRC) {
    gst_pad_send_event (pad, force_key_unit_event);
  } else {
    gst_pad_push_event (pad, force_key_unit_event);
  }

end:
  gst_caps_unref (caps);
}

static GstPadProbeReturn
tee_src_probe (GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
  if (~GST_PAD_PROBE_INFO_TYPE (info) & GST_PAD_PROBE_TYPE_BLOCK)
    return GST_PAD_PROBE_OK;

  if (GST_PAD_PROBE_INFO_TYPE (info) & GST_PAD_PROBE_TYPE_EVENT_BOTH) {
    GstEvent *event = gst_pad_probe_info_get_event (info);

    if (GST_EVENT_TYPE (event) == GST_EVENT_RECONFIGURE) {
      // We drop reconfigure events to avoid not negotiated error caused by
      // continious negotiations
      send_force_key_unit_event (pad);
      GST_DEBUG_OBJECT (pad, "Dropping reconfigure event");
      return GST_PAD_PROBE_DROP;
    }
  }

  return GST_PAD_PROBE_PASS;
}

static void
link_queue_to_tee (GstElement * tee, GstElement * queue)
{
  GstPad *tee_src = gst_element_get_request_pad (tee, "src_%u");
  GstPad *queue_sink = gst_element_get_static_pad (queue, "sink");
  GstPadLinkReturn ret;

  gst_pad_add_probe (tee_src,
      GST_PAD_PROBE_TYPE_BLOCK | GST_PAD_PROBE_TYPE_DATA_BOTH, tee_src_probe,
      NULL, NULL);
  ret = gst_pad_link (tee_src, queue_sink);

  if (G_UNLIKELY (GST_PAD_LINK_FAILED (ret)))
    GST_ERROR ("Linking %" GST_PTR_FORMAT " with %" GST_PTR_FORMAT " result %d",
        tee_src, queue_sink, ret);

  g_object_unref (queue_sink);
  g_object_unref (tee_src);
}

static GstElement *
create_convert_for_caps (GstCaps * caps)
{
  GstCaps *audio_caps = gst_static_caps_get (&static_audio_caps);
  GstElement *convert;

  if (gst_caps_can_intersect (caps, audio_caps))
    convert = gst_element_factory_make ("audioconvert", NULL);
  else
    convert = gst_element_factory_make ("videoconvert", NULL);

  gst_caps_unref (audio_caps);

  return convert;
}

static GstElement *
create_rate_for_caps (GstCaps * caps)
{
  GstCaps *audio_caps = gst_static_caps_get (&static_audio_caps);
  GstElement *rate;

  if (gst_caps_can_intersect (caps, audio_caps)) {
    rate = gst_element_factory_make ("audiorate", NULL);
    g_object_set (G_OBJECT (rate), "tolerance", GST_MSECOND * 100,
        "skip-to-first", TRUE, NULL);
  } else {
    rate = gst_element_factory_make ("videorate", NULL);
    g_object_set (G_OBJECT (rate), "average-period", GST_MSECOND * 200,
        "skip-to-first", TRUE, NULL);
  }

  gst_caps_unref (audio_caps);

  return rate;
}

static GstPadProbeReturn
sink_block (GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
  if (~GST_PAD_PROBE_INFO_TYPE (info) & GST_PAD_PROBE_TYPE_BLOCK)
    return GST_PAD_PROBE_OK;

  if (GST_PAD_PROBE_INFO_TYPE (info) & (GST_PAD_PROBE_TYPE_BUFFER |
          GST_PAD_PROBE_TYPE_BUFFER_LIST)) {
    KmsAgnosticBin2 *self = user_data;

    g_mutex_lock (&self->priv->probe_mutex);

    while (self->priv->block_probe) {
      GST_DEBUG_OBJECT (pad, "Holding a buffer");
      g_cond_wait (&self->priv->probe_cond, &self->priv->probe_mutex);
      GST_DEBUG_OBJECT (pad, "Released");
    }

    g_mutex_unlock (&self->priv->probe_mutex);
  }

  return GST_PAD_PROBE_OK;
}

static void
kms_agnostic_bin2_remove_block_probe (KmsAgnosticBin2 * self)
{
  g_mutex_lock (&self->priv->probe_mutex);
  if (self->priv->block_probe != 0L) {
    gst_pad_remove_probe (self->priv->sink, self->priv->block_probe);
    self->priv->block_probe = 0L;
    g_cond_signal (&self->priv->probe_cond);
  }
  g_mutex_unlock (&self->priv->probe_mutex);
}

static void
kms_agnostic_bin2_set_block_probe (KmsAgnosticBin2 * self)
{
  g_mutex_lock (&self->priv->probe_mutex);
  if (self->priv->block_probe == 0L) {
    self->priv->block_probe =
        gst_pad_add_probe (self->priv->sink,
        GST_PAD_PROBE_TYPE_BLOCK_DOWNSTREAM, sink_block, self, NULL);
    GST_DEBUG_OBJECT (self, "Adding probe %ld while connecting",
        self->priv->block_probe);
  }
  g_mutex_unlock (&self->priv->probe_mutex);
}

static GstPadProbeReturn
queue_block (GstPad * pad, GstPadProbeInfo * info, gpointer data)
{
  if (~GST_PAD_PROBE_INFO_TYPE (info) & GST_PAD_PROBE_TYPE_BLOCK)
    return GST_PAD_PROBE_OK;

  // TODO: Start disconnection of this queue and all the  element chain
  // related to it
  return GST_PAD_PROBE_DROP;
}

static void
remove_target_pad (GstPad * pad)
{
  GstPad *target;

  GST_DEBUG_OBJECT (pad, "Removing target pad");

  target = gst_ghost_pad_get_target (GST_GHOST_PAD (pad));

  if (target != NULL) {
    gst_pad_add_probe (target, GST_PAD_PROBE_TYPE_BLOCK_DOWNSTREAM, queue_block,
        NULL, NULL);
    g_object_unref (target);
    gst_ghost_pad_set_target (GST_GHOST_PAD (pad), NULL);
  }
}

static void
kms_agnostic_bin2_link_to_tee (KmsAgnosticBin2 * self, GstPad * pad,
    GstElement * tee, GstCaps * caps)
{
  GstElement *queue = gst_element_factory_make ("queue", NULL);
  GstPad *target;

  gst_bin_add (GST_BIN (self), queue);
  gst_element_sync_state_with_parent (queue);

  if (!gst_caps_is_any (caps) && is_raw_caps (caps)) {
    GstElement *convert = create_convert_for_caps (caps);
    GstElement *rate = create_rate_for_caps (caps);

    gst_bin_add_many (GST_BIN (self), convert, rate, NULL);
    gst_element_sync_state_with_parent (convert);
    gst_element_sync_state_with_parent (rate);
    gst_element_link_many (queue, rate, convert, NULL);

    target = gst_element_get_static_pad (convert, "src");
  } else {
    target = gst_element_get_static_pad (queue, "src");
  }

  gst_ghost_pad_set_target (GST_GHOST_PAD (pad), target);
  g_object_unref (target);

  link_queue_to_tee (tee, queue);
}

static GstElement *
kms_agnostic_bin2_find_tee_for_caps (KmsAgnosticBin2 * self, GstCaps * caps)
{
  GList *tees, *l;
  GstElement *tee = NULL;

  if (gst_caps_is_any (caps)) {
    return self->priv->current_tee;
  }

  tees = g_hash_table_get_values (self->priv->tees);
  for (l = tees; l != NULL && tee == NULL; l = l->next) {
    GstPad *tee_sink = gst_element_get_static_pad (l->data, "sink");
    GstCaps *current_caps = gst_pad_get_current_caps (tee_sink);

    if (current_caps == NULL) {
      current_caps = gst_pad_get_allowed_caps (tee_sink);
      GST_TRACE_OBJECT (l->data, "Allowed caps are: %" GST_PTR_FORMAT,
          current_caps);
    } else {
      GST_TRACE_OBJECT (l->data, "Current caps are: %" GST_PTR_FORMAT,
          current_caps);
    }

    if (current_caps != NULL) {
      if (gst_caps_can_intersect (caps, current_caps))
        tee = l->data;
      gst_caps_unref (current_caps);
    }

    g_object_unref (tee_sink);
  }

  return tee;
}

static GstCaps *
kms_agnostic_bin2_get_raw_caps (GstCaps * caps)
{
  GstCaps *audio_caps, *video_caps, *raw_caps = NULL;

  audio_caps = gst_static_caps_get (&static_audio_caps);
  video_caps = gst_static_caps_get (&static_video_caps);

  if (gst_caps_can_intersect (caps, audio_caps))
    raw_caps = gst_static_caps_get (&static_raw_audio_caps);
  else if (gst_caps_can_intersect (caps, video_caps))
    raw_caps = gst_static_caps_get (&static_raw_video_caps);

  gst_caps_unref (audio_caps);
  gst_caps_unref (video_caps);

  return raw_caps;
}

static GstElement *
create_decoder_for_caps (GstCaps * caps, GstCaps * raw_caps)
{
  GList *decoder_list, *filtered_list, *l;
  GstElementFactory *decoder_factory = NULL;
  GstElement *decoder = NULL;

  decoder_list =
      gst_element_factory_list_get_elements (GST_ELEMENT_FACTORY_TYPE_DECODER,
      GST_RANK_NONE);
  filtered_list =
      gst_element_factory_list_filter (decoder_list, caps, GST_PAD_SINK, FALSE);
  filtered_list =
      gst_element_factory_list_filter (filtered_list, raw_caps, GST_PAD_SRC,
      FALSE);

  for (l = filtered_list; l != NULL && decoder_factory == NULL; l = l->next) {
    decoder_factory = GST_ELEMENT_FACTORY (l->data);
    if (gst_element_factory_get_num_pad_templates (decoder_factory) != 2)
      decoder_factory = NULL;
  }

  if (decoder_factory != NULL) {
    decoder = gst_element_factory_create (decoder_factory, NULL);
  }

  gst_plugin_feature_list_free (filtered_list);
  gst_plugin_feature_list_free (decoder_list);

  return decoder;
}

static GstElement *
create_parser_for_caps (GstCaps * caps)
{
  GList *parser_list, *filtered_list, *l;
  GstElementFactory *parser_factory = NULL;
  GstElement *parser = NULL;

  parser_list =
      gst_element_factory_list_get_elements (GST_ELEMENT_FACTORY_TYPE_PARSER |
      GST_ELEMENT_FACTORY_TYPE_MEDIA_VIDEO, GST_RANK_NONE + 1);
  filtered_list =
      gst_element_factory_list_filter (parser_list, caps, GST_PAD_SINK, FALSE);

  for (l = filtered_list; l != NULL && parser_factory == NULL; l = l->next) {
    parser_factory = GST_ELEMENT_FACTORY (l->data);
    if (gst_element_factory_get_num_pad_templates (parser_factory) != 2)
      parser_factory = NULL;
  }

  if (parser_factory != NULL) {
    parser = gst_element_factory_create (parser_factory, NULL);
  } else {
    parser = gst_element_factory_make ("identity", NULL);

    g_object_set (G_OBJECT (parser), "signal-handoffs", FALSE, NULL);
  }

  gst_plugin_feature_list_free (filtered_list);
  gst_plugin_feature_list_free (parser_list);

  return parser;
}

static GstElement *
kms_agnostic_bin2_create_raw_tee (KmsAgnosticBin2 * self, GstCaps * raw_caps)
{
  GstCaps *current_caps = self->priv->current_caps;

  if (current_caps == NULL)
    return NULL;

  GstElement *decoder, *queue, *tee, *fakequeue, *fakesink;

  decoder = create_decoder_for_caps (current_caps, raw_caps);

  if (decoder == NULL) {
    GST_DEBUG ("Invalid decoder");
    return NULL;
  }

  GST_DEBUG_OBJECT (self, "Decoder found: %" GST_PTR_FORMAT, decoder);

  queue = gst_element_factory_make ("queue", NULL);
  tee = gst_element_factory_make ("tee", NULL);
  fakequeue = gst_element_factory_make ("queue", NULL);
  fakesink = gst_element_factory_make ("fakesink", NULL);

  g_object_set (G_OBJECT (fakesink), "async", FALSE, NULL);

  gst_bin_add_many (GST_BIN (self), queue, decoder, tee, fakequeue, fakesink,
      NULL);
  gst_element_sync_state_with_parent (queue);
  gst_element_sync_state_with_parent (decoder);
  gst_element_sync_state_with_parent (tee);
  gst_element_sync_state_with_parent (fakequeue);
  gst_element_sync_state_with_parent (fakesink);
  gst_element_link_many (queue, decoder, tee, fakequeue, fakesink, NULL);

  link_queue_to_tee (self->priv->current_tee, queue);

  return tee;
}

static GstElement *
kms_agnostic_bin2_get_or_create_raw_tee (KmsAgnosticBin2 * self, GstCaps * caps)
{
  GstCaps *raw_caps = kms_agnostic_bin2_get_raw_caps (caps);

  if (raw_caps != NULL) {
    GstElement *raw_tee;

    GST_DEBUG ("Raw caps: %" GST_PTR_FORMAT, raw_caps);
    raw_tee = kms_agnostic_bin2_find_tee_for_caps (self, raw_caps);

    if (raw_tee == NULL) {
      raw_tee = kms_agnostic_bin2_create_raw_tee (self, raw_caps);
      g_hash_table_insert (self->priv->tees, GST_OBJECT_NAME (raw_tee),
          g_object_ref (raw_tee));
    }

    gst_caps_unref (raw_caps);

    return raw_tee;
  } else {
    GST_ELEMENT_WARNING (self, CORE, NEGOTIATION,
        ("Formats are not compatible"), ("Formats are not compatible"));
    return NULL;
  }
}

static void
configure_encoder (GstElement * encoder, const gchar * factory_name)
{
  GST_DEBUG ("Configure encoder: %s", factory_name);
  if (g_strcmp0 ("vp8enc", factory_name) == 0) {
    g_object_set (G_OBJECT (encoder), "deadline", G_GINT64_CONSTANT (200000),
        "threads", 1, "cpu-used", 16, "resize-allowed", TRUE,
        "target-bitrate", 300000, "end-usage", /* cbr */ 1, NULL);
  } else if (g_strcmp0 ("x264enc", factory_name) == 0) {
    g_object_set (G_OBJECT (encoder), "speed-preset", 1 /* ultrafast */ ,
        "tune", 4 /* zerolatency */ , "threads", (guint) 1,
        NULL);
  }
}

static GstElement *
create_encoder_for_caps (GstCaps * caps)
{
  GList *encoder_list, *filtered_list, *l;
  GstElementFactory *encoder_factory = NULL;
  GstElement *encoder = NULL;

  encoder_list =
      gst_element_factory_list_get_elements (GST_ELEMENT_FACTORY_TYPE_ENCODER,
      GST_RANK_NONE);
  filtered_list =
      gst_element_factory_list_filter (encoder_list, caps, GST_PAD_SRC, FALSE);

  for (l = filtered_list; l != NULL && encoder_factory == NULL; l = l->next) {
    encoder_factory = GST_ELEMENT_FACTORY (l->data);
    if (gst_element_factory_get_num_pad_templates (encoder_factory) != 2)
      encoder_factory = NULL;
  }

  if (encoder_factory != NULL) {
    encoder = gst_element_factory_create (encoder_factory, NULL);
    configure_encoder (encoder, GST_OBJECT_NAME (encoder_factory));
  }

  gst_plugin_feature_list_free (filtered_list);
  gst_plugin_feature_list_free (encoder_list);

  return encoder;
}

static GstElement *
kms_agnostic_bin2_create_tee_for_caps (KmsAgnosticBin2 * self, GstCaps * caps)
{
  GstElement *tee;
  GstElement *raw_tee = kms_agnostic_bin2_get_or_create_raw_tee (self, caps);
  GstElement *encoder, *queue, *rate, *convert, *fakequeue, *fakesink;

  if (raw_tee == NULL)
    return NULL;

  if (is_raw_caps (caps))
    return raw_tee;

  encoder = create_encoder_for_caps (caps);

  if (encoder == NULL)
    return NULL;

  queue = gst_element_factory_make ("queue", NULL);
  rate = create_rate_for_caps (caps);
  convert = create_convert_for_caps (caps);
  tee = gst_element_factory_make ("tee", NULL);
  fakequeue = gst_element_factory_make ("queue", NULL);
  fakesink = gst_element_factory_make ("fakesink", NULL);

  g_object_set (G_OBJECT (fakesink), "async", FALSE, NULL);

  gst_bin_add_many (GST_BIN (self), queue, rate, convert, encoder, tee,
      fakequeue, fakesink, NULL);

  gst_element_sync_state_with_parent (queue);
  gst_element_sync_state_with_parent (rate);
  gst_element_sync_state_with_parent (convert);
  gst_element_sync_state_with_parent (encoder);
  gst_element_sync_state_with_parent (tee);
  gst_element_sync_state_with_parent (fakequeue);
  gst_element_sync_state_with_parent (fakesink);

  gst_element_link_many (queue, rate, convert, encoder, tee, fakequeue,
      fakesink, NULL);
  link_queue_to_tee (raw_tee, queue);

  g_hash_table_insert (self->priv->tees, GST_OBJECT_NAME (tee),
      g_object_ref (tee));

  return tee;
}

/**
 * Link a pad internally
 *
 * @self: The #KmsAgnosticBin2 owner of the pad
 * @pad: (transfer full): The pad to be linked
 * @peer: (transfer full): The peer pad
 */
static void
kms_agnostic_bin2_link_pad (KmsAgnosticBin2 * self, GstPad * pad, GstPad * peer)
{
  GstCaps *caps;
  GstElement *tee;

  GST_INFO_OBJECT (self, "Linking: %" GST_PTR_FORMAT, pad);

  caps = gst_pad_query_caps (peer, NULL);

  if (caps == NULL)
    goto end;

  GST_DEBUG ("Query caps are: %" GST_PTR_FORMAT, caps);
  tee = kms_agnostic_bin2_find_tee_for_caps (self, caps);

  if (tee == NULL) {
    tee = kms_agnostic_bin2_create_tee_for_caps (self, caps);
    GST_DEBUG_OBJECT (self, "Created tee: %" GST_PTR_FORMAT, tee);
  }

  if (tee != NULL)
    kms_agnostic_bin2_link_to_tee (self, pad, tee, caps);

  gst_caps_unref (caps);

end:
  g_object_unref (pad);
  g_object_unref (peer);
}

/**
 * Unlink a pad internally
 *
 * @self: The #KmsAgnosticBin2 owner of the pad
 * @pad: (transfer full): The pad to be unlinked
 */
static void
kms_agnostic_bin2_unlink_pad (KmsAgnosticBin2 * self, GstPad * pad)
{
  GST_DEBUG_OBJECT (self, "Unlinking: %" GST_PTR_FORMAT, pad);

  // TODO: Implement this
  g_object_unref (pad);
}

/**
 * Process a pad for connecting or disconnecting, it should be always called
 * from the loop.
 *
 * @self: The #KmsAgnosticBin2 owner of the pad
 * @pad: (transfer full): The pad to be processed
 */
static void
kms_agnostic_bin2_process_pad (KmsAgnosticBin2 * self, GstPad * pad)
{
  GstPad *peer = NULL;

  GST_DEBUG_OBJECT (self, "Processing pad: %" GST_PTR_FORMAT, pad);

  if (pad == NULL)
    return;

  peer = gst_pad_get_peer (pad);

  if (peer == NULL)
    kms_agnostic_bin2_unlink_pad (self, pad);
  else
    kms_agnostic_bin2_link_pad (self, pad, peer);

}

static gboolean
kms_agnostic_bin2_process_pad_loop (gpointer data)
{
  KmsAgnosticBin2 *self = KMS_AGNOSTIC_BIN2 (data);

  KMS_AGNOSTIC_BIN2_LOCK (self);
  kms_agnostic_bin2_process_pad (self,
      g_queue_pop_head (self->priv->pads_to_link));

  if (g_queue_is_empty (self->priv->pads_to_link)) {
    kms_agnostic_bin2_remove_block_probe (self);
  }
  KMS_AGNOSTIC_BIN2_UNLOCK (self);

  return FALSE;
}

static void
kms_agnostic_bin2_add_pad_to_queue (KmsAgnosticBin2 * self, GstPad * pad)
{
  KMS_AGNOSTIC_BIN2_LOCK (self);
  if (!self->priv->started)
    goto end;

  if (g_queue_index (self->priv->pads_to_link, pad) == -1) {
    GST_DEBUG_OBJECT (pad, "Adding pad to queue");
    kms_agnostic_bin2_set_block_probe (self);

    remove_target_pad (pad);
    g_queue_push_tail (self->priv->pads_to_link, g_object_ref (pad));
    kms_loop_idle_add_full (self->priv->loop, G_PRIORITY_HIGH,
        kms_agnostic_bin2_process_pad_loop, g_object_ref (self),
        g_object_unref);
  }

end:

  KMS_AGNOSTIC_BIN2_UNLOCK (self);
}

static void
iterate_src_pads (KmsAgnosticBin2 * self)
{
  GstIterator *it = gst_element_iterate_src_pads (GST_ELEMENT (self));
  gboolean done = FALSE;
  GstPad *pad;
  GValue item = G_VALUE_INIT;

  while (!done) {
    switch (gst_iterator_next (it, &item)) {
      case GST_ITERATOR_OK:
        pad = g_value_get_object (&item);
        kms_agnostic_bin2_add_pad_to_queue (self, pad);
        g_value_reset (&item);
        break;
      case GST_ITERATOR_RESYNC:
        gst_iterator_resync (it);
        break;
      case GST_ITERATOR_ERROR:
      case GST_ITERATOR_DONE:
        done = TRUE;
        break;
    }
  }

  gst_iterator_free (it);
}

static void
kms_agnostic_bin2_disconnect_previous_input_tee (KmsAgnosticBin2 * self)
{
  GstElement *queue = NULL, *parser = NULL;
  GstPad *tee_sink = NULL, *parser_src = NULL, *parser_sink = NULL,
      *queue_src = NULL, *queue_sink = NULL, *tee_src = NULL;

  if (self->priv->current_tee == NULL)
    return;

  tee_sink = gst_element_get_static_pad (self->priv->current_tee, "sink");
  parser_src = gst_pad_get_peer (tee_sink);
  parser = gst_pad_get_parent_element (parser_src);
  parser_sink = gst_element_get_static_pad (parser, "sink");
  queue_src = gst_pad_get_peer (parser_sink);
  queue = gst_pad_get_parent_element (queue_src);
  queue_sink = gst_element_get_static_pad (queue, "sink");
  tee_src = gst_pad_get_peer (queue_sink);

  gst_pad_unlink (tee_src, queue_sink);
  gst_element_release_request_pad (GST_ELEMENT (GST_OBJECT_PARENT (tee_src)),
      tee_src);

  g_object_unref (tee_src);
  g_object_unref (queue_sink);
  g_object_unref (queue_src);
  g_object_unref (queue);
  g_object_unref (parser_sink);
  g_object_unref (parser_src);
  g_object_unref (parser);
  g_object_unref (tee_sink);
}

static GstPadProbeReturn
set_input_caps (GstPad * pad, GstPadProbeInfo * info, gpointer tee)
{
  KmsAgnosticBin2 *self = KMS_AGNOSTIC_BIN2 (GST_OBJECT_PARENT (tee));
  GstEvent *event = gst_pad_probe_info_get_event (info);
  GstCaps *current_caps;

  GST_TRACE_OBJECT (self, "Event in parser pad: %" GST_PTR_FORMAT, event);

  if (GST_EVENT_TYPE (event) != GST_EVENT_CAPS)
    return GST_PAD_PROBE_OK;

  KMS_AGNOSTIC_BIN2_LOCK (self);

  self->priv->started = TRUE;
  if (self->priv->current_caps != NULL)
    gst_caps_unref (self->priv->current_caps);

  gst_event_parse_caps (event, &current_caps);
  self->priv->current_caps = gst_caps_copy (current_caps);
  g_hash_table_insert (self->priv->tees, GST_OBJECT_NAME (tee),
      g_object_ref (tee));

  GST_INFO_OBJECT (self, "Setting current caps to: %" GST_PTR_FORMAT,
      current_caps);

  KMS_AGNOSTIC_BIN2_UNLOCK (self);
  iterate_src_pads (self);

  return GST_PAD_PROBE_REMOVE;
}

static void
kms_agnostic_bin2_configure_input_tee (KmsAgnosticBin2 * self, GstCaps * caps)
{
  GstElement *input_queue, *parser, *tee, *queue, *fakesink;
  GstPad *parser_src;

  KMS_AGNOSTIC_BIN2_LOCK (self);
  kms_agnostic_bin2_disconnect_previous_input_tee (self);

  input_queue = gst_element_factory_make ("queue", NULL);
  parser = create_parser_for_caps (caps);
  tee = gst_element_factory_make ("tee", NULL);
  self->priv->current_tee = tee;
  queue = gst_element_factory_make ("queue", NULL);
  fakesink = gst_element_factory_make ("fakesink", NULL);

  g_object_set (G_OBJECT (fakesink), "async", FALSE, NULL);

  gst_bin_add_many (GST_BIN (self), input_queue, parser, tee, queue, fakesink,
      NULL);

  g_hash_table_remove_all (self->priv->tees);

  parser_src = gst_element_get_static_pad (parser, "src");
  gst_pad_add_probe (parser_src, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
      set_input_caps, g_object_ref (tee), g_object_unref);
  g_object_unref (parser_src);

  gst_element_sync_state_with_parent (input_queue);
  gst_element_sync_state_with_parent (parser);
  gst_element_sync_state_with_parent (tee);
  gst_element_sync_state_with_parent (queue);
  gst_element_sync_state_with_parent (fakesink);

  gst_element_link_many (self->priv->main_tee, input_queue, parser, tee, queue,
      fakesink, NULL);

  self->priv->started = FALSE;
  KMS_AGNOSTIC_BIN2_UNLOCK (self);
}

static GstPadProbeReturn
kms_agnostic_bin2_sink_caps_probe (GstPad * pad, GstPadProbeInfo * info,
    gpointer user_data)
{
  KmsAgnosticBin2 *self;
  GstCaps *current_caps;
  GstCaps *new_caps = NULL;
  GstEvent *event = gst_pad_probe_info_get_event (info);

  if (GST_EVENT_TYPE (event) != GST_EVENT_CAPS) {
    return GST_PAD_PROBE_OK;
  }

  GST_DEBUG_OBJECT (pad, "Event: %" GST_PTR_FORMAT, event);

  self = KMS_AGNOSTIC_BIN2 (user_data);

  gst_event_parse_caps (event, &new_caps);

  if (new_caps == NULL) {
    GST_ERROR_OBJECT (self, "Unexpected NULL caps");
    return GST_PAD_PROBE_OK;
  }

  KMS_AGNOSTIC_BIN2_LOCK (self);
  current_caps = self->priv->last_caps;
  self->priv->last_caps = gst_caps_copy (new_caps);
  KMS_AGNOSTIC_BIN2_UNLOCK (self);

  GST_DEBUG_OBJECT (user_data, "New caps event: %" GST_PTR_FORMAT, event);

  if (current_caps != NULL) {
    GST_DEBUG_OBJECT (user_data, "Current caps: %" GST_PTR_FORMAT,
        current_caps);

    if (!gst_caps_can_intersect (new_caps, current_caps) &&
        !is_raw_caps (current_caps) && !is_raw_caps (new_caps)) {
      GST_DEBUG_OBJECT (user_data, "Caps differ caps: %" GST_PTR_FORMAT,
          new_caps);
      kms_agnostic_bin2_configure_input_tee (self, new_caps);
    }

    gst_caps_unref (current_caps);
  } else {
    GST_DEBUG_OBJECT (user_data, "No previous caps, starting");
    kms_agnostic_bin2_configure_input_tee (self, new_caps);
  }

  return GST_PAD_PROBE_OK;
}

static GstPadProbeReturn
kms_agnostic_bin2_src_reconfigure_probe (GstPad * pad, GstPadProbeInfo * info,
    gpointer user_data)
{
  GstEvent *event;

  if (~GST_PAD_PROBE_INFO_TYPE (info) & GST_PAD_PROBE_TYPE_BLOCK)
    return GST_PAD_PROBE_OK;

  event = gst_pad_probe_info_get_event (info);

  if (GST_EVENT_TYPE (event) == GST_EVENT_RECONFIGURE) {
    KmsAgnosticBin2 *self = user_data;

    GST_DEBUG_OBJECT (pad, "Received reconfigure event");
    gst_pad_push_event (KMS_AGNOSTIC_BIN2 (GST_OBJECT_PARENT (pad))->priv->sink,
        gst_event_new_reconfigure ());
    kms_agnostic_bin2_add_pad_to_queue (self, pad);
    return GST_PAD_PROBE_DROP;
  }

  return GST_PAD_PROBE_PASS;
}

static void
kms_agnostic_bin2_src_unlinked_just_lock (GstPad * pad, GstObject * parent)
{
  KMS_AGNOSTIC_BIN2_LOCK (parent);
  // We get the lock here an it will be release in the unlinked signal watcher
  // We need to do this becausse we cannot get the target pad in this function
  // because the object lock of pad is locked
}

static void
kms_agnostic_bin2_src_unlinked (GstPad * pad, GstPad * peer,
    KmsAgnosticBin2 * self)
{
  GST_DEBUG_OBJECT (pad, "Unlinked");

  remove_target_pad (pad);

  // We get the lock here an it will be release in the unlinked signal watcher
  KMS_AGNOSTIC_BIN2_UNLOCK (self);
  GST_DEBUG_OBJECT (pad, "Unlinked OK");
}

static GstPad *
kms_agnostic_bin2_request_new_pad (GstElement * element,
    GstPadTemplate * templ, const gchar * name, const GstCaps * caps)
{
  GstPad *pad;
  gchar *pad_name;
  KmsAgnosticBin2 *self = KMS_AGNOSTIC_BIN2 (element);

  GST_OBJECT_LOCK (self);
  pad_name = g_strdup_printf ("src_%d", self->priv->pad_count++);
  GST_OBJECT_UNLOCK (self);

  pad = gst_ghost_pad_new_no_target_from_template (pad_name, templ);
  g_free (pad_name);

  gst_pad_add_probe (pad, GST_PAD_PROBE_TYPE_BLOCK |
      GST_PAD_PROBE_TYPE_EVENT_UPSTREAM,
      kms_agnostic_bin2_src_reconfigure_probe, element, NULL);

  g_signal_connect (pad, "unlinked",
      G_CALLBACK (kms_agnostic_bin2_src_unlinked), self);
  gst_pad_set_unlink_function (pad, kms_agnostic_bin2_src_unlinked_just_lock);

  gst_pad_set_active (pad, TRUE);

  if (gst_element_add_pad (element, pad))
    return pad;

  g_object_unref (pad);

  return NULL;
}

static void
kms_agnostic_bin2_release_pad (GstElement * element, GstPad * pad)
{
  gst_element_remove_pad (element, pad);
}

static void
kms_agnostic_bin2_dispose (GObject * object)
{
  KmsAgnosticBin2 *self = KMS_AGNOSTIC_BIN2 (object);

  KMS_AGNOSTIC_BIN2_LOCK (self);
  g_clear_object (&self->priv->loop);

  if (self->priv->current_caps) {
    gst_caps_unref (self->priv->current_caps);
    self->priv->current_caps = NULL;
  }

  if (self->priv->last_caps) {
    gst_caps_unref (self->priv->last_caps);
    self->priv->last_caps = NULL;
  }

  KMS_AGNOSTIC_BIN2_UNLOCK (self);

  /* chain up */
  G_OBJECT_CLASS (kms_agnostic_bin2_parent_class)->dispose (object);
}

static void
kms_agnostic_bin2_finalize (GObject * object)
{
  KmsAgnosticBin2 *self = KMS_AGNOSTIC_BIN2 (object);

  g_mutex_clear (&self->priv->thread_mutex);

  g_queue_free_full (self->priv->pads_to_link, g_object_unref);
  g_hash_table_unref (self->priv->tees);

  kms_agnostic_bin2_remove_block_probe (self);

  g_cond_clear (&self->priv->probe_cond);
  g_mutex_clear (&self->priv->probe_mutex);

  /* chain up */
  G_OBJECT_CLASS (kms_agnostic_bin2_parent_class)->finalize (object);
}

static void
kms_agnostic_bin2_class_init (KmsAgnosticBin2Class * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *gstelement_class;

  gobject_class = G_OBJECT_CLASS (klass);
  gstelement_class = GST_ELEMENT_CLASS (klass);

  gobject_class->dispose = kms_agnostic_bin2_dispose;
  gobject_class->finalize = kms_agnostic_bin2_finalize;

  gst_element_class_set_details_simple (gstelement_class,
      "Agnostic connector 2nd version",
      "Generic/Bin/Connector",
      "Automatically encodes/decodes media to match sink and source pads caps",
      "José Antonio Santos Cadenas <santoscadenas@kurento.com>");

  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&src_factory));
  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&sink_factory));

  gstelement_class->request_new_pad =
      GST_DEBUG_FUNCPTR (kms_agnostic_bin2_request_new_pad);
  gstelement_class->release_pad =
      GST_DEBUG_FUNCPTR (kms_agnostic_bin2_release_pad);

  GST_DEBUG_CATEGORY_INIT (GST_CAT_DEFAULT, PLUGIN_NAME, 0, PLUGIN_NAME);

  g_type_class_add_private (klass, sizeof (KmsAgnosticBin2Private));
}

static GstPadProbeReturn
gap_detection_probe (GstPad * pad, GstPadProbeInfo * info, gpointer data)
{
  KmsAgnosticBin2 *self = KMS_AGNOSTIC_BIN2 (data);
  GstEvent *event = GST_PAD_PROBE_INFO_EVENT (info);

  if (GST_EVENT_TYPE (event) == GST_EVENT_GAP) {
    GST_INFO_OBJECT (self, "Gap detected, request key frame");
    send_force_key_unit_event (pad);
  }

  return GST_PAD_PROBE_OK;
}

static void
kms_agnostic_bin2_init (KmsAgnosticBin2 * self)
{
  GstPadTemplate *templ;
  GstElement *tee, *queue, *fakesink;
  GstPad *target;

  self->priv = KMS_AGNOSTIC_BIN2_GET_PRIVATE (self);
  self->priv->pad_count = 0;

  self->priv->current_tee = NULL;

  tee = gst_element_factory_make ("tee", NULL);
  self->priv->main_tee = tee;
  queue = gst_element_factory_make ("queue", NULL);
  fakesink = gst_element_factory_make ("fakesink", NULL);

  g_object_set (G_OBJECT (fakesink), "async", FALSE, NULL);

  gst_bin_add_many (GST_BIN (self), tee, queue, fakesink, NULL);
  gst_element_link_many (tee, queue, fakesink, NULL);

  target = gst_element_get_static_pad (tee, "sink");
  templ = gst_static_pad_template_get (&sink_factory);
  self->priv->sink = gst_ghost_pad_new_from_template ("sink", target, templ);
  self->priv->current_caps = NULL;
  self->priv->last_caps = NULL;
  gst_pad_add_probe (self->priv->sink, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
      gap_detection_probe, self, NULL);
  g_object_unref (templ);
  g_object_unref (target);

  gst_pad_add_probe (self->priv->sink, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
      kms_agnostic_bin2_sink_caps_probe, self, NULL);

  gst_element_add_pad (GST_ELEMENT (self), self->priv->sink);

  g_object_set (G_OBJECT (self), "async-handling", TRUE, NULL);

  self->priv->started = FALSE;

  self->priv->loop = kms_loop_new ();

  g_cond_init (&self->priv->probe_cond);
  g_mutex_init (&self->priv->probe_mutex);
  self->priv->block_probe = 0L;

  self->priv->tees =
      g_hash_table_new_full (g_str_hash, g_str_equal, NULL, g_object_unref);

  self->priv->pads_to_link = g_queue_new ();
  g_mutex_init (&self->priv->thread_mutex);
}

gboolean
kms_agnostic_bin2_plugin_init (GstPlugin * plugin)
{
  return gst_element_register (plugin, PLUGIN_NAME, GST_RANK_NONE,
      KMS_TYPE_AGNOSTIC_BIN2);
}
