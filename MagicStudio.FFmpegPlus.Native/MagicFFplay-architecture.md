# MagicFFplay — Overview Architecture

Sơ đồ tổng quan toàn bộ kiến trúc `MagicFFplay.cpp`. Tham chiếu chi tiết: [MagicFFplay.md](./MagicFFplay.md).

## 1. Component & data-flow overview

Layout 3 tầng từ trên xuống: **Caller / API** → **Bridge (Handle + SharedGpu)** → **Runtime**. Data pipeline trong VideoState đi một chiều trái sang phải. GPU và Clocks tách ra hai khối nhỏ bên cạnh để không cắt qua pipeline.

```mermaid
flowchart TB
    %% ============ TẦNG 1: CALLER + API ============
    UI["UI Thread (C# / WinUI)"]
    FCB["Frame-available handler"]

    subgraph API["Public C API"]
        direction LR
        LIFE["Lifecycle:<br/>open / close"]
        CTRL["Control:<br/>pause • seek • step<br/>speed • volume • mute"]
        OUT["Output:<br/>acquire_texture<br/>copy_to_texture<br/>copy_bgra"]
        REG["set_frame_callback"]
    end

    UI --> API

    %% ============ TẦNG 2: BRIDGE ============
    HANDLE["MagicFFplayHandle<br/>VideoState* + staging tex + mutex"]
    SGPU["FfplaySharedGpu<br/>ID3D11Device + bltMutex + refcount"]

    API --> HANDLE
    HANDLE -.refs.-> SGPU

    %% ============ TẦNG 3: RUNTIME PIPELINE ============
    subgraph RUNTIME["VideoState (data pipeline)"]
        direction LR
        FMT[("AVFormatContext")]
        RT["read_thread"]
        VQ[("videoq")]
        AQ[("audioq")]
        VT["video_thread"]
        AT["audio_thread"]
        PIQ[("pictq<br/>+ fp->tex")]
        SQ[("sampq")]
        RF["refresh_thread"]
        SDL["sdl_audio_callback<br/>OS thread"]

        FMT --> RT
        RT --> VQ --> VT --> PIQ --> RF
        RT --> AQ --> AT --> SQ --> SDL
    end

    HANDLE --> RUNTIME

    %% ============ TẦNG 4: GPU + CLOCK + DEVICES ============
    subgraph GPU["FfplayGpu"]
        direction TB
        VP["VideoProcessor<br/>NV12 to BGRA<br/>+ HDR tone-map"]
        POOL["BGRA pool<br/>3 slots"]
        VP --> POOL
    end

    subgraph CLK["Clocks"]
        direction TB
        ACK["audclk (master)"]
        VCK["vidclk"]
    end

    DAC(("Audio device"))

    SGPU --> VP
    RF -->|NV12| VP
    POOL -.park fp->tex.-> PIQ

    SDL --> ACK
    RF --> VCK
    ACK -.master.-> RF

    SDL --> DAC

    %% ============ OUTPUT & CALLBACK ============
    PIQ -->|AddRef tex| OUT
    POOL -->|GPU or CPU copy| OUT
    OUT --> UI

    RF -.fire on_frame_ready.-> FCB
    FCB --> UI
    REG -.register.-> RF

    %% ============ CONTROL SIDE-CHANNELS ============
    CTRL -.speed_changed flag.-> SDL
    CTRL -.seek_req / pause.-> RT

    %% ============ STYLES ============
    classDef thread fill:#e1f5ff,stroke:#0277bd,stroke-width:2px,color:#000
    classDef queue  fill:#fff4e1,stroke:#ef6c00,stroke-width:2px,color:#000
    classDef clock  fill:#f3e5f5,stroke:#6a1b9a,stroke-width:2px,color:#000
    classDef gpu    fill:#e8f5e9,stroke:#2e7d32,stroke-width:2px,color:#000
    classDef bridge fill:#fffde7,stroke:#f9a825,stroke-width:2px,color:#000
    class RT,VT,AT,RF,SDL thread
    class VQ,AQ,SQ,PIQ queue
    class ACK,VCK clock
    class VP,POOL,SGPU gpu
    class HANDLE bridge
```

