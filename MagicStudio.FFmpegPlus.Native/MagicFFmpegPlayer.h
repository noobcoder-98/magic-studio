// ============================================================================
// Public C API for the stripped ffplay variant in MagicFFmpegPlayer.cpp.
// Designed to be called from the C++/CLI wrapper or any plain-C consumer.
// All texture / device handles are AddRef'd; callers release.
// ============================================================================
#pragma once

#include <cstdint>
#include <d3d11.h>
#include <dxgi.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MagicPlayerHandle MagicPlayerHandle;

// Open a media file and start the demux + decode + audio threads.
// Returns NULL on failure. The returned handle owns SDL audio output and
// the FFmpeg state; release it with magic_player_close.
MagicPlayerHandle* magic_player_open(const char* path);

void  magic_player_close              (MagicPlayerHandle* h);

void  magic_player_toggle_pause       (MagicPlayerHandle* h);
int   magic_player_is_paused          (MagicPlayerHandle* h);
void  magic_player_seek_us            (MagicPlayerHandle* h, int64_t position_us);

int64_t magic_player_master_clock_us  (MagicPlayerHandle* h);
double  magic_player_duration_seconds (MagicPlayerHandle* h);
int     magic_player_video_size       (MagicPlayerHandle* h, int* w, int* h_out);

// Snapshot the BGRA texture for the frame whose presentation time is current.
// *out is AddRef'd on success and the function returns 1; caller releases.
int magic_player_acquire_current_texture(MagicPlayerHandle* h, ID3D11Texture2D** out);

// Monotonic id of the currently-shown frame (0 if none yet). Cheap to call;
// hosts can compare against the last value they observed and skip a fresh
// readback / texture upload when nothing changed.
uint64_t magic_player_current_frame_version(MagicPlayerHandle* h);

// Copy the current BGRA frame into a tightly-packed (no row padding) buffer
// supplied by the caller. dst must be at least width*height*4 bytes.
// Returns 1 on success; *out_w / *out_h are filled with the frame dimensions.
int magic_player_copy_current_bgra    (MagicPlayerHandle* h,
                                       uint8_t* dst, int dst_capacity_bytes,
                                       int* out_w, int* out_h);

// Hand out the shared IDXGIDevice so the host (Win2D) can build a CanvasDevice
// bound to the same D3D11 device the decoder + VideoProcessor write to.
int magic_player_acquire_dxgi_device(IDXGIDevice** out);

#ifdef __cplusplus
} // extern "C"
#endif
