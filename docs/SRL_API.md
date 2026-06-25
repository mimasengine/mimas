# Saturn Ring Library (SRL) — API Reference

**Version**: 0.9.2 (January 11, 2026)
**GitHub**: https://github.com/ReyeMe/SaturnRingLib
**Online docs**: https://srl.reye.me/
**License**: No explicit license (copyright reserved by ReyeMe — contact before public release)

SRL is a C++23 wrapper around SEGA's SGL (Saturn Graphics Library).
All types live under the `SRL::` namespace.

---

## Installation (Windows)

```bash
git clone --recursive-submodules https://github.com/ReyeMe/SaturnRingLib.git
cd SaturnRingLib
setup_compiler.bat   # downloads sh2eb-elf-gcc 14.2 + xorrisofs + sox
```

**Toolchain**: `sh2eb-elf-gcc` 14.2.0 (GCC 14, C2x / C++23)
**Emulator**: Mednafen with `mpr-17933.bin` firmware in `firmware/`

---

## Repository Structure

```
SaturnRingLib/
├── saturnringlib/      # Main SRL headers + implementation
├── modules/
│   ├── sgl/           # SEGA SGL (underlying C library)
│   ├── SaturnMathPP/  # Fixed-point math (vectors, matrices)
│   └── tlsf/          # TLSF memory allocator
├── modules_extra/      # Optional extras
├── Samples/            # 46 example projects
├── tools/              # Compiler setup, ISO tools
├── shared.mk           # Master makefile (include in your project)
└── Documentation/      # Doxygen output + resources
```

**Main include**: `#include <srl.hpp>` (pulls in everything)

---

## Build System (shared.mk)

Include `shared.mk` from your project's Makefile:

```makefile
SRL_PATH = ../SaturnRingLib
include $(SRL_PATH)/shared.mk
```

### Configuration Variables

```makefile
SRL_MAX_TEXTURES             # Max VDP1 textures (default: 100)
SRL_MAX_CD_BACKGROUND_JOBS   # Async CD jobs (default: 1)
SRL_MAX_CD_FILES             # Max files open (default: 255)
SRL_FRAMERATE                # Target framerate
SRL_USE_SGL_SOUND_DRIVER     # Enable SCSP sound (1 = yes)
SRL_MALLOC_METHOD            # Memory allocator (default: TLSF)
```

### Build Targets

```
make all          # Compile + generate ISO
make clean        # Full cleanup
make clean_audio  # Cleanup preserving audio tracks
```

### Build Pipeline

1. Discovers `.c` and `.cxx` files in `src/`
2. Compiles with sh2eb-elf-gcc
3. ELF → raw binary
4. ISO via `xorrisofs`
5. Audio tracks converted via `sox` (MP3/WAV/FLAC/OGG → sector-aligned PCM)
6. CUE sheet generated

---

## Core Initialization & Synchronization

```cpp
namespace SRL::Core {
    Event<> OnAfterSync;     // After synchronize
    Event<> OnBeforeSync;    // Before synchronize
    Event<> OnVblank;        // Every V-blank

    // Initialize SRL, set background color
    void Initialize(const SRL::Types::HighColor& backColor = Colors::Black);

    // Wait for next frame (respects SRL_FRAMERATE)
    void Synchronize();
}
```

**Entry point**: `int main()` — SGL's `slInitSystem` is called internally by SRL.

---

## VDP2 — Background / Bitmap Screens

### Bitmap Screen

```cpp
// Template: ScreenType = NBG0/NBG1/RBG0, Id = screen id, On = default visible
SRL::VDP2::BmpScreen<ScreenType, Id, On>

// Key methods:
void LoadBitmap(SRL::Bitmap::IBitmap* bmp);
void Bmp2VRAM(void* bmpData, void* bmpAdr, BitmapInfo info);
void ScrollEnable() / ScrollDisable();
void SetOpacity(Math::Fxp opacity);
void SetPriority(SRL::VDP2::Priority pr);      // Layer0–Layer7
void TransparentEnable() / TransparentDisable();
```

