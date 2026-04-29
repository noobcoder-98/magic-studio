# MagicFFplay — Tài liệu chi tiết

> Bản port C++ của `ffplay.c` (FFmpeg upstream), bỏ toàn bộ SDL display +
> subtitle + libavfilter, gắn pipeline GPU **D3D11VA → ID3D11VideoProcessor →
> BGRA ID3D11Texture2D** để Win2D wrap zero-copy thành `CanvasBitmap`.
>
> Source chính: `MagicStudio.FFmpegPlus.Native/MagicFFplay.cpp` (~1948 dòng)
> + `MagicFFplay.h` (public C API).
> Wrapper C++/CLI: `MagicStudio.FFmpegPlus.Wrapper/MediaPlayerWrapper.cpp`
> (class `FFplayPlayer`).
> Façade C#: `MagicStudio.FFmpegPlus/FFplayPlayer.cs`.
> UI: `MagicStudio.UI/MagicFFplayControl.xaml{.cs}`.

---

## 1. Kiến trúc tổng quan

```
┌─────────────────────────────────────────────────────────────────────┐
│  MagicFFplayControl (XAML / WinUI3)                                  │
│   ├─ CanvasControl  ──── Draw event (60Hz)                          │
│   ├─ DispatcherTimer ── 16.67ms tick → Invalidate khi version đổi   │
│   └─ Slider / Play-Pause                                            │
└────────────────┬────────────────────────────────────────────────────┘
                 │ TryAcquireCurrentTexture, PeekFrameVersion,
                 │ AcquireDxgiDevice
                 ▼
┌─────────────────────────────────────────────────────────────────────┐
│  FFplayPlayer  (C# façade)  →  WrapperFFplay (C++/CLI ref class)    │
└────────────────┬────────────────────────────────────────────────────┘
                 │ magic_ffplay_* C API
                 ▼
┌─────────────────────────────────────────────────────────────────────┐
│  MagicFFplay.cpp  — VideoState + 3 thread chính                     │
│                                                                      │
│   read_thread ── av_read_frame ──► PacketQueue (audio / video)      │
│                                          │             │             │
│                                          ▼             ▼             │
│                                     audio_thread  video_thread      │
│                                          │             │             │
│                                          │  decoder_decode_frame    │
│                                          ▼             ▼             │
│                                       sampq        pictq            │
│                                       (9)          (3, keep_last)   │
│                                          │             │             │
│             SDL audio cb ◄───────────────┘             │             │
│             (audio master clock)                       │             │
│                                                         │             │
│             refresh_thread ── video_refresh ──► tăng pictq.rindex   │
│                                                         │             │
└─────────────────────────────────────────────────────────┼───────────┘
                                                          │
                                                          ▼
                                              FfplayGpu (per-instance):
                                                ID3D11Device
                                                ID3D11VideoProcessor
                                                NV12 → BGRA Blt
                                                tone-map HDR PQ/HLG → SDR
```

Mỗi `MagicFFplayHandle` sở hữu **một** `FfplayGpu` riêng (D3D11 device độc lập)
— cho phép chạy đồng thời với `MagicFFmpegPlayer` mà không tranh chấp queue
GPU.

---

## 2. Luồng hoạt động chi tiết

### 2.1 Khởi tạo (`magic_ffplay_open`)

1. `SDL_Init(SDL_INIT_AUDIO | SDL_INIT_TIMER)` — chỉ subsystem audio + timer,
   **không** init video subsystem (vì không có cửa sổ SDL).
2. `stream_open(path)`:
   - `av_mallocz(VideoState)` — zero-init toàn bộ struct.
   - `new FfplayGpu()` — chưa thực sự tạo device, chỉ alloc struct.
   - `frame_queue_init(pictq, 3, keep_last=1)` — queue video 3 slot, giữ frame
     cuối để tua/pause vẫn còn frame hiển thị.
   - `frame_queue_init(sampq, 9, keep_last=1)` — queue audio 9 slot.
   - `packet_queue_init(videoq / audioq)` — `AVFifo` autogrow + SDL mutex/cond.
   - `init_clock(vidclk / audclk / extclk)` — speed=1.0, paused=0, pts=NaN.
   - `SDL_CreateThread(read_thread, "ffplay_read", is)`.
   - `SDL_CreateThread(refresh_thread, "ffplay_refresh", is)`.
3. **Synchronous handshake**: caller block trên `prep_cond` cho đến khi
   `read_thread` set `prepared = 1` (sau khi `avformat_find_stream_info` đã
   chạy xong) hoặc `-1` (lỗi). Nhờ thế khi `magic_ffplay_open` trả về thì
   `Duration`, `VideoSize` đã hợp lệ.

### 2.2 Demux (`read_thread`)

1. `avformat_alloc_context` + `interrupt_callback = decode_interrupt_cb`
   (poll `is->abort_request` để hủy giữa các blocking I/O).
2. `avformat_open_input` → `avformat_find_stream_info`.
3. Set `discard = AVDISCARD_ALL` cho mọi stream, sau đó `av_find_best_stream`
   chọn 1 video + 1 audio rồi gọi `stream_component_open` (mở decoder, tạo
   thread).
4. **Signal `prep_cond`** để caller `magic_ffplay_open` thoát block.
5. Vòng main:
   - Theo dõi pause/resume → `av_read_pause` / `av_read_play`.
   - Phục vụ seek request: `avformat_seek_file` → flush cả audioq lẫn videoq
     → reset `extclk` về vị trí seek → bật lại `queue_attachments_req`.
   - Nếu `audioq.size + videoq.size > MAX_QUEUE_SIZE (15 MiB)` hoặc cả hai
     stream đã đủ packet (`stream_has_enough_packets`: ≥ 25 packet *và* ≥ 1s
     duration), **block** trên `continue_read_thread` 10ms → tránh đọc thừa.
   - Còn lại: `av_read_frame` → đẩy vào đúng `PacketQueue` theo
     `pkt->stream_index`.
   - EOF: đẩy 1 null packet vào mỗi queue để decoder flush ra frame cuối.

### 2.3 Decode video (`video_thread`)

1. `decoder_decode_frame` — wrapper quanh `avcodec_send_packet` /
   `avcodec_receive_frame`, xử lý `AVERROR(EAGAIN)` và serial-mismatch
   (trường hợp seek làm packet cũ trong queue bị invalidate).
2. **Framedrop early**: nếu `dpts - master_clock < 0` (frame trễ so với audio
   clock) thì drop trước cả khi chuyển NV12 → BGRA.
