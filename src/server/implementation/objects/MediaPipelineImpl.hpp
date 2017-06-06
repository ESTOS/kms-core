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
#ifndef __MEDIA_PIPELINE_IMPL_HPP__
#define __MEDIA_PIPELINE_IMPL_HPP__

#include "MediaObjectImpl.hpp"
#include "MediaPipeline.hpp"
#include <EventHandler.hpp>
#include <gst/gst.h>
#include <gio/gio.h>
#include <boost/property_tree/ptree.hpp>

namespace kurento
{

class MediaPipelineImpl;

void Serialize (std::shared_ptr<MediaPipelineImpl> &object,
                JsonSerializer &serializer);

class MediaPipelineImpl : public MediaObjectImpl, public virtual MediaPipeline
{

public:

  MediaPipelineImpl (const boost::property_tree::ptree &config);

  virtual ~MediaPipelineImpl ();

  GstElement *getPipeline()
  {
    return pipeline;
  }

  virtual std::string getGstreamerDot ();
  virtual std::string getGstreamerDot (std::shared_ptr<GstreamerDotDetails>
                                       details);

  virtual bool getLatencyStats ();
  virtual void setLatencyStats (bool latencyStats);

  /* Next methods are automatically implemented by code generator */
  virtual bool connect (const std::string &eventType,
                        std::shared_ptr<EventHandler> handler);

  virtual void invoke (std::shared_ptr<MediaObjectImpl> obj,
                       const std::string &methodName, const Json::Value &params,
                       Json::Value &response);

  virtual void Serialize (JsonSerializer &serializer);

  bool addElement (GstElement *element);

  void getSockets (GSocket **rtp_socket_audio, GSocket **rtcp_socket_audio,
                   GSocket **rtp_socket_video, GSocket **rtcp_socket_video);
  void setSockets (GSocket *rtp_socket_audio, GSocket *rtcp_socket_audio,
                   GSocket *rtp_socket_video, GSocket *rtcp_socket_video);

protected:
  virtual void postConstructor ();
private:

  GstElement *pipeline;

  gulong busMessageHandler;

  std::recursive_mutex recMutex;
  bool latencyStats = false;

  void busMessage (GstMessage *message);

  class StaticConstructor
  {
  public:
    StaticConstructor();
  };

  static StaticConstructor staticConstructor;

  GSocket *rtp_socket_reuse_audio; //ru-bu
  GSocket *rtcp_socket_reuse_audio;
  GSocket *rtp_socket_reuse_video;
  GSocket *rtcp_socket_reuse_video;
};

} /* kurento */

#endif /*  __MEDIA_PIPELINE_IMPL_HPP__ */