### VRAM Management

```cpp
namespace SRL::VDP2::VRAM {
    enum VramBank { A0, A1, B0, B1 };
    enum Priority { Layer0, Layer1, ..., Layer7 };

    void* Allocate(size_t size, int boundary, VramBank bank, int cycles);
    void* AutoAllocateCell(TilemapInfo info, ScreenType screen);
    void* AutoAllocateBmp(BitmapInfo info, ScreenType screen, int size);
    int   GetAvailable(VramBank bank);   // Free bytes
    void  ClearVRAM();
}
```

### Color Offset / Visual Effects

```cpp
struct ColorOffset { int16_t Red, Green, Blue; };

void SetColorOffsetA(VDP2::ColorOffset& offset);
void SetColorOffsetB(VDP2::ColorOffset& offset);
void SetBackColor(const Types::HighColor& color);
```

---

## VDP1 — Sprites & 3D

### Textures

```cpp
namespace SRL::VDP1 {
    struct Texture {
        uint16_t Width, Height, Address, Size;
        void* GetData();
    };

    Texture Textures[SRL_MAX_TEXTURES];

    size_t   GetAvailableMemory();
    int32_t  TryLoadTexture(w, h, mode, palette, data);
    int32_t  TryLoadTexture(IBitmap* bitmap, Palette* palette);
    uint16_t GetTextureCount();
    void     ResetTextureHeap();
}
```

### 2D Sprites (Scene2D)

```cpp
enum FlipEffect   { NoFlip, HorizontalFlip, VerticalFlip };
enum SpriteEffect { Gouraud, HalfTransparent, Clipping, Flip, ... };
enum ZoomPoint    { TopLeft, Center, BottomRight, ... };

class Scene2D {
    void Draw();
    void DrawSprite(points, effects);
    void DrawSprite(rotation, scale, zoomPoint);
    void DrawLine(x1, y1, x2, y2, color, depth);
    void DrawPolygon(points, filled, color, depth);
    void SetClippingRectangle(x, y, w, h);
};
```

### 3D Meshes (Scene3D)

```cpp
struct Polygon  { Vector3D Normal; uint16_t Vertices[4]; };
struct MeshData { Vector3D* Vertices; Polygon* Faces; Attribute* Attributes; };
struct Mesh : public MeshData { Mesh(); Mesh(vertCount, faceCount); };

// Render
void DrawMesh(Mesh& mesh, bool useSlave = true);
void DrawSmoothMesh(SmoothMesh& mesh, Vector3D light);
void DrawOrthographicMesh(Mesh& mesh);

// Camera
void LookAt(eye, target, up);
void SetPerspective(angle);
void ProjectToScreen(Vector3D point3d, Vector2D& point2d);

// Transform stack
void PushMatrix() / PopMatrix();
void Rotate(axis, angle);
void Translate(x, y, z);
void Scale(x, y, z);
```

---

## Sound — SCSP Audio

### Initialization

```cpp
SRL::Sound::Hardware::Initialize();   // Called by SRL::Core::Initialize
```

### CDDA (CD Audio)

```cpp
class SRL::Sound::Cdda {
    static void Play(uint16_t fromTrack, uint16_t toTrack, bool loop);
    static void PlaySingle(uint16_t track, bool loop);
    static void Resume();
    static void StopPause();
    static void SetVolume(uint8_t volume);                 // 0-255
    static void SetVolume(uint8_t left, uint8_t right);
    static void SetPan(uint8_t left, uint8_t right);
};
```

### PCM Audio

