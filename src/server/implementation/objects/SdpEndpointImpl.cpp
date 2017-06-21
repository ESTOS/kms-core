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
#include "SdpEndpointImpl.hpp"
#include <jsonrpc/JsonSerializer.hpp>
#include <KurentoException.hpp>
#include <gst/gst.h>
#include <boost/filesystem.hpp>
#include <fstream>
#include <CodecConfiguration.hpp>
#include <gst/sdp/gstsdpmessage.h>
#include <MediaPipelineImpl.hpp>

#define GST_CAT_DEFAULT kurento_sdp_endpoint_impl
GST_DEBUG_CATEGORY_STATIC (GST_CAT_DEFAULT);
#define GST_DEFAULT_NAME "KurentoSdpEndpointImpl"

#define PARAM_NUM_AUDIO_MEDIAS "numAudioMedias"
#define PARAM_NUM_VIDEO_MEDIAS "numVideoMedias"
#define PARAM_CODEC_NAME "name"
#define PARAM_AUDIO_CODECS "audioCodecs"
#define PARAM_VIDEO_CODECS "videoCodecs"
#define PARAM_LOCAL_ADDRESS "localAddress"
#define PARAM_SOCKET_REUSE "socketreuse"

namespace kurento
{

static GstSDPMessage *
str_to_sdp (const std::string &sdpStr)
{
  GstSDPResult result;
  GstSDPMessage *sdp = NULL;

  result = gst_sdp_message_new (&sdp);

  if (result != GST_SDP_OK) {
    throw KurentoException (SDP_CREATE_ERROR, "Error creating SDP message");
  }

  result = gst_sdp_message_parse_buffer ( (const guint8 *) sdpStr.c_str (), -1,
                                          sdp);

  if (result != GST_SDP_OK) {

    gst_sdp_message_free (sdp);
    throw KurentoException (SDP_PARSE_ERROR, "Error parsing SDP");
  }

  if (gst_sdp_message_get_version (sdp) == NULL) {
    gst_sdp_message_free (sdp);
    throw KurentoException (SDP_PARSE_ERROR, "Invalid SDP");
  }

  return sdp;
}

static void
sdp_to_str (std::string &_return, const GstSDPMessage *sdp)
{
  gchar *sdpGchar;

  sdpGchar = gst_sdp_message_as_text (sdp);
  _return.clear ();
  _return.append (sdpGchar);
  free (sdpGchar);
}

static void
append_codec_to_array (GArray *array, const char *codec)
{
  GValue v = G_VALUE_INIT;
  GstStructure *s;

  g_value_init (&v, GST_TYPE_STRUCTURE);
  s = gst_structure_new_empty (codec);
  gst_value_set_structure (&v, s);
  gst_structure_free (s);
  g_array_append_val (array, v);
}

void SdpEndpointImpl::postConstructor ()
{
  gchar *sess_id;
  SessionEndpointImpl::postConstructor ();

  g_signal_emit_by_name (element, "create-session", &sess_id);

  if (sess_id == NULL) {
    throw KurentoException (SDP_END_POINT_CANNOT_CREATE_SESSON,
                            "Cannot create session");
  }

  sessId = std::string (sess_id);
  g_free (sess_id);

  if (dosocketreuse) { //ru-bu get rtp_socket_reuse from pipeline
    std::shared_ptr<MediaPipelineImpl> pipe;
    pipe = std::dynamic_pointer_cast<MediaPipelineImpl> (getMediaPipeline() );
    GSocket *rtp_socket_reuse_audio, *rtcp_socket_reuse_audio,
            *rtp_socket_reuse_video, *rtcp_socket_reuse_video;
    pipe->getSockets (&rtp_socket_reuse_audio, &rtcp_socket_reuse_audio,
                      &rtp_socket_reuse_video, &rtcp_socket_reuse_video);

    //set it in the new session
    if (rtp_socket_reuse_audio) {
      g_signal_emit_by_name (element, "get-set-rtp-socket-audio", sessId.c_str(),
                             rtp_socket_reuse_audio, &rtp_socket_reuse_audio);
    }

    if (rtcp_socket_reuse_audio) {
      g_signal_emit_by_name (element, "get-set-rtcp-socket-audio", sessId.c_str(),
                             rtcp_socket_reuse_audio, &rtcp_socket_reuse_audio);
    }

    if (rtp_socket_reuse_video) {
      g_signal_emit_by_name (element, "get-set-rtp-socket-video", sessId.c_str(),
                             rtp_socket_reuse_video, &rtp_socket_reuse_video);
    }

    if (rtcp_socket_reuse_video) {
      g_signal_emit_by_name (element, "get-set-rtcp-socket-video", sessId.c_str(),
                             rtcp_socket_reuse_video, &rtcp_socket_reuse_video);
    }
  }
}

SdpEndpointImpl::SdpEndpointImpl (const boost::property_tree::ptree &config,
                                  std::shared_ptr< MediaObjectImpl > parent,
                                  const std::string &factoryName, bool useIpv6) :
  SessionEndpointImpl (config, parent, factoryName)
{
  GArray *audio_codecs, *video_codecs;
  guint audio_medias, video_medias, socket_reuse;
  std::string local_address;
  bool bdosocketreuse;

  if (factoryName == "rtpendpoint") {
    isrtpendpoint = TRUE;
  } else {
    isrtpendpoint = FALSE;
  }

  audio_codecs = g_array_new (FALSE, TRUE, sizeof (GValue) );
  video_codecs = g_array_new (FALSE, TRUE, sizeof (GValue) );

  //   TODO: Add support for this events
  //   g_signal_connect (element, "media-start", G_CALLBACK (media_start_cb), this);
  //   g_signal_connect (element, "media-stop", G_CALLBACK (media_stop_cb), this);

  audio_medias = getConfigValue <guint, SdpEndpoint> (PARAM_NUM_AUDIO_MEDIAS, 0);
  video_medias = getConfigValue <guint, SdpEndpoint> (PARAM_NUM_VIDEO_MEDIAS, 0);
  local_address = getConfigValue<std::string, SdpEndpoint> (PARAM_LOCAL_ADDRESS,
                  "");
  socket_reuse = getConfigValue <guint, SdpEndpoint> (PARAM_SOCKET_REUSE, 1);

  if (socket_reuse == 1 && isrtpendpoint == TRUE) {
    dosocketreuse = TRUE;
  } else {
    dosocketreuse = FALSE;
  }

  try {
    std::vector<std::shared_ptr<CodecConfiguration>> list = getConfigValue
        <std::vector<std::shared_ptr<CodecConfiguration>>, SdpEndpoint>
        (PARAM_AUDIO_CODECS);

    for (std::shared_ptr<CodecConfiguration> conf : list) {

      append_codec_to_array (audio_codecs, conf->getName().c_str() );
    }
  } catch (boost::property_tree::ptree_bad_path &e) {
    /* When key is missing we assume an empty array */
  }

  try {
    std::vector<std::shared_ptr<CodecConfiguration>> list = getConfigValue
        <std::vector<std::shared_ptr<CodecConfiguration>>, SdpEndpoint>
        (PARAM_VIDEO_CODECS);

    for (std::shared_ptr<CodecConfiguration> conf : list) {

      append_codec_to_array (video_codecs, conf->getName().c_str() );
    }
  } catch (boost::property_tree::ptree_bad_path &e) {
    /* When key is missing we assume an empty array */
  }

  g_object_set (element, "num-audio-medias", audio_medias, "audio-codecs",
                audio_codecs, NULL);
  g_object_set (element, "num-video-medias", video_medias, "video-codecs",
                video_codecs, NULL);
  g_object_set (element, "use-ipv6", useIpv6, NULL);

  /* set RtpEndpoints Address from config ru-bu */
  if ( (local_address.empty () == false) && (isrtpendpoint == TRUE) ) {
    g_object_set (element, "addr", local_address.c_str(), NULL);
  }

  bdosocketreuse = dosocketreuse;
  g_object_set (element, "reuse-socket", bdosocketreuse, NULL);

  offerInProcess = false;
  waitingAnswer = false;
  answerProcessed = false;
}

int SdpEndpointImpl::getMaxVideoRecvBandwidth ()
{
  int maxVideoRecvBandwidth;

  g_object_get (element, "max-video-recv-bandwidth", &maxVideoRecvBandwidth,
                NULL);

  return maxVideoRecvBandwidth;
}

void SdpEndpointImpl::setMaxVideoRecvBandwidth (int maxVideoRecvBandwidth)
{
  g_object_set (element, "max-video-recv-bandwidth", maxVideoRecvBandwidth, NULL);
}

int SdpEndpointImpl::getMaxAudioRecvBandwidth ()
{
  int maxAudioRecvBandwidth;

  g_object_get (element, "max-audio-recv-bandwidth", &maxAudioRecvBandwidth,
                NULL);

  return maxAudioRecvBandwidth;
}

void SdpEndpointImpl::setMaxAudioRecvBandwidth (int maxAudioRecvBandwidth)
{
  g_object_set (element, "max-audio-recv-bandwidth", maxAudioRecvBandwidth, NULL);
}

std::string SdpEndpointImpl::generateOffer ()
{
  GstSDPMessage *offer = NULL;
  std::string offerStr;
  bool expected = false;

  if (!offerInProcess.compare_exchange_strong (expected, true) ) {
    //the endpoint is already negotiated
    throw KurentoException (SDP_END_POINT_ALREADY_NEGOTIATED,
                            "Endpoint already negotiated");
  }

  g_signal_emit_by_name (element, "generate-offer", sessId.c_str (), &offer);

  if (offer == NULL) {
    offerInProcess = false;
    throw KurentoException (SDP_END_POINT_GENERATE_OFFER_ERROR,
                            "Error generating offer");
  }

  sdp_to_str (offerStr, offer);
  gst_sdp_message_free (offer);
  waitingAnswer = true;

  if (dosocketreuse) { //ru-bu set rtp_socket_reuse to pipeline
    std::shared_ptr<MediaPipelineImpl> pipe;
    pipe = std::dynamic_pointer_cast<MediaPipelineImpl> (getMediaPipeline() );
    GSocket *rtp_socket_reuse_audio = NULL, *rtcp_socket_reuse_audio = NULL,
             *rtp_socket_reuse_video = NULL, *rtcp_socket_reuse_video = NULL;

    g_signal_emit_by_name (element, "get-set-rtp-socket-audio", sessId.c_str(),
                           rtp_socket_reuse_audio, &rtp_socket_reuse_audio);
    g_signal_emit_by_name (element, "get-set-rtcp-socket-audio", sessId.c_str(),
                           rtcp_socket_reuse_audio, &rtcp_socket_reuse_audio);
    g_signal_emit_by_name (element, "get-set-rtp-socket-video", sessId.c_str(),
                           rtp_socket_reuse_video, &rtp_socket_reuse_video);
    g_signal_emit_by_name (element, "get-set-rtcp-socket-video", sessId.c_str(),
                           rtcp_socket_reuse_video, &rtcp_socket_reuse_video);

    pipe->setSockets (rtp_socket_reuse_audio, rtcp_socket_reuse_audio,
                      rtp_socket_reuse_video, rtcp_socket_reuse_video);
  }

  return offerStr;
}

std::string SdpEndpointImpl::processOffer (const std::string &offer)
{
  GstSDPMessage *offerSdp = NULL, *result = NULL;
  std::string offerSdpStr;
  bool expected = false;

  if (offer.empty () ) {
    throw KurentoException (SDP_PARSE_ERROR, "Empty offer not valid");
  }

  offerSdp = str_to_sdp (offer);

  //ru-bu todo
  if (!offerInProcess.compare_exchange_strong (expected, true) ) {
    //the endpoint is already negotiated
    throw KurentoException (SDP_END_POINT_ALREADY_NEGOTIATED,
                            "Endpoint already negotiated");
  }

  g_signal_emit_by_name (element, "process-offer", sessId.c_str (), offerSdp,
                         &result);
  gst_sdp_message_free (offerSdp);

  if (result == NULL) {
    offerInProcess = false;
    throw KurentoException (SDP_END_POINT_PROCESS_OFFER_ERROR,
                            "Error processing offer");
  }

  sdp_to_str (offerSdpStr, result);
  gst_sdp_message_free (result);

  MediaSessionStarted event (shared_from_this(), MediaSessionStarted::getName() );
  signalMediaSessionStarted (event);

  if (dosocketreuse) { //ru-bu set rtp_socket_reuse to pipeline
    std::shared_ptr<MediaPipelineImpl> pipe;
    pipe = std::dynamic_pointer_cast<MediaPipelineImpl> (getMediaPipeline() );
    GSocket *rtp_socket_reuse_audio = NULL, *rtcp_socket_reuse_audio = NULL,
             *rtp_socket_reuse_video = NULL, *rtcp_socket_reuse_video = NULL;

    g_signal_emit_by_name (element, "get-set-rtp-socket-audio", sessId.c_str(),
                           rtp_socket_reuse_audio, &rtp_socket_reuse_audio);
    g_signal_emit_by_name (element, "get-set-rtcp-socket-audio", sessId.c_str(),
                           rtcp_socket_reuse_audio, &rtcp_socket_reuse_audio);
    g_signal_emit_by_name (element, "get-set-rtp-socket-video", sessId.c_str(),
                           rtp_socket_reuse_video, &rtp_socket_reuse_video);
    g_signal_emit_by_name (element, "get-set-rtcp-socket-video", sessId.c_str(),
                           rtcp_socket_reuse_video, &rtcp_socket_reuse_video);

    pipe->setSockets (rtp_socket_reuse_audio, rtcp_socket_reuse_audio,
                      rtp_socket_reuse_video, rtcp_socket_reuse_video);
  }

  return offerSdpStr;
}

std::string SdpEndpointImpl::processAnswer (const std::string &answer)
{
  GstSDPMessage *answerSdp;
  bool expected = true;
  bool expected_false = false;
  gboolean result;

  if (answer.empty () ) {
    throw KurentoException (SDP_PARSE_ERROR, "Empty answer not valid");
  }

  if (!waitingAnswer.compare_exchange_strong (expected, true) ) {
    //offer not generated
    throw KurentoException (SDP_END_POINT_NOT_OFFER_GENERATED,
                            "Offer not generated. It is not possible to process an answer.");
  }

  answerSdp = str_to_sdp (answer);

  if (!answerProcessed.compare_exchange_strong (expected_false, true) ) {
    //the endpoint is already negotiated
    gst_sdp_message_free (answerSdp);
    throw KurentoException (SDP_END_POINT_ANSWER_ALREADY_PROCCESED,
                            "Sdp Answer already processed");
  }

  g_signal_emit_by_name (element, "process-answer", sessId.c_str (), answerSdp,
                         &result);
  gst_sdp_message_free (answerSdp);

  if (!result) {
    throw KurentoException (SDP_END_POINT_PROCESS_ANSWER_ERROR,
                            "Error processing answer");
  }

  MediaSessionStarted event (shared_from_this(), MediaSessionStarted::getName() );
  signalMediaSessionStarted (event);

  return getLocalSessionDescriptor ();
}

std::string SdpEndpointImpl::getLocalSessionDescriptor ()
{
  GstSDPMessage *localSdp = NULL;
  std::string localSdpStr;

  g_signal_emit_by_name (element, "get-local-sdp", sessId.c_str (), &localSdp);

  if (localSdp == NULL) {
    throw KurentoException (SDP_END_POINT_NO_LOCAL_SDP_ERROR, "No local SDP");
  }

  sdp_to_str (localSdpStr, localSdp);
  gst_sdp_message_free (localSdp);

  return localSdpStr;
}

std::string SdpEndpointImpl::getRemoteSessionDescriptor ()
{
  GstSDPMessage *remoteSdp = NULL;
  std::string remoteSdpStr;

  g_signal_emit_by_name (element, "get-remote-sdp", sessId.c_str (), &remoteSdp);

  if (remoteSdp == NULL) {
    throw KurentoException (SDP_END_POINT_NO_REMOTE_SDP_ERROR, "No remote SDP");
  }

  sdp_to_str (remoteSdpStr, remoteSdp);;
  gst_sdp_message_free (remoteSdp);

  return remoteSdpStr;
}

SdpEndpointImpl::StaticConstructor SdpEndpointImpl::staticConstructor;

SdpEndpointImpl::StaticConstructor::StaticConstructor()
{
  GST_DEBUG_CATEGORY_INIT (GST_CAT_DEFAULT, GST_DEFAULT_NAME, 0,
                           GST_DEFAULT_NAME);
}

} /* kurento */
