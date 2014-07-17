#include <gst/gst.h>
#include "MediaType.hpp"
#include "MediaSourceImpl.hpp"
#include "MediaSinkImpl.hpp"
#include "MediaElementImpl.hpp"
#include <jsonrpc/JsonSerializer.hpp>
#include <KurentoException.hpp>
#include <MediaPipelineImpl.hpp>
#include <MediaSet.hpp>
#include <gst/gst.h>

#define GST_CAT_DEFAULT kurento_media_element_impl
GST_DEBUG_CATEGORY_STATIC (GST_CAT_DEFAULT);
#define GST_DEFAULT_NAME "KurentoMediaElementImpl"

namespace kurento
{

MediaElementImpl::MediaElementImpl (std::shared_ptr<MediaObjectImpl> parent, const std::string &factoryName) : MediaObjectImpl (parent)
{
  std::shared_ptr<MediaPipelineImpl> pipe;

  pipe = std::dynamic_pointer_cast<MediaPipelineImpl> (getMediaPipeline() );

  element = gst_element_factory_make (factoryName.c_str(), NULL);

  if (element == NULL) {
    throw KurentoException (MEDIA_OBJECT_NOT_AVAILABLE,
                            "Cannot create gstreamer element: " + factoryName);
  }

  g_object_ref (element);
  gst_bin_add (GST_BIN ( pipe->getPipeline() ), element);
  gst_element_sync_state_with_parent (element);
}

MediaElementImpl::~MediaElementImpl ()
{
  std::shared_ptr<MediaPipelineImpl> pipe;

  pipe = std::dynamic_pointer_cast<MediaPipelineImpl> (getMediaPipeline() );

  gst_element_set_locked_state (element, TRUE);
  gst_element_set_state (element, GST_STATE_NULL);
  gst_bin_remove (GST_BIN ( pipe->getPipeline() ), element);
  g_object_unref (element);
}

std::vector<std::shared_ptr<MediaSource>> MediaElementImpl::getMediaSrcs ()
{
  std::vector<std::shared_ptr<MediaSource>> srcs;

  srcs.push_back (getOrCreateAudioMediaSrc() );
  srcs.push_back (getOrCreateVideoMediaSrc() );

  return srcs;
}

std::vector<std::shared_ptr<MediaSource>> MediaElementImpl::getMediaSrcs (std::shared_ptr<MediaType> mediaType)
{
  std::vector<std::shared_ptr<MediaSource>> srcs;

  if (mediaType->getValue() == MediaType::AUDIO) {
    srcs.push_back (getOrCreateAudioMediaSrc() );
  } else if (mediaType->getValue() == MediaType::VIDEO) {
    srcs.push_back (getOrCreateVideoMediaSrc() );
  }

  return srcs;
}

std::vector<std::shared_ptr<MediaSource>> MediaElementImpl::getMediaSrcs (std::shared_ptr<MediaType> mediaType, const std::string &description)
{
  if (description == "")  {
    return getMediaSrcs (mediaType);
  } else {
    std::vector<std::shared_ptr<MediaSource>> srcs;

    return srcs;
  }
}

std::vector<std::shared_ptr<MediaSink>> MediaElementImpl::getMediaSinks ()
{
  std::vector<std::shared_ptr<MediaSink>> sinks;

  sinks.push_back (getOrCreateAudioMediaSink() );
  sinks.push_back (getOrCreateVideoMediaSink() );

  return sinks;
}

std::vector<std::shared_ptr<MediaSink>> MediaElementImpl::getMediaSinks (std::shared_ptr<MediaType> mediaType)
{
  std::vector<std::shared_ptr<MediaSink>> sinks;

  if (mediaType->getValue() == MediaType::AUDIO) {
    sinks.push_back (getOrCreateAudioMediaSink() );
  } else if (mediaType->getValue() == MediaType::VIDEO) {
    sinks.push_back (getOrCreateVideoMediaSink() );
  }

  return sinks;
}

std::vector<std::shared_ptr<MediaSink>> MediaElementImpl::getMediaSinks (std::shared_ptr<MediaType> mediaType, const std::string &description)
{
  if (description == "")  {
    return getMediaSinks (mediaType);
  } else {
    std::vector<std::shared_ptr<MediaSink>> sinks;

    return sinks;
  }
}

void MediaElementImpl::connect (std::shared_ptr<MediaElement> sink)
{
  std::shared_ptr<MediaElementImpl> sinkImpl =
    std::dynamic_pointer_cast<MediaElementImpl> (sink);

  std::shared_ptr<MediaSource> audio_src = getOrCreateAudioMediaSrc();
  std::shared_ptr<MediaSink> audio_sink = sinkImpl->getOrCreateAudioMediaSink();

  std::shared_ptr<MediaSource> video_src = getOrCreateVideoMediaSrc();
  std::shared_ptr<MediaSink> video_sink = sinkImpl->getOrCreateVideoMediaSink();

  audio_src->connect (audio_sink);

  try {
    video_src->connect (video_sink);
  } catch (...) {
    try {
      audio_sink->disconnect (audio_src);
    } catch (...) {
    }

    throw;
  }
}

void MediaElementImpl::connect (std::shared_ptr<MediaElement> sink, std::shared_ptr<MediaType> mediaType)
{
  std::shared_ptr<MediaElementImpl> sinkImpl =
    std::dynamic_pointer_cast<MediaElementImpl> (sink);

  if (mediaType->getValue() == MediaType::AUDIO) {
    std::shared_ptr<MediaSource> audio_src = getOrCreateAudioMediaSrc();
    std::shared_ptr<MediaSink> audio_sink = sinkImpl->getOrCreateAudioMediaSink();

    audio_src->connect (audio_sink);
  } else if (mediaType->getValue() == MediaType::VIDEO) {
    std::shared_ptr<MediaSource> video_src = getOrCreateVideoMediaSrc();
    std::shared_ptr<MediaSink> video_sink = sinkImpl->getOrCreateVideoMediaSink();

    video_src->connect (video_sink);
  }
}

void MediaElementImpl::connect (std::shared_ptr<MediaElement> sink, std::shared_ptr<MediaType> mediaType, const std::string &mediaDescription)
{
  if (mediaDescription == "") {
    connect (sink, mediaType);
  }
}

/*Internal utilities methods*/

std::shared_ptr<MediaSourceImpl>
MediaElementImpl::getOrCreateAudioMediaSrc()
{
  mutex.lock();

  std::shared_ptr<MediaSourceImpl> locked;

  try {
    locked = audioMediaSrc.lock();
  } catch (const std::bad_weak_ptr &e) {
  }

  if (locked.get() == NULL) {
    std::shared_ptr<MediaType> mediaType (new MediaType (MediaType::AUDIO) );

    MediaSourceImpl *source = new  MediaSourceImpl (mediaType, "",
        std::dynamic_pointer_cast <MediaElementImpl> (shared_from_this() ) );

    locked = std::dynamic_pointer_cast<MediaSourceImpl>
             (MediaSet::getMediaSet().ref (source) );

    audioMediaSrc = std::weak_ptr<MediaSourceImpl> (locked);
  }

  mutex.unlock();

  return locked;
}

std::shared_ptr<MediaSourceImpl>
MediaElementImpl::getOrCreateVideoMediaSrc()
{
  mutex.lock();

  std::shared_ptr<MediaSourceImpl> locked;

  try {
    locked = videoMediaSrc.lock();
  } catch (const std::bad_weak_ptr &e) {
  }

  if (locked.get() == NULL) {
    std::shared_ptr<MediaType> mediaType (new MediaType (MediaType::VIDEO) );

    MediaSourceImpl *source = new  MediaSourceImpl (mediaType, "",
        std::dynamic_pointer_cast <MediaElementImpl> (shared_from_this() ) );

    locked = std::dynamic_pointer_cast<MediaSourceImpl>
             (MediaSet::getMediaSet().ref (source) );

    videoMediaSrc = std::weak_ptr<MediaSourceImpl> (locked);
  }

  mutex.unlock();

  return locked;
}

std::shared_ptr<MediaSinkImpl>
MediaElementImpl::getOrCreateAudioMediaSink()
{
  mutex.lock();

  std::shared_ptr<MediaSinkImpl> locked;

  try {
    locked = audioMediaSink.lock();
  } catch (const std::bad_weak_ptr &e) {
  }

  if (locked.get() == NULL) {
    std::shared_ptr<MediaType> mediaType (new MediaType (MediaType::AUDIO) );

    MediaSinkImpl *sink = new  MediaSinkImpl (mediaType, "",
        std::dynamic_pointer_cast <MediaElementImpl> (shared_from_this() ) );

    locked = std::dynamic_pointer_cast<MediaSinkImpl>
             (MediaSet::getMediaSet().ref (sink) );

    audioMediaSink = std::weak_ptr<MediaSinkImpl> (locked);
  }

  mutex.unlock();

  return locked;
}

std::shared_ptr<MediaSinkImpl>
MediaElementImpl::getOrCreateVideoMediaSink()
{
  mutex.lock();

  std::shared_ptr<MediaSinkImpl> locked;

  try {
    locked = videoMediaSink.lock();
  } catch (const std::bad_weak_ptr &e) {
  }

  if (locked.get() == NULL) {
    std::shared_ptr<MediaType> mediaType (new MediaType (MediaType::VIDEO) );

    MediaSinkImpl *sink = new  MediaSinkImpl (mediaType, "",
        std::dynamic_pointer_cast <MediaElementImpl> (shared_from_this() ) );

    locked = std::dynamic_pointer_cast<MediaSinkImpl>
             (MediaSet::getMediaSet().ref (sink) );

    videoMediaSink = std::weak_ptr<MediaSinkImpl> (locked);
  }

  mutex.unlock();

  return locked;
}

MediaElementImpl::StaticConstructor MediaElementImpl::staticConstructor;

MediaElementImpl::StaticConstructor::StaticConstructor()
{
  GST_DEBUG_CATEGORY_INIT (GST_CAT_DEFAULT, GST_DEFAULT_NAME, 0,
                           GST_DEFAULT_NAME);
}

} /* kurento */
