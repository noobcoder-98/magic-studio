# MagicFFplay — Sơ đồ tổng quan

> Tài liệu chi tiết: [MagicFFplay.md](MagicFFplay.md).
> File này tóm tắt các **component chính**, **thread chính**, và **khác biệt
> so với `ffplay.c` upstream** ở mức bản đồ tổng quan để hiểu nhanh cách
> MagicFFplay hoạt động.

---

## 1. Sơ đồ component

```mermaid
flowchart TB
    subgraph UI["UI Layer — WinUI3"]
        Ctl["MagicFFplayControl.xaml.cs"]
        Cv["CanvasControl (Win2D)"]
        DT["DispatcherTimer 60Hz<br/>poll version"]
        Sl["Slider / Play-Pause"]
        Ctl --- Cv
        Ctl --- DT
        Ctl --- Sl
    end

    subgraph CS["C# Façade"]
        FP["FFplayPlayer.cs"]
    end

    subgraph CLI["C++/CLI Wrapper"]
        WP["FFplayPlayer (ref class)<br/>MediaPlayerWrapper.cpp"]
    end

    subgraph Native["Native Core — MagicFFplay.cpp"]
        Handle["MagicFFplayHandle 🟢<br/>+ staging texture (BGRA readback)"]
        subgraph VS["VideoState (port từ ffplay)"]
            PQQ["PacketQueue × 2<br/>audioq / videoq"]
            FQQ["FrameQueue × 2<br/>sampq(9) / pictq(3, keep_last)"]
            Decs["Decoder × 2<br/>auddec / viddec"]
            Clks["Clock × 3<br/>audclk / vidclk / extclk"]
            Swr["SwrContext"]
            FVS["frame_version_seq 🟢<br/>atomic&lt;uint64&gt;"]
            Prep["prep_mutex/cond 🟢<br/>sync open handshake"]
        end
        subgraph Gpu["FfplayGpu 🟢 — per-instance"]
            Dev["ID3D11Device + context<br/>BGRA + VIDEO,<br/>multithread-protected"]
            VP["ID3D11VideoProcessor<br/>NV12 → BGRA<br/>+ tone-map HDR PQ/HLG → SDR"]
            Locks["bltMutex + ffmpegLock<br/>(AVD3D11VADeviceContext)"]
        end
        Handle --> VS
        VS --> Gpu
    end

    subgraph Ext["Dependencies"]
        FFmpeg["FFmpeg<br/>libavformat / libavcodec / libswresample"]
        D3DVA["D3D11VA hwaccel<br/>share device với FfplayGpu"]
        SDL["SDL2<br/>chỉ AUDIO + TIMER subsystem"]
    end

    Ctl -->|"AcquireDxgiDevice<br/>TryAcquireCurrentTexture<br/>PeekFrameVersion"| FP
    FP --> WP
    WP -->|"magic_ffplay_* C API"| Handle
    VS --> FFmpeg
    VS --> SDL
    Gpu -.->|share ID3D11Device| D3DVA
    FFmpeg -.->|hwaccel| D3DVA

    classDef new fill:#e8f5e9,stroke:#2e7d32,stroke-width:2px;
    class Handle,FVS,Prep,Gpu,Dev,VP,Locks new;
```

> 🟢 = component **không có trong ffplay gốc**, được thêm cho MagicFFplay.

Cùng `ID3D11Device` đi xuyên suốt: decoder D3D11VA → VideoProcessor →
`CanvasDevice` của Win2D. BGRA texture từ GPU đi thẳng sang `CanvasBitmap`
qua `IDXGISurface`, **zero-copy**, không qua CPU.

---

## 2. Sơ đồ thread & luồng data

