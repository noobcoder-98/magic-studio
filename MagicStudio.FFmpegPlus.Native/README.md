# MagicFFmpegPlayer

Phiên bản strip-down của `ffplay.c` (FFmpeg upstream) — giữ lại lõi
demux/decode/A-V sync, bỏ toàn bộ phần SDL UI, subtitle, libavfilter,
command-line, và thêm pipeline GPU (D3D11VA + ID3D11VideoProcessor) để
xuất frame BGRA cho Win2D không cần copy về CPU.

- `ffplay.c` — bản gốc tham khảo (nguyên trạng từ FFmpeg).
- `MagicFFmpegPlayer.cpp` — bản custom (~1660 dòng, từ 3985).
- `MagicFFmpegPlayer.h` — public C API.

## Cái đã bỏ

| Nhóm | Symbol/section bỏ |
|---|---|
| SDL UI | `SDL_Window`, `SDL_Renderer`, `SDL_Texture`, `event_loop`, `refresh_loop_wait_event`, `video_open`, `video_display`, `video_image_display`, `video_audio_display`, `realloc_texture`, `upload_texture`, `calculate_display_rect`, `draw_video_background`, `set_sdl_yuv_conversion_mode`, `fill_rectangle`, `toggle_full_screen`, `toggle_audio_display`, `seek_chapter`, `stream_cycle_channel`, `set_default_window_size` |
| Subtitle | `subdec` / `subpq` / `subtitleq`, `subtitle_thread`, `AVSubtitle`, `sub_convert_ctx`, `USE_ONEPASS_SUBTITLE_RENDER` |
| libavfilter | `configure_filtergraph` / `configure_video_filters` / `configure_audio_filters`, `in_video_filter` / `out_video_filter` / `in_audio_filter` / `out_audio_filter` / `agraph`, `vfilter_idx`, autorotate filter chain, `INSERT_FILT` |
| Audio viz | `SHOW_MODE_WAVES` / `SHOW_MODE_RDFT`, `update_sample_display`, `sample_array`, `rdft*`, `compute_mod` |
| Command-line | `main`, `parse_options`, `show_usage` / `show_help_default`, mọi `opt_*`, `OptionDef options[]`, `cmdutils.h`, `opt_common.h`, `init_dynload`, `print_error`, `dump_dictionary`, `sigterm_handler`, `do_exit`, `show_status`, `show_banner` |
| Vulkan | `vk_renderer*`, `enable_vulkan`, `vulkan_params`, `ffplay_renderer.h` |
| Khác | `sdl_texture_format_map`, `TextureFormatEntry`, `RenderParams`, `video_background` |

## Cái giữ verbatim từ ffplay

- `PacketQueue` (SDL mutex/cond + `AVFifo`), `FrameQueue` (3 video / 9 sample), `Decoder`, `Clock`
  (`get_clock` / `set_clock_at` / `sync_clock_to_slave`).
- `read_thread` (demux), `video_thread`, `audio_thread`.
- A/V sync core: `compute_target_delay`, `synchronize_audio`, `vp_duration`,
  `update_video_pts`, `framedrop` (early/late), audio master clock.
- SDL audio output: `sdl_audio_callback`, `audio_open` (`SDL_OpenAudioDevice`
  với fallback frequency/channel), `audio_decode_frame` qua `swr_ctx`.
- `stream_seek`, `toggle_pause`, `stream_toggle_pause`, EOF, `attached_pic`.

## Mới thêm — GPU output

| Thành phần | Vai trò |
|---|---|
| `GpuPipeline` (singleton) | Shared `ID3D11Device` (BGRA + VIDEO support, multithread-protected) + `ID3D11VideoProcessor` (BT.709 limited → sRGB full). |
| `create_hwaccel` | Hardcode `AV_HWDEVICE_TYPE_D3D11VA` bind vào device dùng chung; cài `AVD3D11VADeviceContext.lock/unlock` cho FFmpeg. |
| `get_d3d11_format` | Ép codec chọn `AV_PIX_FMT_D3D11`. |
| `gpu_convert_nv12_to_bgra` | `VideoProcessorBlt` zero-copy NV12 array-slice → BGRA `ID3D11Texture2D`. |
| `Frame.tex` | BGRA texture lưu kèm `AVFrame`; `frame_queue_unref_item` release. |
| `refresh_thread` | Chạy `video_refresh` để `pictq.rindex` tiến đúng nhịp PTS; UI chỉ poll texture ở vsync. |

## Public API (`MagicFFmpegPlayer.h`)

```c
MagicPlayerHandle* magic_player_open(const char* path);
void   magic_player_close(MagicPlayerHandle* h);

void   magic_player_toggle_pause(MagicPlayerHandle* h);
int    magic_player_is_paused   (MagicPlayerHandle* h);
void   magic_player_seek_us     (MagicPlayerHandle* h, int64_t position_us);

int64_t magic_player_master_clock_us  (MagicPlayerHandle* h);
double  magic_player_duration_seconds (MagicPlayerHandle* h);
int     magic_player_video_size       (MagicPlayerHandle* h, int* w, int* h_out);

// AddRef'd; caller releases.
int     magic_player_acquire_current_texture(MagicPlayerHandle* h, ID3D11Texture2D** out);
int     magic_player_acquire_dxgi_device(IDXGIDevice** out);
```

## Cách dùng phía Win2D

1. `magic_player_acquire_dxgi_device` → wrap qua
   `CreateDirect3D11DeviceFromDXGIDevice` → `IDirect3DDevice` →
   `CanvasDevice.CreateFromDirect3D11Device` → set `CanvasControl.CustomDevice`.
2. Mỗi `Draw`: `magic_player_acquire_current_texture` → wrap qua
   `CreateDirect3D11SurfaceFromDXGISurface` → `CanvasBitmap.CreateFromDirect3D11Surface`
   → `DrawingSession.DrawImage`. Không có copy CPU.

## Còn lại (chưa làm)

- Chưa add vào `MagicStudio.FFmpegPlus.Native.vcxproj`. Khi integrate cần:
  - Add SDL2 vào `native/include` + `native/lib`, link `SDL2.lib`, copy `SDL2.dll` ra output.
  - Add `MagicFFmpegPlayer.cpp` + `.h` vào `<ItemGroup>` của vcxproj.
  - Wire `MediaPlayerWrapper` gọi `magic_player_*` thay cho `MediaDecoder` cũ.
- HW decode là cứng — codec không có D3D11VA hwaccel sẽ fail `magic_player_open`.
  Nếu cần fallback SW có thể nới `create_hwaccel` (chấp nhận `AVERROR(ENOSYS)`)
  + giữ lại path `sws_scale` upload BGRA texture.
- Color space hardcode BT.709 limited → full RGB. SD content (BT.601) sẽ hơi
  lệch màu — đọc `frame->colorspace` / `frame->color_range` để chọn động khi cần.