**Cách đọc**:
- Đi từ trên xuống thấy **chuỗi sở hữu**: UI → API → Handle → VideoState.
- Đi từ trái sang phải trong `RUNTIME` thấy **luồng dữ liệu**: file → demuxer → packet queue → decoder → frame queue → consumer (refresh / SDL).
- GPU và Clocks là **dịch vụ phụ trợ** ở dưới, nối vào pipeline qua mũi tên ngắn.
- Đường nét đứt = control / side-channel (callback, flag, refcount). Đường nét liền = data flow chính.

## 2. Three output paths (cheat sheet)

```mermaid
flowchart LR
    SRC["fp->tex<br/>BGRA, BGRA pool slot<br/>(GPU, SharedGpu.device)"]

    SRC -- "AddRef only" --> P1["OUT1: acquire_current_texture<br/><i>zero-copy</i><br/>caller wraps as CanvasBitmap"]
    SRC -- "CopyResource<br/>GPU→GPU<br/>same device" --> P2["OUT2: copy_current_to_texture(dst)<br/><i>GPU-only copy</i><br/>N players → 1 canvas"]
    SRC -- "CopyResource → staging<br/>→ Map(READ) → memcpy" --> P3["OUT3: copy_current_bgra(dst_cpu)<br/><i>GPU→CPU readback</i><br/>screenshot / OCR / encode"]

    P1 --> UC1["1 player / 1 canvas"]
    P2 --> UC2["Quad/Dual canvas<br/>(SharedGpu required)"]
    P3 --> UC3["Snapshot, software<br/>processing, IPC"]

    classDef cheap fill:#e8f5e9,stroke:#2e7d32
    classDef mid   fill:#fff4e1,stroke:#ef6c00
    classDef heavy fill:#ffebee,stroke:#c62828
    class P1 cheap
    class P2 mid
    class P3 heavy
```

## 3. Thread / queue / clock matrix

```mermaid
flowchart LR
    %% Producers
    subgraph PROD["Producers"]
        direction TB
        RT2["read_thread"]
        VT2["video_thread"]
        AT2["audio_thread"]
        RF2["refresh_thread"]
    end

    %% Shared data
    subgraph DATA["Shared data (lock-protected)"]
        direction TB
        AQ2["audioq<br/>(q.mutex + cond)"]
        VQ2["videoq<br/>(q.mutex + cond)"]
        SQ2["sampq<br/>(f.mutex + 2 cond)"]
        PIQ2["pictq<br/>(f.mutex + 2 cond)<br/>guards fp->tex"]
        ACK2["audclk / vidclk / extclk<br/>(plain int, aligned atomic)"]
    end

    %% Consumers
    subgraph CONS["Consumers"]
        direction TB
        SDL2["sdl_audio_callback<br/>(OS thread)"]
        RF3["refresh_thread<br/>(also consumer of pictq)"]
        UI2["UI thread<br/>(via 3 output APIs)"]
    end

    RT2 -->|put pkt| AQ2
    RT2 -->|put pkt| VQ2
    VT2 -->|put frame| PIQ2
    AT2 -->|put frame| SQ2

    AQ2 --> AT2
    VQ2 --> VT2
    SQ2 --> SDL2
    PIQ2 --> RF3
    PIQ2 --> UI2

    RF3 -->|set_clock| ACK2
    SDL2 -->|set_clock_at| ACK2
    ACK2 --> RF3

    classDef p fill:#e1f5ff,stroke:#0277bd
    classDef d fill:#fff4e1,stroke:#ef6c00
    classDef c fill:#f3e5f5,stroke:#6a1b9a
    class RT2,VT2,AT2,RF2 p
    class AQ2,VQ2,SQ2,PIQ2,ACK2 d
    class SDL2,RF3,UI2 c
```

## 4. Cross-references

| Sơ đồ | Mục chi tiết trong MagicFFplay.md |
|---|---|
| Component & data-flow | §1 Tổng quan, §2.1 Mở file, §2.7 Video refresh, §2.8 UI consumer |
| Three output paths | §2.8 (bảng API + cost) |
| Thread / queue / clock matrix | §4 Threading model |
| Speed control box | §2.6 Audio filter chain, §2.10 Đổi speed |
| GPU subgraph (FfplayGpu / SharedGpu) | §1 Hai chế độ GPU, §3 Texture pool BGRA |
| Handle + staging | §6 Lifetime mô hình + struct `MagicFFplayHandle` |
