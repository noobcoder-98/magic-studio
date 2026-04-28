// ============================================================================
// Public C API for MagicFFplay.cpp -- a C++ port of ffplay.c with SDL display
// and subtitle pipeline removed, and Win2D-compatible BGRA texture output
// added via D3D11VA + ID3D11VideoProcessor.
//
// All texture / device handles are AddRef'd; callers must Release.
// ============================================================================
#pragma once

#include <cstdint>
#include <d3d11.h>
#include <dxgi.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MagicFFplayHandle MagicFFplayHandle;

// Open a media file and start the demux + decode + audio threads.
// Blocks until the file header has been parsed so the caller sees a valid
// Duration / VideoSize on return.  Returns NULL on failure.
MagicFFplayHandle* magic_ffplay_open(const char* path);

void    magic_ffplay_close           (MagicFFplayHandle* h);

void    magic_ffplay_toggle_pause    (MagicFFplayHandle* h);
int     magic_ffplay_is_paused       (MagicFFplayHandle* h);
void    magic_ffplay_seek_us         (MagicFFplayHandle* h, int64_t position_us);
void    magic_ffplay_step_frame      (MagicFFplayHandle* h);

// Master clock in microseconds (audio-anchored by default).
int64_t magic_ffplay_master_clock_us (MagicFFplayHandle* h);
double  magic_ffplay_duration_seconds(MagicFFplayHandle* h);
int     magic_ffplay_video_size      (MagicFFplayHandle* h, int* w, int* h_out);

// Returns the AddRef'd BGRA ID3D11Texture2D* for the most recently presented
// frame.  Returns 1 on success; *out is set.  Caller releases.
int magic_ffplay_acquire_current_texture(MagicFFplayHandle* h, ID3D11Texture2D** out);

// Cheap monotonic id for the currently-shown frame -- compare with the last
// value to skip unnecessary CanvasBitmap rewrapping when nothing changed.
uint64_t magic_ffplay_current_frame_version(MagicFFplayHandle* h);

// Copy the current BGRA frame into a CPU buffer (tightly packed, no padding).
// dst must be >= width*height*4 bytes.  Returns 1 on success.
int magic_ffplay_copy_current_bgra(MagicFFplayHandle* h,
                                   uint8_t* dst, int dst_capacity_bytes,
                                   int* out_w, int* out_h);

// Returns the AddRef'd IDXGIDevice* of the player's own D3D11 device so the
// host can build a CanvasDevice bound to the same GPU pipeline.
int magic_ffplay_acquire_dxgi_device(MagicFFplayHandle* h, IDXGIDevice** out);

#ifdef __cplusplus
} // extern "C"
#endif