3. `queue_picture`:
   - Lấy slot writable trong `pictq`.
   - Nếu frame là `AV_PIX_FMT_D3D11` (luôn đúng vì
     `get_d3d11_format` ép codec dùng D3D11VA):
     - `data[0]` là `ID3D11Texture2D*` (NV12 array-texture).
     - `data[1]` là array slice index.
   - `frame_to_dxgi_color_space` đọc `color_trc / color_primaries /
     colorspace / color_range` để chọn `DXGI_COLOR_SPACE_TYPE` đúng (HDR PQ,
     HDR HLG, BT.2020, BT.601, hoặc default BT.709).
   - `frame_to_hdr10_metadata` đọc `AV_FRAME_DATA_MASTERING_DISPLAY_METADATA`
     + `AV_FRAME_DATA_CONTENT_LIGHT_LEVEL` cho HDR10.
   - `ffplay_gpu_ensure_processor` (re)create VideoProcessor khi size /
     color-space / metadata đổi.
   - `ffplay_gpu_nv12_to_bgra`:
     `CreateVideoProcessorInputView` (slice cụ thể) →
     `CreateVideoProcessorOutputView` (BGRA texture mới) →
     `VideoProcessorBlt`. Driver tự **tone-map** HDR → SDR khi input là PQ/HLG
     và output set `DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709`.
   - Stamp `vp->version = ++(*frame_version_seq)` (atomic counter).
   - `frame_queue_push` → tăng `pictq.windex` + signal cond.

### 2.4 Decode audio (`audio_thread` + `sdl_audio_callback`)

- `audio_thread` chạy decoder riêng, push `AVFrame` audio vào `sampq` (9 slot).
- SDL gọi `sdl_audio_callback` từ thread audio của OS bất cứ khi nào device
  cần data:
  1. `audio_decode_frame` lấy frame kế tiếp từ `sampq`.
  2. Nếu format/sample-rate/channel khác hardware → tạo `SwrContext` mới.
  3. `synchronize_audio` (chỉ active khi audio không phải master) — chèn /
     bỏ sample bù drift bằng `swr_set_compensation`, giới hạn ±10%
     (`SAMPLE_CORRECTION_PERCENT_MAX`).
  4. `swr_convert` → buffer S16 stereo (hoặc đa kênh).
  5. `memcpy` (full volume) hoặc `SDL_MixAudioFormat` (volume < max) ra
     `stream`.
  6. `set_clock_at(audclk, …)` — cập nhật audio master clock dùng
     `audio_callback_time` làm anchor; đồng thời `sync_clock_to_slave(extclk,
     audclk)` để external clock bám audio.

### 2.5 Refresh & A/V sync (`refresh_thread` + `video_refresh`)

- Vòng lặp đơn: tick mỗi `REFRESH_RATE = 10ms`, gọi `video_refresh`.
- `video_refresh` không vẽ — **chỉ điều khiển `pictq.rindex`**:
  1. `vp_duration(lastvp, vp)` — duration nominal = `nextpts - pts`, fallback
     là `vp->duration` (1/framerate).
  2. `compute_target_delay(delay, is)` — clamp delay theo
     `[AV_SYNC_THRESHOLD_MIN=40ms, AV_SYNC_THRESHOLD_MAX=100ms]`:
     - `diff = vidclk - master`.
     - `diff < -threshold`: video chậm → `delay = max(0, delay+diff)`
       (dồn frame).
     - `diff > +threshold` & `delay > AV_SYNC_FRAMEDUP_THRESHOLD=100ms`:
       `delay = delay + diff` (giữ frame lâu hơn).
     - `diff > +threshold` & `delay` nhỏ: `delay = 2*delay` (duplicate).
  3. Nếu chưa đến hạn → tính `remaining_time` cho `av_usleep` rồi return.
  4. Đến hạn → `update_video_pts` set `vidclk` + `frame_queue_next` (rời
     `lastvp` sang `vp`, mark `force_refresh`).
  5. **Framedrop late**: còn ≥1 frame nữa và `time > frame_timer + duration`
     → `frame_drops_late++`, drop tiếp.
- UI thread không cần xem `force_refresh`; nó chỉ poll
  `magic_ffplay_current_frame_version()` qua DispatcherTimer 60Hz.

### 2.6 Hiển thị (UI side, `MagicFFplayControl`)

- `Open`:
  - Dispose `_frameBitmap`, `_canvasDevice` cũ (vì player mới có D3D11 device
    mới).
  - `BindCanvasDeviceToPlayer`: `AcquireDxgiDevice` →
    `CreateDirect3D11DeviceFromDXGIDevice` →
    `CanvasDevice.CreateFromDirect3D11Device` →
    `FFplayCanvas.CustomDevice = …`. **Cùng D3D11 device** giữa player và
    Win2D → mọi `CanvasBitmap` wrap được trực tiếp, không cross-device copy.
- `Play`: `_presentTimer.Start()` (16.67ms), `Invalidate` lần đầu.
- `OnPresentTimerTick`: nếu `PeekFrameVersion() != _lastVersion` → `Invalidate`.
- `FFplayCanvas_Draw`:
  1. `TryAcquireCurrentTexture(out version, _, _)` — AddRef'd
     `ID3D11Texture2D*`.
  2. Nếu `version != _lastVersion` → `WrapTextureAsBitmap`:
     - `QueryInterface(IID_IDXGISurface)` trên texture.
     - `CreateDirect3D11SurfaceFromDXGISurface` → `IDirect3DSurface`.
     - `CanvasBitmap.CreateFromDirect3D11Surface` → CanvasBitmap mới.
  3. `DrawingSession.DrawImage(_frameBitmap, fullRect)` — Win2D scale + filter.

---

## 3. Constant & tunable