```mermaid
flowchart LR
    subgraph RT["🧵 read_thread"]
        RT1["av_read_frame<br/>+ seek / pause / EOF<br/>+ back-pressure 10ms"]
    end

    subgraph Pkts["PacketQueue"]
        AQ[("audioq")]
        VQ[("videoq")]
    end

    subgraph AT["🧵 audio_thread"]
        AT1["decode audio"]
    end

    subgraph VT["🧵 video_thread"]
        VT1["decoder_decode_frame"]
        VT2["framedrop early<br/>(diff &lt; 0)"]
        VT3["NV12 → BGRA<br/>VideoProcessorBlt<br/>+ HDR tone-map"]
        VT4["stamp version<br/>frame_queue_push"]
        VT1 --> VT2 --> VT3 --> VT4
    end

    subgraph Frms["FrameQueue"]
        SQ[("sampq<br/>9 slot")]
        PIQ[("pictq<br/>3 slot, keep_last<br/>+ tex BGRA<br/>+ version")]
    end

    subgraph RFT["🧵 refresh_thread 🟢"]
        RFT1["10ms tick<br/>video_refresh:<br/>A/V sync,<br/>tiến rindex,<br/>framedrop late"]
    end

    subgraph AC["🧵 SDL audio callback<br/>(WASAPI thread)"]
        AC1["audio_decode_frame"]
        AC2["swr_convert"]
        AC3["synchronize_audio"]
        AC4["set_clock_at(audclk)<br/>⭐ MASTER CLOCK"]
        AC1 --> AC2 --> AC3 --> AC4
    end

    subgraph UIT["🧵 UI thread — XAML"]
        UI1["DispatcherTimer 60Hz<br/>PeekFrameVersion()<br/>≠ lastVersion?"]
        UI2["FFplayCanvas_Draw<br/>TryAcquireCurrentTexture<br/>→ wrap CanvasBitmap<br/>→ DrawImage"]
        UI1 -.invalidate.-> UI2
    end

    RT1 -->|push pkt| AQ
    RT1 -->|push pkt| VQ
    AQ -->|pop| AT1
    VQ -->|pop| VT1
    AT1 -->|push| SQ
    VT4 -->|push| PIQ
    SQ -->|pop| AC1
    PIQ -.peek_last.-> RFT1
    RFT1 -.update vidclk<br/>tiến rindex.-> PIQ
    PIQ -.peek_last.-> UI2
    AC4 -.audio master<br/>clock feedback.-> RFT1
    RT1 -.continue_read<br/>cond wake.-> RT1

    classDef new fill:#e8f5e9,stroke:#2e7d32,stroke-width:2px;
    class RFT,RFT1 new;
```

### Tổng số thread native do MagicFFplay quản lý

| # | Thread | Tạo bởi | Vai trò |
|---|---|---|---|
| 1 | `read_thread` | `SDL_CreateThread` trong `stream_open` | Demux: `av_read_frame` → `PacketQueue`. Phục vụ seek, pause/resume, back-pressure. |
| 2 | `video_thread` | `SDL_CreateThread` trong `stream_component_open` | Decode video → framedrop early → NV12→BGRA `VideoProcessorBlt` → push `pictq` + stamp version. |
| 3 | `audio_thread` | `SDL_CreateThread` trong `stream_component_open` | Decode audio → push `sampq`. |
| 4 | `refresh_thread` 🟢 | `SDL_CreateThread` trong `stream_open` | A/V sync chuyên dụng: 10ms tick, `video_refresh` tiến `pictq.rindex` theo PTS deadline, framedrop late. |
| 5 | SDL audio callback | SDL audio backend (Windows: WASAPI thread) | Pull sample từ `sampq`, mix theo volume, **set audio master clock**. |

UI thread (XAML) tách hoàn toàn khỏi native, chỉ poll `frame_version_seq`
qua C API.

---

## 3. Khác biệt so với FFplay gốc (`ffplay.c` upstream)

### 3.1 Đối chiếu thread side-by-side

