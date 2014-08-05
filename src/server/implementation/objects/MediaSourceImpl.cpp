#include <gst/gst.h>
#include "MediaSinkImpl.hpp"
#include "MediaSourceImpl.hpp"
#include <jsonrpc/JsonSerializer.hpp>
#include <KurentoException.hpp>
#include <MediaType.hpp>
#include <gst/gst.h>

#define GST_CAT_DEFAULT kurento_media_source_impl
GST_DEBUG_CATEGORY_STATIC (GST_CAT_DEFAULT);
#define GST_DEFAULT_NAME "KurentoMediaSourceImpl"

namespace kurento
{

struct tmp_data {
  GMutex mutex;
  std::weak_ptr<MediaSourceImpl> src;
  std::weak_ptr<MediaSinkImpl> sink;
  gulong handler;
};

static void
destroy_tmp_data (gpointer data, GClosure *closure)
{
  struct tmp_data *tmp = (struct tmp_data *) data;

  tmp->src.reset();
  tmp->sink.reset();
  g_mutex_clear (&tmp->mutex);
  g_slice_free (struct tmp_data, tmp);
}

static struct tmp_data *
create_tmp_data (std::shared_ptr<MediaSourceImpl> src,
                 std::shared_ptr<MediaSinkImpl> sink)
{
  struct tmp_data *tmp;

  tmp = g_slice_new0 (struct tmp_data);

  g_mutex_init (&tmp->mutex);

  tmp->src = std::weak_ptr<MediaSourceImpl> (src);
  tmp->sink = std::weak_ptr<MediaSinkImpl> (sink);
  tmp->handler = 0L;

  return tmp;
}

static void
pad_unlinked (GstPad *pad, GstPad *peer, GstElement *parent)
{
  gst_element_release_request_pad (parent, pad);
}

gboolean
link_media_elements (std::shared_ptr<MediaSourceImpl> src,
                     std::shared_ptr<MediaSinkImpl> sink)
{
  std::unique_lock<std::recursive_mutex> lock (src->mutex);
  bool ret = FALSE;
  GstPad *pad;

  pad = gst_element_get_request_pad (src->getGstreamerElement(),
                                     src->getPadName() );

  if (pad == NULL) {
    return FALSE;
  }

  GST_WARNING ("Connecting pad %s", src->getPadName() );

  g_signal_connect_data (G_OBJECT (pad), "unlinked", G_CALLBACK (pad_unlinked),
                         g_object_ref (src->getGstreamerElement() ),
                         (GClosureNotify) g_object_unref, (GConnectFlags) 0);

  if (sink->linkPad (src, pad) ) {
    src->connectedSinks.push_back (std::weak_ptr<MediaSinkImpl> (sink) );
    ret = TRUE;
  } else {
    gst_element_release_request_pad (src->getGstreamerElement(), pad);
    ret = FALSE;
  }

  gst_object_unref (pad);

  return ret;
}

static void
disconnect_handler (GstElement *element, struct tmp_data *data)
{
  g_mutex_lock (&data->mutex);

  if (data->handler != 0) {
    g_signal_handler_disconnect (element, data->handler);
    data->handler = 0;
  }

  g_mutex_unlock (&data->mutex);
}

static void
agnosticbin_added_cb (GstElement *element, gpointer data)
{
  struct tmp_data *tmp = (struct tmp_data *) data;
  std::shared_ptr<MediaSourceImpl> src;
  std::shared_ptr<MediaSinkImpl> sink;

  try {
    src = tmp->src.lock();
    sink = tmp->sink.lock();

    if (src && sink && link_media_elements (src, sink) ) {
      disconnect_handler (element, tmp);
    }
  } catch (const std::bad_weak_ptr &e) {
    GST_WARNING ("Removed before connecting");

    disconnect_handler (element, tmp);
  }
}

MediaSourceImpl::MediaSourceImpl (std::shared_ptr<MediaType> mediaType,
                                  const std::string &mediaDescription,
                                  std::shared_ptr<MediaObjectImpl> parent) :
  MediaPadImpl (parent, mediaType, mediaDescription)
{
}

MediaSourceImpl::~MediaSourceImpl()
{
  std::unique_lock<std::recursive_mutex> lock (mutex);

  for (auto it = connectedSinks.begin(); it != connectedSinks.end(); it++) {
    try {
      std::shared_ptr<MediaSinkImpl> connectedSinkLocked;

      GST_INFO ("connectedSink");
      connectedSinkLocked = it->lock();

      if (connectedSinkLocked != NULL) {
        connectedSinkLocked->unlinkUnchecked (NULL);
      }
    } catch (const std::bad_weak_ptr &e) {
      GST_WARNING ("Got invalid reference while releasing MediaSrc %s",
                   getId().c_str() );
    }
  }
}

const gchar *
MediaSourceImpl::getPadName ()
{
  if ( ( (MediaPadImpl *) this)->getMediaType()->getValue() == MediaType::AUDIO) {
    return (const gchar *) "audio_src_%u";
  } else {
    return (const gchar *) "video_src_%u";
  }
}

void MediaSourceImpl::connect (std::shared_ptr<MediaSink> sink)
{
  std::unique_lock<std::recursive_mutex> lock (mutex);
  std::shared_ptr<MediaSinkImpl> mediaSinkImpl =
    std::dynamic_pointer_cast<MediaSinkImpl> (sink);
  GstPad *pad;
  bool ret;

  GST_INFO ("connect %s to %s", this->getId().c_str(),
            mediaSinkImpl->getId().c_str() );

  pad = gst_element_get_request_pad (getGstreamerElement(), getPadName() );

  if (pad == NULL) {
    struct tmp_data *tmp;

    GST_DEBUG ("Put connection off until agnostic bin is created for pad %s",
               getPadName() );
    tmp = create_tmp_data (std::dynamic_pointer_cast<MediaSourceImpl>
                           (shared_from_this() ), mediaSinkImpl);
    tmp->handler = g_signal_connect_data (getGstreamerElement(),
                                          "agnosticbin-added",
                                          G_CALLBACK (agnosticbin_added_cb),
                                          tmp, destroy_tmp_data,
                                          (GConnectFlags) 0);
    return;
  }

  g_signal_connect_data (G_OBJECT (pad), "unlinked", G_CALLBACK (pad_unlinked),
                         g_object_ref (getGstreamerElement() ),
                         (GClosureNotify) g_object_unref, (GConnectFlags) 0);

  ret = mediaSinkImpl->linkPad (
          std::dynamic_pointer_cast <MediaSourceImpl> (shared_from_this() ), pad);

  if (ret) {
    connectedSinks.push_back (std::weak_ptr<MediaSinkImpl> (mediaSinkImpl) );
  } else {
    gst_element_release_request_pad (getGstreamerElement(), pad);
  }

  g_object_unref (pad);

  if (!ret) {
    throw KurentoException (CONNECT_ERROR, "Cannot link pads");
  }
}

void
MediaSourceImpl::removeSink (MediaSinkImpl *mediaSink)
{
  std::unique_lock<std::recursive_mutex> lock (mutex);
  std::shared_ptr<MediaSinkImpl> sinkLocked;
  std::vector< std::weak_ptr<MediaSinkImpl> >::iterator it;

  it = connectedSinks.begin();

  while (it != connectedSinks.end() ) {
    try {
      sinkLocked = (*it).lock();
    } catch (const std::bad_weak_ptr &e) {
    }

    if (sinkLocked == NULL || sinkLocked->getId() == mediaSink->getId() ) {
      it = connectedSinks.erase (it);
    } else {
      it++;
    }
  }
}

void
MediaSourceImpl::disconnect (MediaSinkImpl *mediaSink)
{
  std::unique_lock<std::recursive_mutex> lock (mutex);

  GST_INFO ("disconnect %s from %s", this->getId().c_str(),
            mediaSink->getId().c_str() );

  mediaSink->unlink (std::dynamic_pointer_cast<MediaSourceImpl>
                     (shared_from_this() ), NULL);
}

std::vector < std::shared_ptr<MediaSink> >
MediaSourceImpl::getConnectedSinks ()
{
  std::unique_lock<std::recursive_mutex> lock (mutex);
  std::vector < std::shared_ptr<MediaSink> > sinks;

  std::shared_ptr<MediaSinkImpl> sinkLocked;
  std::vector< std::weak_ptr<MediaSinkImpl> >::iterator it;

  for ( it = connectedSinks.begin() ; it != connectedSinks.end(); ++it) {
    try {
      sinkLocked = (*it).lock();
    } catch (const std::bad_weak_ptr &e) {
    }

    if (sinkLocked != NULL) {
      sinks.push_back (std::dynamic_pointer_cast<MediaSink> (sinkLocked) );
    }
  }

  return sinks;
}

MediaSourceImpl::StaticConstructor MediaSourceImpl::staticConstructor;

MediaSourceImpl::StaticConstructor::StaticConstructor()
{
  GST_DEBUG_CATEGORY_INIT (GST_CAT_DEFAULT, GST_DEFAULT_NAME, 0,
                           GST_DEFAULT_NAME);
}

} /* kurento */
