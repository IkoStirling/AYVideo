#include <AYVideo/DecodeSession.h>
#include <AYTestFixtures.h>
#include "../unittest/FFmpegTestMedia.h"
#include <httplib.h>
#include <fstream>
#include <thread>
using namespace ayt::video;
TEST_SUITE(StandaloneDecode)
TEST_CASE(real_generated_video_and_audio_decode_without_game_runtime){
    ayt::test::ScratchDirectory scratch("decode-session");const auto path=scratch.path()/"sample.mp4";std::string issue;
    CHECK(ayt::testmedia::generateClip(path.string(),true,12,64,48,25,issue));DecodeSession session;CHECK(session.open({path.string()},true)==VideoResult::Ok);CHECK(session.info().hasAudio);
    int frames=0,audio=0;std::vector<uint8_t> first;std::int64_t previous=-1;bool ended=false;
    for(int n=0;n<2000;++n){DecodedSample sample;const auto result=session.next(sample);if(result==VideoResult::EndOfStream){ended=true;break;}CHECK(result==VideoResult::Ok);if(!sample.bgra.empty()){CHECK_INT_EQ(sample.bgra.size(),64*48*4);CHECK(sample.videoPts.toUs()>=previous);previous=sample.videoPts.toUs();if(!frames)first=sample.bgra;++frames;}if(!sample.audio.empty()){CHECK(sample.channels==2);CHECK(sample.sampleRate==48000);++audio;}}
    CHECK(ended);CHECK_INT_EQ(frames,12);CHECK(audio>0);CHECK_INT_EQ(first.size(),64*48*4);CHECK(session.seek(ayt::time::Duration::fromMs(0))==VideoResult::Ok);DecodedSample again;for(int n=0;n<100&&again.bgra.empty();++n)CHECK(session.next(again)==VideoResult::Ok);CHECK(!again.bgra.empty());CHECK(again.bgra==first);
    session.requestAbort();CHECK(session.next(again)==VideoResult::Cancelled);session.clearAbort();session.close();CHECK(session.next(again)==VideoResult::NotInitialized);
}
TEST_CASE(local_http_url_uses_existing_network_demux_backend){
    ayt::test::ScratchDirectory scratch("decode-http");const auto path=scratch.path()/"stream.mp4";std::string issue;CHECK(ayt::testmedia::generateClip(path.string(),false,6,64,48,25,issue));std::ifstream input(path,std::ios::binary);std::string bytes((std::istreambuf_iterator<char>(input)),{});
    httplib::Server server;server.Get("/clip.mp4",[&](const auto&,auto& response){response.set_content(bytes,"video/mp4");});const int port=server.bind_to_any_port("127.0.0.1");CHECK(port>0);std::thread listener([&]{server.listen_after_bind();});
    {DecodeSession session;DemuxerOpenParams options;options.path="http://127.0.0.1:"+std::to_string(port)+"/clip.mp4";options.openTimeoutMs=3000;options.rwTimeoutMs=3000;CHECK(session.open(options,false)==VideoResult::Ok);DecodedSample sample;for(int n=0;n<100&&sample.bgra.empty();++n)CHECK(session.next(sample)==VideoResult::Ok);CHECK(!sample.bgra.empty());}
    server.stop();listener.join();
}
TEST_CASE(explicit_source_allowlists_apply_to_the_actual_ffmpeg_open){
    ayt::test::ScratchDirectory scratch("decode-allowlists");const auto path=scratch.path()/"clip.mp4";std::string issue;CHECK(ayt::testmedia::generateClip(path.string(),false,6,64,48,25,issue));
    DecodeSession session;DemuxerOpenParams options;options.path=path.string();options.protocolAllowlist="file";options.formatAllowlist="mov";CHECK(session.open(options,false)==VideoResult::Ok);session.close();
    options.protocolAllowlist="http,tcp";CHECK(session.open(options,false)!=VideoResult::Ok);options.protocolAllowlist="file";options.formatAllowlist="asf";CHECK(session.open(options,false)!=VideoResult::Ok);
    options.formatAllowlist="mov";options.protocolAllowlist="file\nhttp";CHECK(session.open(options,false)==VideoResult::InvalidArgument);
    options.protocolAllowlist="file";options.formatAllowlist="mov,,hls";CHECK(session.open(options,false)==VideoResult::InvalidArgument);
}
TEST_SUITE_END
int main(int argc,char** argv){return ayt::test::runTests("AYVideoDecode",argc,argv);}
