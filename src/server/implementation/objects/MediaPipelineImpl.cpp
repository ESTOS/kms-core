/*
 * (C) Copyright 2016 Kurento (http://kurento.org/)
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */
#include <gst/gst.h>
#include <MediaPipelineImplFactory.hpp>
#include "MediaPipelineImpl.hpp"
#include <jsonrpc/JsonSerializer.hpp>
#include <KurentoException.hpp>
#include <gst/gst.h>
#include <DotGraph.hpp>
#include <GstreamerDotDetails.hpp>
#include <SignalHandler.hpp>
#include "kmselement.h"

#define GST_CAT_DEFAULT kurento_media_pipeline_impl
GST_DEBUG_CATEGORY_STATIC (GST_CAT_DEFAULT);
#define GST_DEFAULT_NAME "KurentoMediaPipelineImpl"

namespace kurento
{
void
MediaPipelineImpl::busMessage (GstMessage *message)
{
  switch (message->type) {
  case GST_MESSAGE_ERROR: {
    GError *err = NULL;
    gchar *debug = NULL;

    GST_ERROR ("Error on bus: %" GST_PTR_FORMAT, message);
    gst_debug_bin_to_dot_file_with_ts (GST_BIN (pipeline),
                                       GST_DEBUG_GRAPH_SHOW_ALL, "error");
    gst_message_parse_error (message, &err, &debug);
    std::string errorMessage;

    if (err) {
      errorMessage = std::string (err->message);
    }

    if (debug != NULL) {
      errorMessage += " -> " + std::string (debug);
    }

    try {
      gint code = 0;

      if (err) {
        code = err->code;
      }

      Error error (shared_from_this(), errorMessage, code,
                   "UNEXPECTED_PIPELINE_ERROR");

      signalError (error);
    } catch (std::bad_weak_ptr &e) {
    }

    g_error_free (err);
    g_free (debug);
    break;
  }

  default:
    break;
  }
}

void MediaPipelineImpl::postConstructor ()
{
  GstBus *bus;

  MediaObjectImpl::postConstructor ();

  bus = gst_pipeline_get_bus (GST_PIPELINE (pipeline) );
  gst_bus_add_signal_watch (bus);
  busMessageHandler = register_signal_handler (G_OBJECT (bus), "message",
                      std::function <void (GstBus *, GstMessage *) > (std::bind (
                            &MediaPipelineImpl::busMessage, this,
                            std::placeholders::_2) ),
                      std::dynamic_pointer_cast<MediaPipelineImpl>
                      (shared_from_this() ) );
  g_object_unref (bus);
}

MediaPipelineImpl::MediaPipelineImpl (const boost::property_tree::ptree &config)
  : MediaObjectImpl (config)
{
  GstClock *clock;

  pipeline = gst_pipeline_new (NULL);

  if (pipeline == NULL) {
    throw KurentoException (MEDIA_OBJECT_NOT_AVAILABLE,
                            "Cannot create gstreamer pipeline");
  }

  clock = gst_system_clock_obtain ();
  gst_pipeline_use_clock (GST_PIPELINE (pipeline), clock);
  g_object_unref (clock);

  gst_element_set_state (pipeline, GST_STATE_PLAYING);

  busMessageHandler = 0;

  rtp_socket_reuse_audio = NULL;
  rtcp_socket_reuse_audio = NULL;
  rtp_socket_reuse_video = NULL;
  rtcp_socket_reuse_video = NULL;
}

MediaPipelineImpl::~MediaPipelineImpl ()
{
  GstBus *bus = gst_pipeline_get_bus (GST_PIPELINE (pipeline) );

  gst_element_set_state (pipeline, GST_STATE_NULL);

  if (rtp_socket_reuse_audio != NULL) { //ru-bu
    g_object_set (pipeline, "rtp-socket-close", rtp_socket_reuse_audio, NULL);
    rtp_socket_reuse_audio = NULL;
  }

  if (rtcp_socket_reuse_audio != NULL) {
    g_object_set (pipeline, "rtp-socket-close", rtcp_socket_reuse_audio, NULL);
    rtcp_socket_reuse_audio = NULL;
  }

  if (rtp_socket_reuse_video != NULL) { //ru-bu
    g_object_set (pipeline, "rtp-socket-close", rtp_socket_reuse_video, NULL);
    rtp_socket_reuse_video = NULL;
  }

  if (rtcp_socket_reuse_video != NULL) {
    g_object_set (pipeline, "rtp-socket-close", rtcp_socket_reuse_video, NULL);
    rtcp_socket_reuse_video = NULL;
  }

  if (busMessageHandler > 0) {
    unregister_signal_handler (bus, busMessageHandler);
  }

  gst_bus_remove_signal_watch (bus);
  g_object_unref (bus);
  g_object_unref (pipeline);
}

std::string MediaPipelineImpl::getGstreamerDot (
  std::shared_ptr<GstreamerDotDetails> details)
{
  return generateDotGraph (GST_BIN (pipeline), details);
}

std::string MediaPipelineImpl::getGstreamerDot()
{
  return generateDotGraph (GST_BIN (pipeline),
                           std::shared_ptr <GstreamerDotDetails> (new GstreamerDotDetails (
                                 GstreamerDotDetails::SHOW_VERBOSE) ) );
}

bool
MediaPipelineImpl::getLatencyStats ()
{
  std::unique_lock <std::recursive_mutex> lock (recMutex);
  return latencyStats;
}

void
MediaPipelineImpl::setLatencyStats (bool latencyStats)
{
  GstIterator *it;
  gboolean done = FALSE;
  GValue item = G_VALUE_INIT;
  std::unique_lock <std::recursive_mutex> lock (recMutex);

  if (this->latencyStats == latencyStats) {
    return;
  }

  this->latencyStats = latencyStats;
  it = gst_bin_iterate_elements (GST_BIN (pipeline) );

  while (!done) {
    switch (gst_iterator_next (it, &item) ) {
    case GST_ITERATOR_OK: {
      GstElement *element = GST_ELEMENT (g_value_get_object (&item) );

      if (KMS_IS_ELEMENT (element) ) {
        g_object_set (element, "media-stats", latencyStats, NULL);
      }

      g_value_reset (&item);
      break;
    }

    case GST_ITERATOR_RESYNC:
      gst_iterator_resync (it);
      break;

    case GST_ITERATOR_ERROR:
    case GST_ITERATOR_DONE:
      done = TRUE;
      break;
    }
  }

  g_value_unset (&item);
  gst_iterator_free (it);
}

bool
MediaPipelineImpl::addElement (GstElement *element)
{
  std::unique_lock <std::recursive_mutex> lock (recMutex);
  bool ret;

  if (KMS_IS_ELEMENT (element) ) {
    g_object_set (element, "media-stats", latencyStats, NULL);
  }

  ret = gst_bin_add (GST_BIN (pipeline), element);

  if (ret) {
    gst_element_sync_state_with_parent (element);
  }

  return ret;
}

void
MediaPipelineImpl::getSockets (GSocket **rtp_socket_audio,
                               GSocket **rtcp_socket_audio, GSocket **rtp_socket_video,
                               GSocket **rtcp_socket_video)
{
  if (rtp_socket_audio) {
    *rtp_socket_audio = rtp_socket_reuse_audio;
  }

  if (rtcp_socket_audio) {
    *rtcp_socket_audio = rtcp_socket_reuse_audio;
  }

  if (rtp_socket_video) {
    *rtp_socket_video = rtp_socket_reuse_video;
  }

  if (rtcp_socket_video) {
    *rtcp_socket_video = rtcp_socket_reuse_video;
  }
}

void
MediaPipelineImpl::setSockets (GSocket *rtp_socket_audio,
                               GSocket *rtcp_socket_audio, GSocket *rtp_socket_video,
                               GSocket *rtcp_socket_video)
{
  if ( (rtp_socket_reuse_audio != NULL)
       && (rtp_socket_reuse_audio != rtp_socket_audio) ) {
    g_object_set (pipeline, "rtp-socket-close", rtp_socket_reuse_audio, NULL);
  }

  rtp_socket_reuse_audio = rtp_socket_audio;

  if ( (rtcp_socket_reuse_audio != NULL)
       && (rtcp_socket_reuse_audio != rtcp_socket_audio) ) {
    g_object_set (pipeline, "rtp-socket-close", rtcp_socket_reuse_audio, NULL);
  }

  rtcp_socket_reuse_audio = rtcp_socket_audio;

  if ( (rtp_socket_reuse_video != NULL)
       && (rtp_socket_reuse_video != rtp_socket_video) ) {
    g_object_set (pipeline, "rtp-socket-close", rtp_socket_reuse_video, NULL);
  }

  rtp_socket_reuse_video = rtp_socket_video;

  if ( (rtcp_socket_reuse_video != NULL)
       && (rtcp_socket_reuse_video != rtcp_socket_video) ) {
    g_object_set (pipeline, "rtp-socket-close", rtcp_socket_reuse_video, NULL);
  }

  rtcp_socket_reuse_video = rtcp_socket_video;
}

MediaObjectImpl *
MediaPipelineImplFactory::createObject (const boost::property_tree::ptree &pt)
const
{
  return new MediaPipelineImpl (pt);
}

MediaPipelineImpl::StaticConstructor MediaPipelineImpl::staticConstructor;

MediaPipelineImpl::StaticConstructor::StaticConstructor()
{
  GST_DEBUG_CATEGORY_INIT (GST_CAT_DEFAULT, GST_DEFAULT_NAME, 0,
                           GST_DEFAULT_NAME);
}

} /* kurento */
