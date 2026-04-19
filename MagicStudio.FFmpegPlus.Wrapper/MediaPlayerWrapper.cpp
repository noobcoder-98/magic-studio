#include "pch.h"
#include "MediaPlayerWrapper.h"

using namespace msclr::interop;

namespace MagicStudio {
namespace FFmpegPlus {
namespace Wrapper {

MediaPlayer::MediaPlayer()
    : _decoder(new MagicStudio::Native::MediaDecoder())
{}

MediaPlayer::~MediaPlayer() {
    this->!MediaPlayer();
}

MediaPlayer::!MediaPlayer() {
    if (_decoder) {
        _decoder->Close();
        delete _decoder;
        _decoder = nullptr;
    }
}

bool MediaPlayer::Open(String^ path) {
    std::string nativePath = marshal_as<std::string>(path);
    return _decoder->Open(nativePath.c_str());
}

void MediaPlayer::Play()  { _decoder->Play();  }
void MediaPlayer::Pause() { _decoder->Pause(); }
void MediaPlayer::Stop()  { _decoder->Stop();  }

Int64 MediaPlayer::GetAudioPositionUs() {
    return static_cast<Int64>(_decoder->GetAudioPositionUs());
}

bool MediaPlayer::TryGetFrame(Int64 audioPtsUs, [Out] FrameData^% frame) {
    std::vector<uint8_t> bgra;
    int w = 0, h = 0;

    if (!_decoder->TryGetFrameForTime(static_cast<int64_t>(audioPtsUs), bgra, w, h))
        return false;

    auto fd = gcnew FrameData();
    fd->Width  = w;
    fd->Height = h;

    array<Byte>^ managed = gcnew array<Byte>(static_cast<int>(bgra.size()));
    pin_ptr<Byte> pin = &managed[0];
    memcpy(pin, bgra.data(), bgra.size());
    fd->BgraData = managed;

    frame = fd;
    return true;
}

int    MediaPlayer::VideoWidth::get()  { return _decoder->GetVideoWidth();  }
int    MediaPlayer::VideoHeight::get() { return _decoder->GetVideoHeight(); }
double MediaPlayer::Duration::get()    { return _decoder->GetDurationSeconds(); }

}}} // namespace MagicStudio::FFmpegPlus::Wrapper