```mermaid
flowchart TB
    subgraph FF["FFplay gốc (ffplay.c)"]
        F1["🧵 Main thread<br/>event_loop +<br/>refresh_loop_wait_event<br/>(SDL_PollEvent + refresh ghép)"]
        F2["🧵 read_thread"]
        F3["🧵 video_thread"]
        F4["🧵 audio_thread"]
        F5["🧵 subtitle_thread"]
        F6["🧵 SDL audio callback"]
    end

    subgraph MG["MagicFFplay"]
        M1["🧵 UI thread — WinUI3 XAML<br/>(THAY event_loop, ngoài native)"]
        M2["🧵 read_thread"]
        M3["🧵 video_thread<br/>+ NV12→BGRA GPU"]
        M4["🧵 audio_thread"]
        M5["🧵 refresh_thread<br/>(TÁCH MỚI)"]
        M6["🧵 SDL audio callback"]
    end

    F1 -.->|"⇒ thay bằng UI thread"| M1
    F2 -.->|"✓ giữ nguyên"| M2
    F3 -.->|"+ thêm GPU convert"| M3
    F4 -.->|"✓ giữ nguyên"| M4
    F5 -.->|"❌ BỎ"| X(("BỎ"))
    F6 -.->|"✓ giữ nguyên"| M6
    M5 -.->|"🟢 TÁCH RIÊNG<br/>từ refresh_loop_wait_event"| M5

    classDef removed fill:#ffebee,stroke:#c62828,stroke-width:2px;
    classDef added fill:#e8f5e9,stroke:#2e7d32,stroke-width:2px;
    class F5,X removed;
    class M5 added;
```

**Tổng kết thread**: MagicFFplay **bớt 1 thread** (subtitle), **tách 1 thread
mới** (`refresh_thread`), **bỏ main event loop native** (UI XAML thay thế).

### 3.2 Component diff

```mermaid
flowchart LR
    subgraph Removed["❌ BỎ ĐI"]
        R1["SDL video subsystem<br/>SDL_Window + Renderer + Texture"]
        R2["Subtitle pipeline<br/>subq + subtitle_thread + SubPicture"]
        R3["libavfilter graph<br/>video & audio filters"]
        R4["Main event_loop<br/>+ refresh_loop_wait_event"]
        R5["RDFT audio visualization"]
        R6["upload_texture<br/>video_image_display<br/>video_open<br/>set_default_window_size"]
        R7["SDL_INIT_VIDEO"]
    end

    subgraph Added["🟢 THÊM MỚI"]
        A1["FfplayGpu<br/>ID3D11Device + VideoProcessor<br/>NV12→BGRA + HDR tone-map"]
        A2["refresh_thread<br/>tách thành thread riêng"]
        A3["prep_cond / prepared<br/>synchronous open handshake"]
        A4["frame_version_seq<br/>atomic counter cho UI poll"]
        A5["Frame::tex<br/>BGRA ID3D11Texture2D in slot"]
        A6["MagicFFplayHandle<br/>+ staging readback"]
        A7["C++/CLI Wrapper + C# Façade"]
    end

    subgraph Modified["✏️ SỬA MẠNH"]
        M1["Hwaccel: BẮT BUỘC D3D11VA<br/>get_d3d11_format ép pix_fmt<br/>thread_count = 1"]
        M2["queue_picture: convert<br/>NV12→BGRA NGAY khi decode<br/>(gốc: upload lúc display)"]
        M3["Frame slot lifecycle:<br/>+ tex->Release() khi unref"]
        M4["Color / HDR:<br/>PQ / HLG / BT.2020 mapping<br/>+ HDR10 metadata"]
    end

    classDef removed fill:#ffebee,stroke:#c62828;
    classDef added fill:#e8f5e9,stroke:#2e7d32;
    classDef modified fill:#fff8e1,stroke:#f57f17;
    class R1,R2,R3,R4,R5,R6,R7 removed;
    class A1,A2,A3,A4,A5,A6,A7 added;
    class M1,M2,M3,M4 modified;
```

### 3.3 Pipeline data video — so sánh