| Constant | Giá trị | Vai trò |
|---|---|---|
| `MAX_QUEUE_SIZE` | 15 MiB | Trần tổng size 2 PacketQueue trước khi `read_thread` block. |
| `MIN_FRAMES` | 25 | Ngưỡng "queue đủ" → demuxer ngừng đọc. |
| `EXTERNAL_CLOCK_MIN_FRAMES` | 2 | < ngưỡng này → external-clock giảm tốc 0.001/tick. |
| `EXTERNAL_CLOCK_MAX_FRAMES` | 10 | > ngưỡng này → external-clock tăng tốc. |
| `EXTERNAL_CLOCK_SPEED_{MIN,MAX,STEP}` | 0.900 / 1.010 / 0.001 | Khoảng kéo dãn external-clock. |
| `SDL_AUDIO_MIN_BUFFER_SIZE` | 512 sample | Buffer audio tối thiểu. |
| `SDL_AUDIO_MAX_CALLBACKS_PER_SEC` | 30 | Trần tần suất audio callback (dùng để chọn `samples`). |
| `AV_SYNC_THRESHOLD_MIN` / `MAX` | 0.04s / 0.10s | Cửa sổ "đồng bộ"; ngoài cửa sổ thì kéo / drop frame. |
| `AV_SYNC_FRAMEDUP_THRESHOLD` | 0.10s | Ngưỡng để duplicate frame (delay = delay+diff). |
| `AV_NOSYNC_THRESHOLD` | 10s | Vượt ngưỡng → bỏ qua sync (vd: gap PTS lớn). |
| `SAMPLE_CORRECTION_PERCENT_MAX` | 10 | Audio resampler chèn/bỏ tối đa ±10% sample. |
| `AUDIO_DIFF_AVG_NB` | 20 | Số mẫu trượt EMA để smooth audio drift. |
| `REFRESH_RATE` | 0.01s | Tick refresh_thread. |
| `VIDEO_PICTURE_QUEUE_SIZE` | 3 | Slot trong `pictq`. Đủ để overlap decode + display. |
| `SAMPLE_QUEUE_SIZE` | 9 | Slot trong `sampq`. |
| `s_framedrop` | -1 (auto) | -1: drop khi không phải video master; 0: tắt; 1: luôn drop. |
| `s_decoder_reorder_pts` | -1 (auto) | Dùng `best_effort_timestamp` thay vì `pkt_dts`. |
| `s_av_sync_type` | `AV_SYNC_AUDIO_MASTER` | Chế độ sync mặc định. |

---

## 4. Cấu trúc dữ liệu chính

### 4.1 `FfplayGpu` (per-instance GPU pipeline)

| Field | Mô tả |
|---|---|
| `device / context` | `ID3D11Device` + immediate context, tạo với `D3D11_CREATE_DEVICE_BGRA_SUPPORT \| D3D11_CREATE_DEVICE_VIDEO_SUPPORT`, `SetMultithreadProtected(TRUE)`. |
| `dxgiDevice` | Để wrap thành `IDirect3DDevice` cho Win2D. |
| `videoDevice / videoContext` | Mở video pipeline. |
| `videoContext2` | `ID3D11VideoContext2` (Win10 1607+) — cần cho `VideoProcessorSetStream/OutputColorSpace1` + `VideoProcessorSetStreamHDRMetaData`. |
| `enumerator / processor` | `ID3D11VideoProcessor` config NV12→BGRA. |
| `procW / procH` | Cache kích thước hiện tại để biết khi nào re-create. |
| `inColorSpace / outColorSpace / hasHdrMeta / hdrMeta` | Cache state, tránh re-apply mỗi frame. |
| `bltMutex` | Bao quanh `VideoProcessorBlt` + `CopyResource` — context không thread-safe khi nhiều caller cùng dùng. |
| `ffmpegLock` | `std::recursive_mutex`, gắn vào `AVD3D11VADeviceContext.lock/unlock` để FFmpeg sync khi share device. |

### 4.2 `Frame` (slot trong FrameQueue)

| Field | Mô tả |
|---|---|
| `frame` | `AVFrame*` gốc (audio: data, video: NV12 hw frame, vẫn giữ ref). |
| `serial` | Serial của packet → biết frame có thuộc về stream sau seek hay không. |
| `pts / duration / pos` | Thời gian. |
| `width / height / format / sar` | Hình học. |
| `tex` | **BGRA `ID3D11Texture2D*` đã AddRef** — cái UI sẽ wrap. Chỉ video. |
| `version` | Atomic stamp tăng dần khi frame mới được park. UI so sánh để skip rewrap. |

### 4.3 `VideoState` — gom toàn bộ runtime

Phân cụm:
- **Threads / control**: `read_tid`, `refresh_tid`, `abort_request`,
  `force_refresh`, `paused`, `last_paused`, `step`, `seek_*`,
  `continue_read_thread`, `prep_mutex/cond/prepared`.
- **Format / streams**: `ic`, `audio_st`, `video_st`,
  `audio_stream / video_stream` (index), `realtime`, `eof`, `filename`.
- **Clocks**: `audclk / vidclk / extclk`, `av_sync_type`,
  `max_frame_duration`, `frame_timer`.
- **Queues**: `videoq / audioq` (PacketQueue), `pictq / sampq` (FrameQueue).
- **Decoders**: `auddec / viddec`.
- **Audio runtime**: `audio_buf*`, `audio_buf_size/index`, `audio_clock`,
  `audio_diff_*`, `audio_src/audio_tgt`, `swr_ctx`, `audio_dev`,
  `audio_callback_time`, `audio_volume`, `muted`.
- **Frame drops counters**: `frame_drops_early / frame_drops_late`.
- **GPU**: `gpu (FfplayGpu*)`, `frame_version_seq (atomic<uint64>*)`.

### 4.4 `MagicFFplayHandle` (opaque handle public)

Wrapper bao quanh `VideoState*` cộng staging texture cho path
`magic_ffplay_copy_current_bgra`:

| Field | Mô tả |
|---|---|
| `is` | `VideoState*`. |
| `staging` | `D3D11_USAGE_STAGING + CPU_ACCESS_READ` — readback CPU. |
| `staging_w / staging_h` | Cache size; tạo lại khi resolution thay đổi. |
| `staging_mutex` | Tránh race khi UI gọi readback song song. |

---

## 5. Chức năng từng nhóm function

### 5.1 GPU pipeline (FfplayGpu)

