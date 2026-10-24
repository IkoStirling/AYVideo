#include <AYVideo/DecodeSession.h>
#include "../backend/FFmpegDemuxer.h"
#include "../backend/FFmpegDecoder.h"
#include <algorithm>
#include <atomic>
#include <cstring>
namespace ayt::video {
struct DecodeSession::Impl {FFmpegDemuxer demux;FFmpegDecoder decoder;MediaInfo media;bool eof=false,opened=false;std::atomic<bool> abort=false;};
DecodeSession::DecodeSession():impl_(std::make_unique<Impl>()){}
DecodeSession::~DecodeSession(){close();}
VideoResult DecodeSession::open(const DemuxerOpenParams& source,bool audio){auto& a=*impl_;close();a.abort=false;a.demux.clearAbort();auto result=a.demux.open(source);if(result!=VideoResult::Ok)return result;
    result=a.demux.getMediaInfo(a.media);if(result!=VideoResult::Ok){close();return result;}
    if(!a.media.hasVideo||a.media.width<=0||a.media.height<=0||static_cast<uint64_t>(a.media.width)*a.media.height>4096ULL*2160){close();return VideoResult::UnsupportedFormat;}
    DecoderOpenParams options;options.codecName=a.media.videoCodec;options.media=a.media;options.decodeAudio=audio&&a.media.hasAudio;
    result=a.decoder.open(options);if(result!=VideoResult::Ok){close();return result;}a.eof=false;a.opened=true;return VideoResult::Ok;
}
VideoResult DecodeSession::next(DecodedSample& sample){auto& a=*impl_;sample={};if(!a.opened)return VideoResult::NotInitialized;
    for(unsigned packetCount=0;packetCount<512;++packetCount){if(a.abort)return VideoResult::Cancelled;VideoFrame frame{};AudioPcmFrame pcm{};
        auto status=a.decoder.dequeueFrame(frame);if(status!=VideoResult::Ok&&status!=VideoResult::EndOfStream)return status;
        const auto audioStatus=a.decoder.dequeueAudioFrame(pcm);if(audioStatus!=VideoResult::Ok&&audioStatus!=VideoResult::EndOfStream)return audioStatus;
        if(frame.data){const auto w=frame.width,h=frame.height;if(w<=0||h<=0||static_cast<uint64_t>(w)*h>4096ULL*2160)return VideoResult::UnsupportedFormat;sample.width=w;sample.height=h;sample.videoPts=frame.pts;sample.bgra.resize(static_cast<size_t>(w)*h*4);
            if(frame.format==VideoPixelFormat::BGRA8||frame.format==VideoPixelFormat::RGBA8){if(frame.stride<static_cast<uint32_t>(w)*4||static_cast<uint64_t>(frame.stride)*h>frame.dataSize)return VideoResult::DecodeError;for(int y=0;y<h;++y)for(int x=0;x<w;++x){const auto* p=frame.data+static_cast<size_t>(y)*frame.stride+x*4;auto* d=sample.bgra.data()+(static_cast<size_t>(y)*w+x)*4;d[0]=p[frame.format==VideoPixelFormat::BGRA8?0:2];d[1]=p[1];d[2]=p[frame.format==VideoPixelFormat::BGRA8?2:0];d[3]=255;}}
            else if(frame.format==VideoPixelFormat::I420||frame.format==VideoPixelFormat::NV12){const auto cw=(w+1)/2,ch=(h+1)/2;const auto chromaBytes=static_cast<uint64_t>(cw)*ch*(frame.format==VideoPixelFormat::NV12?2:1);if(frame.stride<static_cast<uint32_t>(w)||static_cast<uint64_t>(frame.stride)*h>frame.dataSize||frame.planeOffset[1]+chromaBytes>frame.dataSize||(frame.format==VideoPixelFormat::I420&&frame.planeOffset[2]+chromaBytes>frame.dataSize))return VideoResult::DecodeError;
                for(int y=0;y<h;++y)for(int x=0;x<w;++x){const int yy=frame.data[static_cast<size_t>(y)*frame.stride+x]-16;const auto uv=(y/2)*cw+x/2;const int u=(frame.format==VideoPixelFormat::I420?frame.data[frame.planeOffset[1]+uv]:frame.data[frame.planeOffset[1]+uv*2])-128;const int v=(frame.format==VideoPixelFormat::I420?frame.data[frame.planeOffset[2]+uv]:frame.data[frame.planeOffset[1]+uv*2+1])-128;auto* d=sample.bgra.data()+(static_cast<size_t>(y)*w+x)*4;d[0]=static_cast<uint8_t>(std::clamp((298*yy+516*u+128)>>8,0,255));d[1]=static_cast<uint8_t>(std::clamp((298*yy-100*u-208*v+128)>>8,0,255));d[2]=static_cast<uint8_t>(std::clamp((298*yy+409*v+128)>>8,0,255));d[3]=255;}}
            else return VideoResult::UnsupportedFormat;
        }
        if(pcm.data){if(!pcm.channels||pcm.channels>8||!pcm.sampleRate||pcm.frameCount>262144)return VideoResult::DecodeError;sample.audio.assign(pcm.data,pcm.data+static_cast<size_t>(pcm.frameCount)*pcm.channels);sample.channels=pcm.channels;sample.sampleRate=pcm.sampleRate;sample.audioPts=pcm.pts;}
        if(!sample.bgra.empty()||!sample.audio.empty())return VideoResult::Ok;
        if(a.eof)return VideoResult::EndOfStream;
        VideoPacket packet;status=a.demux.readNextPacket(packet);if(status==VideoResult::EndOfStream){a.eof=true;const auto drained=a.decoder.flush();if(drained!=VideoResult::Ok)return drained;continue;}if(status!=VideoResult::Ok)return status;
        status=a.decoder.feedPacket(packet);if(status!=VideoResult::Ok)return status;
    }return VideoResult::Ok;
}
VideoResult DecodeSession::seek(ayt::time::Duration position){auto& a=*impl_;if(!a.opened)return VideoResult::NotInitialized;if(position.toUs()<0)return VideoResult::InvalidArgument;a.demux.clearAbort();a.abort=false;auto status=a.demux.seek(position);if(status!=VideoResult::Ok)return status;a.eof=false;return a.decoder.flush();}
MediaInfo DecodeSession::info()const{return impl_->media;}
void DecodeSession::requestAbort()noexcept{impl_->abort=true;impl_->demux.requestAbort();}
void DecodeSession::clearAbort()noexcept{impl_->abort=false;impl_->demux.clearAbort();}
void DecodeSession::close()noexcept{impl_->decoder.close();impl_->demux.close();impl_->opened=false;impl_->eof=false;impl_->media={};}
}