```mermaid
flowchart TB
    subgraph FFPipe["FFplay gốc — path video"]
        FP1["AVPacket"]
        FP2["decoder (sw / hw)"]
        FP3["AVFrame (YUV)"]
        FP4["libavfilter graph<br/>scale / format / eq"]
        FP5["AVFrame (YUV/RGB)"]
        FP6["pictq slot (AVFrame only)"]
        FP7["upload_texture<br/>⚠️ CPU memcpy / GPU upload"]
        FP8["SDL_RenderCopy<br/>SDL_RenderPresent"]
        FP1 --> FP2 --> FP3 --> FP4 --> FP5 --> FP6 --> FP7 --> FP8
    end

    subgraph MGPipe["MagicFFplay — path video"]
        MP1["AVPacket"]
        MP2["decoder D3D11VA"]
        MP3["AVFrame NV12 trên<br/>ID3D11Texture2D + slice idx"]
        MP4["video_thread:<br/>ffplay_gpu_nv12_to_bgra<br/>VideoProcessorBlt GPU<br/>+ HDR tone-map"]
        MP5["BGRA ID3D11Texture2D<br/>RT + SR, AddRef'd"]
        MP6["pictq slot<br/>+ tex + version"]
        MP7["refresh_thread:<br/>tiến rindex theo PTS<br/>(KHÔNG render)"]
        MP8["UI thread:<br/>QI IDXGISurface →<br/>CanvasBitmap →<br/>DrawImage<br/>✅ zero-copy"]
        MP1 --> MP2 --> MP3 --> MP4 --> MP5 --> MP6 --> MP7 --> MP8
    end

    classDef cpu fill:#ffebee,stroke:#c62828;
    classDef gpu fill:#e8f5e9,stroke:#2e7d32;
    class FP7 cpu;
    class MP4,MP5,MP8 gpu;
```

Khác biệt then chốt:
- **MagicFFplay không có CPU memcpy** ở bất kỳ điểm nào trên đường video
  (zero-copy GPU thuần).
- **Tone-map HDR** do driver thực hiện trong `VideoProcessorBlt`; FFplay
  gốc không hỗ trợ HDR tone-map.
- **Convert được làm tại video_thread**, không phải tại display time →
  giảm jitter render, UI chỉ wrap + draw.

### 3.4 Bảng đối chiếu component chi tiết

| Mục | FFplay gốc | MagicFFplay |
|---|---|---|
| **Display backend** | SDL window + renderer + streaming texture | Win2D `CanvasControl` qua `CanvasBitmap` wrap `ID3D11Texture2D` |
| **Hwaccel** | Optional (`-hwaccel`), mặc định software | **Bắt buộc D3D11VA**, ép `AV_PIX_FMT_D3D11` |
| **Color / HDR** | BT.601/709 cơ bản, không HDR | Đầy đủ PQ / HLG / BT.2020 / BT.601 / BT.709, HDR10 metadata, tone-map qua VP |
| **Filter graph** | `libavfilter` cho video + audio | Bỏ — color qua VP, audio qua `libswresample` trực tiếp |
| **Subtitle** | `subtitle_thread` + `subq` + render overlay | Bỏ hoàn toàn |
| **Main loop** | `event_loop` + `refresh_loop_wait_event` chung | Bỏ native; UI XAML thay thế; `refresh_thread` tách riêng |
| **Visualization** | RDFT waveform / spectrum khi no-video | Bỏ |
| **SDL init** | `AUDIO \| VIDEO \| TIMER` | Chỉ `AUDIO \| TIMER` |
| **Frame slot** | `AVFrame*` (render lúc display) | `AVFrame*` + **BGRA `ID3D11Texture2D*` AddRef'd** + version |
| **Open handshake** | Trả về ngay, format info bất đồng bộ | **Synchronous** qua `prep_cond` — handle có `Duration`/`VideoSize` ngay |
| **Frame notify UI** | SDL event push | `frame_version_seq` atomic, UI poll cheap qua C API |
| **GPU device sharing** | N/A | Cùng `ID3D11Device` giữa decoder, VP, Win2D |
| **PacketQueue/FrameQueue/Clock/Decoder/Sync logic** | — | Port verbatim, chỉ thêm hook `tex->Release()` |

---

## 4. Tham chiếu

- Chi tiết từng function, constant, optimization: [MagicFFplay.md](MagicFFplay.md).
- Source: [MagicFFplay.cpp](MagicFFplay.cpp) + [MagicFFplay.h](MagicFFplay.h).
- Upstream: `ffplay.c` (FFmpeg, LGPL v2.1+).

> 💡 Nếu cần đối chiếu trực tiếp với `ffplay.c` upstream (vd để bổ sung
> subtitle thread state machine, RDFT path chi tiết, hay danh sách option
> CLI bỏ đi) thì gửi file qua — sẽ refine thêm.