| Function | Vai trò |
|---|---|
| `ffplay_gpu_create` | Tạo `ID3D11Device` (Hardware, BGRA+VIDEO, multithread-protected), query `IDXGIDevice / ID3D11VideoDevice / ID3D11VideoContext / ID3D11VideoContext2`. **Coi là fatal** nếu không có `VideoContext2` (cần cho HDR tone-map). |
| `ffplay_gpu_lock_cb / unlock_cb` | Callback gắn vào `AVD3D11VADeviceContext` để FFmpeg lock device khi giải mã. |
| `frame_to_dxgi_color_space` | Map `AVFrame->color_trc/primaries/colorspace/color_range` → `DXGI_COLOR_SPACE_TYPE`. PQ/HLG ưu tiên trước; sau đó BT.2020 / BT.601 / BT.709 (default), studio vs full. |
| `frame_to_hdr10_metadata` | Đọc `AV_FRAME_DATA_MASTERING_DISPLAY_METADATA` + `AV_FRAME_DATA_CONTENT_LIGHT_LEVEL`, scale primaries × 50000 và luminance theo đơn vị DXGI yêu cầu. |
| `ffplay_gpu_ensure_processor` | (Re)create processor khi size đổi, set `ColorSpace1` input/output khi color đổi, set HDR metadata khi metadata đổi. **Output luôn `DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709`** → driver tự tone-map. |
| `ffplay_gpu_nv12_to_bgra` | Tạo BGRA texture mới (`RENDER_TARGET \| SHADER_RESOURCE`), tạo input view bám `slice` của NV12 array, tạo output view, set source/dest/output rect, gọi `VideoProcessorBlt`. AddRef trước khi return. |

### 5.2 PacketQueue (verbatim từ ffplay)

| Function | Vai trò |
|---|---|
| `packet_queue_init` | `av_fifo_alloc2(AV_FIFO_FLAG_AUTO_GROW)` + SDL mutex/cond, mặc định abort=1 (chỉ bật bằng `start`). |
| `packet_queue_put / put_private / put_nullpacket` | Push (move-ref) packet, tăng `nb_packets / size / duration`, signal cond. |
| `packet_queue_get` | Block-pop 1 packet, trả về serial. |
| `packet_queue_flush` | Drain hết, **tăng `serial`** — frame với serial cũ sẽ bị decoder reject. |
| `packet_queue_abort` | Set `abort_request=1` + signal cond → unblock mọi consumer. |
| `packet_queue_start / destroy` | Start: set `abort_request=0`, tăng `serial`. Destroy: flush + free fifo + destroy cond/mutex. |

### 5.3 FrameQueue (verbatim, thêm release `tex`)

| Function | Vai trò |
|---|---|
| `frame_queue_init` | Pre-alloc `max_size` `AVFrame*` để không phải malloc giữa hot-path. |
| `frame_queue_unref_item` | `av_frame_unref` + `tex->Release()` (mới thêm). |
| `frame_queue_peek / peek_next / peek_last` | Không block. `peek = rindex + rindex_shown`; `peek_last = rindex` (frame đang hiển thị nếu `keep_last`). |
| `frame_queue_peek_writable / readable` | Block trên cond cho đến khi có slot. |
| `frame_queue_push / next` | Tiến `windex` / `rindex`, `next` còn release ref khi `keep_last` đã set. |
| `frame_queue_signal` | Signal cond (dùng khi abort để consumer thoát block). |
| `frame_queue_nb_remaining / last_pos` | Helper cho A/V sync logic. |

### 5.4 Clock

| Function | Vai trò |
|---|---|
| `init_clock` | speed=1, paused=0, gắn `queue_serial` (con trỏ đến serial PacketQueue tương ứng). |
| `get_clock` | Trả về `pts_drift + t - (t-last_updated)*(1-speed)`; NaN nếu serial mismatch (frame từ trước seek). |
| `set_clock / set_clock_at` | Update pts + drift baseline. |
| `set_clock_speed` | Đọc giá trị hiện tại trước rồi đổi speed → tránh nhảy. |
| `sync_clock_to_slave` | Nếu drift > `AV_NOSYNC_THRESHOLD` → snap master clock vào slave (extclk follow audclk/vidclk). |
| `get_master_sync_type` | Chọn master clock có thực: nếu mode chỉ định stream không có thì fallback sang clock khác. |
| `get_master_clock` | Đọc clock theo master type. |
| `check_external_clock_speed` | Buffer < min → giảm tốc 0.001; > max → tăng tốc; trong khoảng → kéo về 1.0. Chỉ chạy khi external master + realtime stream. |

### 5.5 Decoder

| Function | Vai trò |
|---|---|
| `decoder_init` | Alloc packet, gắn queue + `empty_queue_cond`. |
| `decoder_decode_frame` | Vòng `send_packet` / `receive_frame`; xử lý serial mismatch (flush khi serial nhảy), flush khi EOF, handle `AVERROR(EAGAIN)` API violation. Cho video, chọn `best_effort_timestamp` hoặc `pkt_dts` theo `s_decoder_reorder_pts`. Cho audio, rescale `pts` về `1/sample_rate` time-base + bám `next_pts` cho frame thiếu pts. |
| `decoder_destroy / abort` | Free packet + codec context. Abort: `packet_queue_abort` → `frame_queue_signal` → `SDL_WaitThread` → flush queue. |

### 5.6 Pause / Seek / Step

| Function | Vai trò |
|---|---|
| `stream_seek` | Set `seek_pos / seek_rel / seek_flags / seek_req=1`, signal `continue_read_thread`. Idempotent (skip nếu đã có seek pending). |
| `stream_toggle_pause` | Khi unpause: bù `frame_timer` cho khoảng thời gian pause; resync `vidclk`. Đồng bộ `paused` cho cả 3 clock. |
| `toggle_pause` | Wrapper: gọi `stream_toggle_pause` + reset `step=0`. |
| `step_to_next_frame` | Resume nếu đang pause + bật `step=1` → `video_refresh` sẽ tự pause lại sau đúng 1 frame. |

### 5.7 A/V sync

| Function | Vai trò |
|---|---|
| `compute_target_delay` | Như mục 2.5. Logic clamp + branching theo dấu/độ lớn `diff`. |
| `vp_duration` | Trả về `nextvp.pts - vp.pts` nếu hợp lệ, fallback `vp.duration`. Bảo vệ với `max_frame_duration`. |
| `update_video_pts` | `set_clock(vidclk, pts, serial)` + `sync_clock_to_slave(extclk, vidclk)`. |
| `synchronize_audio` | Smooth diff bằng EMA `audio_diff_cum`. Đợi đủ `AUDIO_DIFF_AVG_NB=20` mẫu rồi mới chèn/bỏ sample. Output clamp ±10%. |

### 5.8 Video pipeline

| Function | Vai trò |
|---|---|
| `get_video_frame` | `decoder_decode_frame` → `av_guess_sample_aspect_ratio` → framedrop early khi audio master + diff < 0. |
| `queue_picture` | Park frame vào `pictq` slot, release `tex` cũ (under `pictq.mutex` để không race với UI), gọi `ffplay_gpu_ensure_processor` + `ffplay_gpu_nv12_to_bgra`, stamp `version`. |
| `video_thread` | Loop `get_video_frame` → `queue_picture`. Duration từ `av_guess_frame_rate`. |
| `video_refresh` | Tiến `pictq.rindex` đúng deadline (mục 2.5). |