```cpp
class SRL::Sound::Pcm {
    enum PcmChannels { Mono, Stereo };
    enum PcmBitDepth { Pcm8Bit, Pcm16Bit };

    // Load from CD file
    class RawPcm : public IPcmFile {
        RawPcm(Cd::File* file, PcmChannels ch, PcmBitDepth depth, uint16_t sampleRate);
    };

    // Load .WAV directly
    class WaveSound : public IPcmFile {
        WaveSound(const char* filename);
    };

    static int8_t Play(IPcmFile& pcm, uint8_t volume = 127, int8_t pan = 0);
    static bool   PlayOnChannel(IPcmFile& pcm, uint8_t ch, uint8_t vol, int8_t pan);
    static bool   StopSound(uint8_t channel);
    static bool   IsChannelFree(uint8_t channel);
    static void   SetVolumePan(uint8_t channel, uint8_t volume, int8_t pan);
};
```

---

## Input — SMPC Peripherals

### Digital Pad (Gamepad)

```cpp
class SRL::Input::Digital : public PeripheralGeneric {
    enum Button {
        Right, Left, Down, Up,
        START, A, B, C, X, Y, Z, R, L
    };

    Digital(uint8_t port);          // port 0 or 1

    bool IsHeld(Button button);     // held this frame
    bool WasPressed(Button button); // pressed this frame
    bool WasReleased(Button button);
};

// Global functions:
void SRL::Input::RefreshPeripherals();          // Call once per frame
bool SRL::Input::IsConnected(uint8_t port);
SRL::Input::PeripheralType SRL::Input::GetType(uint8_t port);
uint32_t SRL::Input::GetRawData(uint8_t port);
```

### Analog Pad

```cpp
class SRL::Input::Analog : public Digital {
    enum Axis { Axis1, Axis2, Axis3, Axis4, Axis5, Axis6 };
    uint8_t GetAxis(Axis axis);     // 0-255
};
```

---

## CD-ROM — File Operations

### Initialization & Navigation

```cpp
bool SRL::Cd::Initialize();                          // Init GFS / CD filesystem
int32_t SRL::Cd::ChangeDir(const char* name);        // Change directory

enum ErrorCode { ErrorOk, ErrorCDRD, ErrorTimeout, ErrorEOF, ... };
```

### File I/O

```cpp
struct SRL::Cd::File {
    // Construct with filename on current CD directory
    File(const char* name);

    bool Open();
    void Close();
    bool IsOpen();
    bool IsEOF();
    bool Exists();
    int32_t GetCurrentPosition();

    // Read operations
    int32_t Read(int32_t size, void* destination);          // Sequential
    int32_t ReadSectors(int32_t count, void* destination);  // Sector-aligned
    int32_t LoadBytes(size_t offset, int32_t size, void* destination); // Direct

    int32_t Seek(int32_t offset);
};
```

---

## Memory Management

```cpp
namespace SRL::Memory {
    enum Zones { HWRam, LWRam, CartRam, Default };

    void* Malloc(size_t size, Zones zone = Default);
    void  Free(void* ptr);
    void* Realloc(void* ptr, size_t size, Zones zone = Default);
    void* PlacementMalloc(size_t size, void* address);  // At fixed address

    class HighWorkRam {
        static void* Malloc(size_t size);
        static void  Free(void* ptr);
        static int32_t GetFreeSpace();
        static int32_t GetSize();         // 1MB
    };

    class LowWorkRam {
        // Same interface — 1MB at 0x00200000
        // WARNING: cannot be SCU DMA source
    };
}

// operator new/delete with zone selection
void* operator new(size_t size, SRL::Memory::Zones zone);
void operator delete(void* ptr);
```

---

## Slave SH-2

```cpp
namespace SRL::Slave {
    class ITask {
        virtual void Start()        = 0;   // Runs on slave CPU
        virtual bool IsDone()       = 0;   // Master polls this
        virtual bool IsRunning();
        virtual bool ResetTask();
    };

    void ExecuteOnSlave(ITask& task);
}
```

**Usage for Mimas column renderer**:
```cpp
class ColumnTask : public SRL::Slave::ITask {
    volatile bool done = false;
    void Start() override {
        // render all columns on slave
        done = true;
    }
    bool IsDone() override { return done; }
};
```

---

## System & Interrupts

