#include "api/audio_codecs/builtin_audio_encoder_factory.h"
#include "api/video_codecs/builtin_video_encoder_factory.h"
#include "api/video_codecs/builtin_video_decoder_factory.h"
#include "api/create_peerconnection_factory.h"
#include "api/peer_connection_interface.h"
#include "Interop.h"

extern "C" RTC_EXPORT webrtc::AudioEncoderFactory*
CreateBuiltinAudioEncoderFactory() {
  auto srp = webrtc::CreateBuiltinAudioEncoderFactory();
  auto p = (webrtc::AudioEncoderFactory*)&srp;
  return p;
}

extern "C" RTC_EXPORT webrtc::VideoEncoderFactory* 
CreateBuiltinVideoEncoderFactory() 
{
  auto up = webrtc::CreateBuiltinVideoEncoderFactory();
  auto p = up.release();
  return p;
}

extern "C" RTC_EXPORT webrtc::VideoDecoderFactory*
CreateBuiltinVideoDecoderFactory() 
{
  auto up = webrtc::CreateBuiltinVideoDecoderFactory();
  auto p = up.release();
  return p;

  //// Raw pointer to unique pointer.
  ////auto x = std::unique_ptr<webrtc::VideoDecoderFactory>{rp} 

}



//extern "C" RTC_EXPORT rtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface>*
////webrtc::PeerConnectionFactoryInterface*
//CallCreatePeerConnectionFactory(
//    rtc::Thread* network_thread,
//    rtc::Thread* worker_thread,
//    rtc::Thread* signaling_thread,
//    rtc::scoped_refptr<webrtc::AudioDeviceModule>* default_adm,
//    rtc::scoped_refptr<webrtc::AudioEncoderFactory>* audio_encoder_factory,
//    rtc::scoped_refptr<webrtc::AudioDecoderFactory>* audio_decoder_factory,
//    std::unique_ptr<webrtc::VideoEncoderFactory>* video_encoder_factory,
//    std::unique_ptr<webrtc::VideoDecoderFactory>* video_decoder_factory,
//    rtc::scoped_refptr<webrtc::AudioMixer>* audio_mixer,
//    rtc::scoped_refptr<webrtc::AudioProcessing>* audio_processing,
//    webrtc::AudioFrameProcessor* audio_frame_processor = nullptr,
//    std::unique_ptr<webrtc::FieldTrialsView>* field_trials = nullptr) 
//{
//#pragma GCC diagnostic push
//#pragma GCC diagnostic ignored "-Waddress-of-temporary" 
//  return &webrtc::CreatePeerConnectionFactory(
//      network_thread, worker_thread, signaling_thread, *default_adm,
//      *audio_encoder_factory, *audio_decoder_factory,
//      nullptr,  //*video_encoder_factory,
//      nullptr,  //*video_decoder_factory,
//      *audio_mixer, *audio_processing, audio_frame_processor,
//      *field_trials);
//#pragma GCC diagnostic pop
//}
//

//extern "C" RTC_EXPORT PeerConnectionFactoryInterface* NewPeerConnectionFactoryInterface()
//{
//	return new PeerConnectionFactoryInterface();
//}
//
//extern "C" RTC_EXPORT void DeletePeerConnectionFactoryInterface(PeerConnectionFactoryInterface* pObject)
//{
//	if (pObject != NULL)
//	{
//          delete pObject;
//          pObject = NULL;
//	}
//}
//
//