### 5.9 Audio pipeline

| Function | Vai trò |
|---|---|
| `audio_thread` | Loop decode → push `sampq`. |
| `audio_decode_frame` | Pop `sampq`, lazy (re)init `swr_ctx` khi format thay đổi; gọi `swr_convert` với `compensation` từ `synchronize_audio`. Cập nhật `audio_clock` theo `pts + nb_samples/sample_rate`. |
| `sdl_audio_callback` | Callback từ thread SDL audio: copy `audio_buf` ra `stream`, hoặc mix với volume; sau cùng cập nhật `audclk` đã trừ độ trễ buffer (`2 * audio_hw_buf_size + audio_write_buf_size`) → thời gian audio thực sự đang phát. |
| `audio_open` | Mở `SDL_OpenAudioDevice` với fallback giảm dần channels và sample-rate. Format luôn `AUDIO_S16SYS`. |

### 5.10 Hardware decode

| Function | Vai trò |
|---|---|
| `get_d3d11_format` | Callback `avctx->get_format`: luôn ép `AV_PIX_FMT_D3D11`. |
| `create_hwaccel` | `av_hwdevice_ctx_alloc(D3D11VA)` → gắn `device + lock + unlock + lock_ctx` → `av_hwdevice_ctx_init`. **Share device** với GPU pipeline. |

### 5.11 Stream lifecycle

| Function | Vai trò |
|---|---|
| `stream_component_open` | Alloc codec context, copy params, set `threads=auto`. Video: tạo hwaccel, ép `thread_count=1` (hw decode trong driver, không cần FFmpeg multi-thread). Audio: `audio_open`, set `audio_diff_threshold = audio_hw_buf_size / bytes_per_sec`, `SDL_PauseAudioDevice(0)`. Tạo decoder thread tương ứng. |
| `stream_component_close` | Abort decoder, close audio device / decoder, free `swr` + `audio_buf1`. |
| `stream_open` | Section 2.1. |
| `stream_close` | Set `abort_request=1`, wait read+refresh thread, close component, destroy queue, free `gpu` + `frame_version_seq`. |
| `read_thread / refresh_thread` | Section 2.2 / 2.5. |
| `decode_interrupt_cb` | Trả `is->abort_request` cho FFmpeg poll khi blocking I/O. |
| `is_realtime` | Detect rtp/rtsp/sdp/udp → ảnh hưởng `check_external_clock_speed`. |
| `stream_has_enough_packets` | ≥ 25 packet AND ≥ 1s duration → demuxer ngừng đọc. |

### 5.12 Public C API

| Function | Vai trò |
|---|---|
| `magic_ffplay_open` | `SDL_Init(audio+timer)` → `stream_open` → block đến `prepared`. Trả handle hoặc NULL. |
| `magic_ffplay_close` | `stream_close` + delete handle. |
| `magic_ffplay_toggle_pause` / `is_paused` | Wrapper. |
| `magic_ffplay_seek_us` | `av_rescale(us, AV_TIME_BASE, 1e6)` → `stream_seek`. |
| `magic_ffplay_step_frame` | `step_to_next_frame`. |
| `magic_ffplay_master_clock_us` | `get_master_clock * 1e6` (NaN → 0). |
| `magic_ffplay_duration_seconds` / `video_size` | Đọc từ `ic->duration` / `video_st->codecpar`. |
| `magic_ffplay_acquire_current_texture` | `pictq.rindex_shown ? AddRef tex` (under `pictq.mutex`). |
| `magic_ffplay_current_frame_version` | `pictq.rindex_shown ? .version` (under `pictq.mutex`). UI poll cheap. |
| `magic_ffplay_copy_current_bgra` | Lazy create staging texture cùng size, `CopyResource` GPU→staging, `Map(D3D11_MAP_READ)`, copy từng row (RowPitch có thể lớn hơn `w*4`), `Unmap`. |
| `magic_ffplay_acquire_dxgi_device` | `IDXGIDevice` của player → để Win2D bind. |

### 5.13 Wrapper / Façade / UI

| Layer | Class / Method | Mô tả |
|---|---|---|
| C++/CLI | `FFplayPlayer` (ref class) | Bao quanh `MagicFFplayHandle*` qua `magic_ffplay_*`. Chuyển `String^` ↔ `std::string` qua `pin_ptr`/marshal. |
| C# | `MagicStudio.FFmpegPlus.FFplayPlayer` | Façade thuần delegate đến wrapper, throw `ObjectDisposedException`, expose `Duration`, `VideoWidth/Height`, `Open/Play/Pause/Stop/Seek/StepFrame`, `TryAcquireCurrentTexture`, `PeekFrameVersion`, `AcquireDxgiDevice`. |
| UI | `MagicFFplayControl.Open` | Dispose bitmap + canvas device cũ, tạo player mới, **rebind CanvasDevice**. |
| UI | `BindCanvasDeviceToPlayer` | `AcquireDxgiDevice` → `CreateDirect3D11DeviceFromDXGIDevice` → `CanvasDevice.CreateFromDirect3D11Device` → set `CustomDevice`. |
| UI | `OnPresentTimerTick` | 60Hz; `Invalidate` chỉ khi `PeekFrameVersion() != _lastVersion`. |
| UI | `FFplayCanvas_Draw` | `TryAcquireCurrentTexture` → so version → `WrapTextureAsBitmap` (QI `IID_IDXGISurface` → `CreateDirect3D11SurfaceFromDXGISurface` → `CanvasBitmap.CreateFromDirect3D11Surface`) → `DrawImage`. |
| UI | `PerformSeek` | `_player.Seek(seconds)` + ép `_playing=true` + `Invalidate`. |
| UI | Slider drag | `PointerPressed` set `_sliderDragging=true`; `ValueChanged` chỉ seek nếu **không** đang drag (tránh thrashing); `PointerCaptureLost` mới seek đến giá trị cuối. |

### 5.14 Constant phía UI