```cpp
class SRL::System {
    enum InterruptType { VBlankIn, VBlankOut, HBlankIn, Timer0, Timer1, ... };

    static void SetInterruptHandler(InterruptType type, handler);
    static void SetInterruptMask(uint32_t mask);
    static void ChangeInterruptMask(uint32_t enable, uint32_t disable);
    static void Exit(int exitCode);
};

namespace SRL::Interrupt {
    enum Mask { None, VBlankIn, VBlankOut, HBlankIn, DspEnd, Pad, Dma0, ..., All };
    void SetMask(Mask mask);
    void ChangeMask(Mask enable, Mask disable);
}
```

---

## Timer / Frame Timing

```cpp
namespace SRL::Timer {
    class Timer {
        static uint64_t          DeltaTicks();
        static Math::Fxp         DeltaSeconds();
        static Math::Fxp         DeltaMilliseconds();
        static const Tickstamp&  CurrentTickstamp();
        static Tickstamp         Capture();
    };

    class Tickstamp {
        Math::Fxp ToMilliseconds();
        Math::Fxp ToSeconds();
        // Operators: + - == < > <= >=
    };
}
```

---

## Debugging

```cpp
class SRL::Debug {
    // Print at cell (x, y) — NBG0 text overlay
    static void Print(uint8_t x, uint8_t y, const char* text);
    template<class ...Args>
    static void Print(uint8_t x, uint8_t y, const char* text, Args...args);

    static void PrintClearLine(uint8_t line);
    static void PrintClearScreen();
    static void PrintColorSet(uint8_t color);

    #define Assert(msg, ...)   // Halts with assert screen
};

// Logging
void LogInfo(const char* message);
void LogWarning(const char* message);
void LogFatal(const char* message);
```

---

## Color Types

```cpp
struct SRL::Types::HighColor {     // 16-bit ABGR1555
    uint8_t Opaque : 1;
    uint8_t Blue   : 5;
    uint8_t Green  : 5;
    uint8_t Red    : 5;

    static HighColor FromRGB555(uint8_t r, uint8_t g, uint8_t b);
    static HighColor FromRGB24(uint8_t r, uint8_t g, uint8_t b);
    uint16_t GetABGR();

    struct Colors {
        static HighColor White, Black, Red, Green, Blue, Yellow;
    };
};
```

---

## Event System

```cpp
template<class ...Args>
class SRL::Event {
    Event& operator+=(void(*callback)(Args...));
    Event& operator-=(void(*callback)(Args...));
    void Invoke(Args...args);
};

// Usage:
SRL::Core::OnVblank += &my_vblank_fn;
```

---

## Display / TV

```cpp
class SRL::TV {
    static int16_t Width;       // e.g. 320
    static int16_t Height;      // e.g. 224/240
    static void TVOn();
    static void TVOff();
    // 24 resolution modes: 320/352/640/704 x 224/240/256 (normal + interlaced)
};
```

---

## Release History

| Version | Date | Notable changes |
|---------|------|-----------------|
| 0.9.2 | Jan 11, 2026 | Custom resolutions, Cinepak wrapper, VDP2 bitmap expansion, Scene2D expansion |
| 0.9.1 | Jun 13, 2025 | SRL logger, Scene3D enhancement, PCM efficiency, input reliability |
| 0.9   | Mar 28, 2025 | First public release |

---

## Sample Projects (46 total)

Key examples relevant to Mimas:

| Sample | Covers |
|--------|--------|
| Input - Gamepad | `SRL::Input::Digital`, button mapping |
| VDP2 - Layers | NBG0/NBG1 bitmap + scroll |
| Sound - CDDA | `SRL::Sound::Cdda::Play` |
| Sound - PCM | `SRL::Sound::Pcm::WaveSound` |
| CD - File reading | `SRL::Cd::File::LoadBytes` |
| SH2 - Slave | `SRL::Slave::ITask` dual-CPU |
| VDP1 - Sprites | Texture loading, 2D draw |
| VDP1 - 3D - Flat teapot | Full 3D pipeline |