| Constant | Mô tả |
|---|---|
| `IID_IDXGISurface` | `CAFCB56C-6AC3-4889-BF47-9E23BBD260EC`, dùng `QueryInterface` từ `ID3D11Texture2D` → `IDXGISurface` để Win2D wrap. |
| `PresentationInterval` | `1/60s` — cap polling rate (24/30/60fps không kéo monitor 144Hz vào repaint mỗi vsync). |
| `_lastVersion` | Last seen `frame_version_seq` — guard chống rewrap CanvasBitmap khi player chưa có frame mới. |

---

## 6. Kĩ thuật tối ưu performance đã áp dụng

### 6.1 Zero-copy GPU path

- **NV12 array-texture từ D3D11VA** không bao giờ rời GPU. `data[0]` của
  `AVFrame` là chính `ID3D11Texture2D*` mà driver decode ra; `data[1]` là
  array slice index → không phải copy ra surface mới.
- `VideoProcessorBlt` chuyển NV12→BGRA **trên GPU**, output là
  `ID3D11Texture2D*` BGRA mới với `BindFlags = RENDER_TARGET | SHADER_RESOURCE`.
- Win2D wrap qua `QueryInterface(IID_IDXGISurface)` →
  `CreateDirect3D11SurfaceFromDXGISurface` →
  `CanvasBitmap.CreateFromDirect3D11Surface`. **Không có path CPU**, không
  `memcpy`, không upload.
- **Cùng `ID3D11Device`** giữa decoder, video processor và `CanvasDevice` →
  texture không cross-device, Win2D không phải tạo shared handle.

### 6.2 Driver tone-map HDR → SDR

- `frame_to_dxgi_color_space` set input là `YCBCR_STUDIO_G2084_LEFT_P2020`
  (PQ) hoặc `GHLG_TOPLEFT_P2020` (HLG), output là
  `RGB_FULL_G22_NONE_P709`. Driver tự apply ST.2084 / HLG → linear → tone-map
  → BT.709 gamma encoding. Không cần shader custom.
- `VideoProcessorSetStreamHDRMetaData(HDR10)` — driver còn dùng
  mastering-display + content-light để chọn tone-mapping curve hợp lý.

### 6.3 Lazy / cached re-init

- `ffplay_gpu_ensure_processor` so size + color + metadata với cached
  values. Chỉ recreate processor khi **size** đổi; chỉ
  `SetStreamColorSpace1` / `SetOutputColorSpace1` khi **color** đổi; chỉ
  `SetStreamHDRMetaData` khi **metadata** đổi. Per-frame overhead chỉ là so
  sánh struct + `memcmp` nhỏ.
- `swr_ctx` audio chỉ recreate khi format/sample-rate/channel của frame mới
  khác cache trong `audio_src`.

### 6.4 Frame version + decoupled UI invalidate

- Atomic counter `frame_version_seq` (free-running). Tăng mỗi khi
  `queue_picture` park được texture mới.
- `magic_ffplay_current_frame_version` cheap (chỉ `pictq.mutex` ngắn).
- UI `DispatcherTimer` 60Hz **chỉ Invalidate** khi `PeekFrameVersion() !=
  _lastVersion` → 24fps video chạy trên monitor 144Hz vẫn chỉ ~24
  Invalidate/s.
- `Draw` callback so version trước khi rewrap `CanvasBitmap` → tránh
  alloc/release `IDXGISurface` không cần thiết.

### 6.5 Tách render loop khỏi decode loop

- `refresh_thread` chạy độc lập với UI thread. UI bận / window dispatcher
  block không làm A/V sync trượt.
- `pictq.rindex` tiến đúng nhịp PTS cho dù UI có vẽ hay không. Khi UI lấy
  `peek_last`, nó luôn nhận frame mới nhất cùng `version` đã tăng.

### 6.6 Hardware decode + multi-threading

- D3D11VA hwaccel: decode trong dedicated GPU engine (H.264/HEVC/VP9/AV1
  tùy GPU support).
- `avctx->thread_count = 1` cho video → tránh duplicate work với hw engine.
- Audio decode `threads=auto` (FFmpeg tự chọn).
- `AVD3D11VADeviceContext.lock/unlock` gắn vào `recursive_mutex` chung →
  share device giữa nhiều thread an toàn nhưng không over-serialize.

### 6.7 Multithread-protected D3D11

- `D3D10Multithread.SetMultithreadProtected(TRUE)` → driver tự lock context
  cho mọi caller. Cần thiết vì decode thread, video processor blt, UI
  thread (CanvasBitmap wrap) đều dùng cùng context.
- Thêm `bltMutex` trên `FfplayGpu` để serialize cụ thể `VideoProcessorBlt`
  + `CopyResource` (`Map`), nơi context được dùng intensive nhất.

### 6.8 Framedrop hai tầng

- **Early drop** (`get_video_frame`): bỏ frame trước khi NV12→BGRA — tiết
  kiệm cả `VideoProcessorBlt` cho frame quá trễ.
- **Late drop** (`video_refresh`): bỏ frame đã encode xong nhưng bị trễ so
  với deadline kế tiếp.
- `s_framedrop = -1` (auto): chỉ drop khi audio là master (case phổ biến).

### 6.9 Bounded queues + back-pressure

- `MAX_QUEUE_SIZE` 15 MiB + `stream_has_enough_packets` (≥25 packet & ≥1s
  duration) → `read_thread` block 10ms thay vì busy-loop.
- `frame_queue_peek_writable` block trên cond → `video_thread` /
  `audio_thread` không spin khi consumer chậm.

### 6.10 SDL audio with feedback to clock

- `audio_callback_time` được set ở đầu callback → `set_clock_at` dùng làm
  anchor. Audio clock phản ánh đúng thời điểm sample sắp được phát ra DAC.
- Trừ `2 * audio_hw_buf_size + audio_write_buf_size` bytes trong audio
  clock — tính cả độ trễ buffer SDL → video sync chính xác đến cỡ 1 frame.

### 6.11 Synchronous open handshake

- `magic_ffplay_open` block trên `prep_cond` cho đến khi `read_thread` set
  `prepared`. Caller nhận handle với `Duration` / `VideoSize` đã sẵn sàng,
  không cần polling vòng `while(player.Duration == 0)`.

### 6.12 UI — slider drag tiết kiệm seek

- Drag chỉ update `_seekTargetSeconds`, **không** seek mỗi tick.
- Seek chỉ chạy khi `PointerCaptureLost` (thả chuột) → tránh flush queue
  hàng chục lần khi user kéo.

### 6.13 Bitmap recycle qua version

- `_frameBitmap` chỉ Dispose + tạo mới khi **version** đổi. Repaint cùng
  frame (do vsync, resize, layout) không alloc lại bitmap.

---

## 7. Đề xuất cải tiến (ưu tiên Win2D)

> Mục tiêu: giảm CPU overhead, giảm memory churn, tăng độ smooth, tận dụng
> tối đa Win2D thay cho phần đang phải đi đường D3D11 raw.

### 7.1 Tái sử dụng BGRA texture pool thay vì alloc-per-frame

**Vấn đề hiện tại** — `ffplay_gpu_nv12_to_bgra` `CreateTexture2D` cho mỗi
frame; `CanvasBitmap.CreateFromDirect3D11Surface` cũng wrap mới mỗi lần
version đổi. Với 60fps × N giây sẽ tạo / drop cùng số texture + bitmap.

**Cải tiến** — duy trì **ring 3 BGRA texture** (đúng bằng
`VIDEO_PICTURE_QUEUE_SIZE`) và pool **3 `CanvasRenderTarget`** (Win2D RT có
thể vừa làm input cho `DrawImage`, vừa làm output cho `VideoProcessorBlt` qua
`GetSurface()`).

- `CanvasRenderTarget` API: `new CanvasRenderTarget(canvasDevice, w, h,
  96, DirectXPixelFormat.B8G8R8A8UIntNormalized, alphaMode)` → trong native
  có thể recover `ID3D11Texture2D*` qua `IDirect3DDxgiInterfaceAccess`.
- Native side viết thẳng vào texture của `CanvasRenderTarget` đó →
  Draw chỉ cần `DrawImage(rt, …)`, không tạo `CanvasBitmap` mới.

**Lợi**: bỏ alloc/free `ID3D11Texture2D` per frame, bỏ alloc/free
`CanvasBitmap` per frame, giảm GC pressure phía .NET, giảm fragmentation
GPU heap.

### 7.2 Dùng `CanvasImageSource` + dirty rect cho UI scaling

**Hiện tại** — `DrawImage(_frameBitmap, new Rect(0,0,actualW,actualH))` mỗi
Draw, Win2D scale toàn bộ khung hình bằng linear filter mặc định.

**Cải tiến** —
- Wrap render-target qua `CanvasImageSource` (target là XAML) → Win2D vẽ
  ngay vào back buffer XAML, không qua `CanvasControl.Draw` callback nữa.
- Khi video size = control size, set source rect = dest rect → hardware scaler
  của compositor xử lý scaling, Win2D không phải sample.
- Khi video size ≠ control size, đặt
  `CanvasImageInterpolation.HighQualityCubic` chỉ khi paused (frame tĩnh,
  user staring) và `Linear` khi playing → trade-off chất lượng / chi phí
  shader.

### 7.3 Đẩy NV12→BGRA vào `CanvasEffect` chain (full Win2D)

**Hiện tại** — chuyển color space hoàn toàn bằng `ID3D11VideoProcessor`.
Driver tone-map black box, không control được output curve.

**Cải tiến** — pipeline thuần Win2D:
- Wrap NV12 plane Y + UV thành 2 `CanvasBitmap` (R8 + R8G8) qua share
  texture.
- Dùng `PixelShaderEffect` (Win2D 1.x) hoặc `ShaderEffect` (Win2D 2.x +
  ComputeSharp): viết shader HLSL convert YUV→RGB + tone-map (Hable /
  Reinhard / BT.2390) tự kiểm soát.
- Compose chuỗi `BlendEffect` / `LinearTransferEffect` / `GammaTransferEffect`
  để apply gamma & gamut mapping.
- Output thành `CanvasRenderTarget` reuse.

**Lợi**: kiểm soát được tone-map curve (vd: cố định Hable cho cảnh sáng,
Reinhard cho cảnh tối), đồng nhất giữa các GPU vendor (driver Intel / AMD /
NVIDIA tone-map khác nhau), unit-test được bằng cách so output bitmap.

**Trade-off**: shader ops nhiều hơn fixed-function VP nhưng modern GPU dư sức,
4K@60 vẫn dưới 1ms/frame.

### 7.4 Dùng `Microsoft.UI.Composition` SwapChain thay `CanvasControl`

**Hiện tại** — `CanvasControl` chạy trên DirectComposition nhưng vẫn qua
XAML render thread. Mỗi `Invalidate` đi qua composition tree.

**Cải tiến** —
- Tạo riêng `IDXGISwapChain1` (flip-model, 2 buffer, BGRA) gắn vào
  `SpriteVisual` qua `Compositor.CreateSwapChainPanel`-style host.
- Native side `IDXGISwapChain::Present` ngay khi
  `magic_ffplay_acquire_current_texture` có frame mới → bypass UI thread.
- UI thread chỉ chịu trách nhiệm size change.

**Lợi**: latency frame → screen giảm 1-2 vsync (composition tree skip),
high-refresh monitor không bị giam ở 60Hz timer.

**Trade-off**: phức tạp khi mix với XAML overlay (label "MagicFFplay"). Phải
dùng `LayoutVisual` để stack UI lên swapchain visual. Mất một phần API
Win2D (effect chain trên `CanvasDrawingSession`).

### 7.5 Async readback với `ID3D11Query` thay vì `Map(BLOCK)`

**Hiện tại** — `magic_ffplay_copy_current_bgra` dùng `CopyResource` →
`Map(D3D11_MAP_READ)`. `Map` block CPU đến khi GPU copy xong.

**Cải tiến** —
- `ID3D11Query(D3D11_QUERY_EVENT)` sau `CopyResource` → poll bằng
  `GetData(D3D11_ASYNC_GETDATA_DONOTFLUSH)`.
- Hoặc giữ một queue 2-3 staging texture và `Map(D3D11_MAP_READ_NO_OVERWRITE)`
  trên cái nào sẵn sàng → caller chỉ block khi mọi staging đều bận.

**Lợi**: snapshot/screenshot không stall pipeline; thumbnail sidebar có thể
sample nhiều frame song song.

### 7.6 Cache `ffplay_gpu_ensure_processor` cho size không đổi

**Hiện tại** — code đã cache `procW/procH/inColorSpace/hdrMeta`. Nhưng khi
content có metadata dynamic (per-scene HDR — Dolby Vision frame metadata),
`metaChanged` true mỗi frame → `SetStreamHDRMetaData` mỗi frame.

**Cải tiến** —
- Hash 12 bytes của `DXGI_HDR_METADATA_HDR10` (skip nếu giống lần trước
  bằng so sánh hash 64-bit thay vì `memcmp` 24 bytes — micro-optim).
- Hoặc gộp metadata theo scene boundary (`AV_FRAME_DATA_DOVI_RPU_BUFFER`)
  thay vì set per-frame.

### 7.7 Win2D `CanvasVirtualImageSource` cho preview thumbnail strip

Khi cần thumbnail strip (timeline preview), `CanvasVirtualImageSource` cho
phép vẽ on-demand từng tile khi user scroll mà không cần render toàn bộ.

- Native expose API decode-at-pts (seek + 1 frame + return texture).
- UI tạo `CanvasVirtualImageSource(width = totalDuration*pixelsPerSec, …)`
  → `RegionsInvalidatedHandler` mới invoke decode chỉ những region scrolled
  tới.

### 7.8 GPU resize trong VideoProcessor thay vì Win2D scale

Hiện `DrawImage(rect)` Win2D scale từ video native size (vd 4K) xuống
control size. Nếu control nhỏ (preview window 540p), shader sample 4K
texture mỗi vsync.

**Cải tiến** — đặt `OutputWidth/Height` của `VideoProcessor` = control size
(làm tròn lên bội của 16). VP có hardware scaler riêng + filter chất
lượng cao (Bob, Edge, Spatial, Frame-rate). Output BGRA chỉ control-size →
DrawImage gần như free.

**Trade-off**: phải tái configure VP khi user resize cửa sổ → debounce 200ms
sau khi resize ngừng. Recreate VP chi phí ~1ms.

### 7.9 Intern `WrapTextureAsBitmap` qua `IDirect3DSurface` cache

`Marshal.QueryInterface` + `CreateDirect3D11SurfaceFromDXGISurface` không
miễn phí. Nếu kết hợp với 7.1 (texture pool 3 slot), cache `CanvasBitmap`
theo pointer texture → key (`IntPtr → CanvasBitmap`):

```csharp
private readonly Dictionary<IntPtr, CanvasBitmap> _bitmapCache = new(3);
```

Dispose bitmap cũ khi key bị evict (texture pool reuse cùng pointer).

### 7.10 Dùng `D3D11_USAGE_DYNAMIC + DISCARD` cho audio buffer

Không liên quan video nhưng có thể tham chiếu: `is->audio_buf1` hiện cấp
bằng `av_fast_malloc` (heap CPU). Nếu chuyển audio output sang
`IAudioClient3` (WASAPI shared mode low-latency) thay cho SDL, giảm
latency 20-40ms — quan trọng cho seek scrubbing đồng bộ với video preview.

### 7.11 Drop `SDL_INIT_TIMER` — dùng Win32 `CreateWaitableTimerEx`

`SDL_INIT_TIMER` chỉ phục vụ `av_usleep` qua SDL internals. Trên Windows
`CreateWaitableTimerEx(CREATE_WAITABLE_TIMER_HIGH_RESOLUTION)` (Win10
1803+) cho granularity 100ns → `refresh_thread` 10ms tick chính xác hơn.
Hiện `av_usleep` qua SDL trên Windows dùng `Sleep(0)` loop hoặc
`timeBeginPeriod(1)` (1ms granularity, side-effect global).

### 7.12 Detach `pictq.mutex` cho `current_texture` peek

`magic_ffplay_acquire_current_texture` lock `pictq.mutex` để bảo vệ vs
`queue_picture` release. Có thể dùng atomic shared_ptr (hoặc atomic
`ID3D11Texture2D*` + interlocked AddRef/Release) → UI peek không bao giờ
block decode thread.

```cpp
std::atomic<ID3D11Texture2D*> currentTex{nullptr};   // owning, AddRef'd
// publisher swap:
auto* old = currentTex.exchange(newTex);  if (old) old->Release();
// reader:
auto* t = currentTex.load();  if (t) t->AddRef();   // race-free? cần seqlock
```

Đúng pattern là **seqlock** hoặc `std::shared_ptr<ID3D11Texture2D>`
(`atomic_load`). Lock-free hoàn toàn nếu cần micro-latency.

### 7.13 Pre-warm CanvasDevice khi Open

`BindCanvasDeviceToPlayer` cấu hình CanvasDevice **sau** `_player.Open(path)`.
Có thể parallel: tạo device sync với open (cùng D3D11 device đã có ngay
sau `ffplay_gpu_create`). Open của player tự gọi `ffplay_gpu_create` trong
`stream_component_open` (video) — sau khi đã chuẩn bị header. Có thể
expose `magic_ffplay_init_gpu(handle)` chạy ngay đầu open để Win2D bind
device song song.

### 7.14 Telemetry hooks

Thêm 2-3 atomic counter expose ra C API:
- `frame_drops_early / frame_drops_late` (đã có trong VideoState).
- VP blt time (microseconds) — high-resolution timer quanh
  `VideoProcessorBlt`.
- Queue depth (`pictq.size`, `audioq.size`).

UI hiển thị overlay (toggle bằng phím) → debug perf nhanh hơn xé code log.

### 7.15 Hỗ trợ vsync-locked refresh thay vì 10ms timer

`refresh_thread` hardcode `REFRESH_RATE = 10ms`. Trên monitor 60Hz, 24fps
content sẽ có tick rơi lệch nhịp với frame deadline → có lúc 1 frame chờ 1
tick (16ms) thay vì 8ms.

**Cải tiến** — block trên `IDXGIOutput::WaitForVBlank` thay
`av_usleep(remaining_time)`. Khi remaining_time < 1 vsync interval thì
WaitForVBlank, trả về đúng sau vsync → frame_timer căn chuẩn vsync.

---

## 8. Tham khảo nguồn

| File | Mô tả |
|---|---|
| `MagicStudio.FFmpegPlus.Native/MagicFFplay.h` | Public C API. |
| `MagicStudio.FFmpegPlus.Native/MagicFFplay.cpp` | Implementation. |
| `MagicStudio.FFmpegPlus.Native/README.md` | Tài liệu cho `MagicFFmpegPlayer.cpp` (sibling). |
| `MagicStudio.FFmpegPlus.Wrapper/MediaPlayerWrapper.{h,cpp}` | C++/CLI ref class `FFplayPlayer`. |
| `MagicStudio.FFmpegPlus/FFplayPlayer.cs` | Façade C#. |
| `MagicStudio.UI/MagicFFplayControl.xaml{.cs}` | UI control. |

Upstream: `ffplay.c` (FFmpeg, LGPL v2.1+, Copyright (c) 2003 Fabrice
Bellard).
