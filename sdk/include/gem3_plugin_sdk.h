#pragma once
/**
 * @file gem3_plugin_sdk.h
 * @brief Complete C++ Plugin & Hook SDK for Majesty 2 (1C / Ino-Co GEM 3 Engine)
 *
 * Reconstructed from PE Export/Import Tables, MSVC RTTI TypeDescriptors,
 * and VTable layouts across all 37 GEM 3 binaries:
 *   - base.dll       (xSet, xConsole::eWriter, xCrypt::eMD5, eIntrusiveRegistrator)
 *   - gembase.dll    (xConsole::ExecString, xConsole::ExecFile, xMod)
 *   - io.dll         (xIo::eStream, xIo::xRepository, xIo::xSdl::eReader/eWriter)
 *   - dda.dll        (xDda::eValue, xDda::eServer, xDda::eContext)
 *   - script.dll     (xScript::eAtom, xScript::eBase, xScript::eInstance)
 *   - geometry.dll   (Vec3, Quat, Matrix33, Matrix34, BoundBox)
 *   - scene.dll      (xScene, xEntity, xActor, xLand)
 *   - render.dll     (xRender, xFog, D3D9 device pipeline)
 *   - ui.dll         (xUi::eControl, xUi::eDialog)
 *   - Multiplayer.dll(Game::Multiplayer)
 *
 * Target ABI: 32-bit x86 MSVC 8.0 (Visual Studio 2005, MSVCR80.dll / MSVCP80.dll)
 */

#include <stdint.h>
#include <stddef.h>
#include <string>
#include <typeinfo>

#if defined(_MSC_VER)
  #define GEM3_API __declspec(dllimport)
  #define GEM3_EXPORT extern "C" __declspec(dllexport)
  #define GEM3_CXX_EXPORT __declspec(dllexport)
  #define GEM3_THISCALL __thiscall
  #define GEM3_CDECL __cdecl
  #define GEM3_FASTCALL __fastcall
#else
  #define GEM3_API
  #define GEM3_EXPORT extern "C" __attribute__((visibility("default")))
  #define GEM3_CXX_EXPORT __attribute__((visibility("default")))
  #define GEM3_THISCALL
  #define GEM3_CDECL
  #define GEM3_FASTCALL
#endif

// ============================================================================
// 1. Intrusive Registration System (eIntrusiveRegistrator)
// ============================================================================
// Used across base.dll, dda.dll, scene.dll, ui.dll, render.dll, traceable.dll
// to self-register console variables (xSet::eAbstract), script atoms, UI dialogs,
// and entity classes at DLL load time.

template <typename TBase, typename TKey = const char*>
class eIntrusiveRegInfo {
public:
    TKey   m_key;
    TBase* m_object;
};

template <typename TInfo, typename TBase = TInfo>
class eIntrusiveRegistrator {
public:
    struct eInstances {
        TInfo* m_head;
        TInfo* m_tail;
        size_t m_count;
    };

    class eDef {
    public:
        static eInstances& GEM3_CDECL Get();
    };

    TInfo* m_prev;
    TInfo* m_next;

    GEM3_API eIntrusiveRegistrator();
    GEM3_API ~eIntrusiveRegistrator();
    GEM3_API static eInstances& GEM3_CDECL Instances();
};

// ============================================================================
// 2. Engine Settings & Console Variables (xSet - base.dll)
// ============================================================================
// Mangled constructor in base.dll (RVA 0x000066C0):
//   ??0eAbstract@xSet@@QAE@PBDPBVtype_info@@@Z
// Mangled lookup in base.dll (RVA 0x00011640):
//   ?Search@xSet@@YAPAVeAbstract@1@PBDPBVtype_info@@@Z

namespace xSet {

class GEM3_API eAbstract : public eIntrusiveRegistrator<eAbstract, eAbstract> {
public:
    struct eItem {
        const void*      m_ptr;
        const type_info* m_type;
        const char*      m_name;
    };

    const char*      m_name;       // +0x08
    const type_info* m_typeInfo;   // +0x0C
    void*            m_valuePtr;   // +0x10

    eAbstract(const char* name, const type_info* type);
    eAbstract(const eAbstract& other);
    ~eAbstract();

    const char*      Name() const;
    const type_info* Type() const;

    void NotifyAdd(void (GEM3_CDECL* callback)());
    void NotifyAdd(void* ctx, void (GEM3_CDECL* callback)(void*));
    void NotifyRemove(void (GEM3_CDECL* callback)());
    void NotifyRemove(void* ctx);

protected:
    void OnChanged();
    void UseAdd(const void* ptr, const type_info* type, const char* name);
    void UseRemove(const void* ptr, const type_info* type);
};

template <typename T>
class eSet : public eAbstract {
public:
    T m_value;

    eSet(const char* name, const T& defaultVal)
        : eAbstract(name, &typeid(T)), m_value(defaultVal) {
        this->UseAdd(&m_value, &typeid(T), name);
    }

    ~eSet() {
        this->UseRemove(&m_value, &typeid(T));
    }

    operator const T&() const { return m_value; }
    eSet<T>& operator=(const T& val) {
        m_value = val;
        this->OnChanged();
        return *this;
    }
};

// Lookup any engine cvar by name (e.g. "r_shadows", "con_debugs", "sync_level")
GEM3_API eAbstract* GEM3_CDECL Search(const char* name, const type_info* type = nullptr);

} // namespace xSet

// ============================================================================
// 3. Virtual File System & Streams (xIo - io.dll)
// ============================================================================

namespace xIo {

enum eFileNameTag {};
enum eResourceNameTag {};

enum eSeekDir {
    eSeekBeg = 0,
    eSeekCur = 1,
    eSeekEnd = 2
};

enum eCompressLevel {
    eCompressNone    = 0,
    eCompressFast    = 1,
    eCompressDefault = 6,
    eCompressBest    = 9
};

template <typename TTag>
class eName {
public:
    std::string m_str;
};

typedef eName<eFileNameTag>     eFileName;
typedef eName<eResourceNameTag> eResourceName;

class GEM3_API eStream {
public:
    // Exact memory layout verified from io.dll (0x10014740..0x10015409):
    //   +0x00: vtable
    //   +0x04: int32_t  m_size
    //   +0x08: int32_t  m_position
    //   +0x0C: uint32_t m_flags
    //   +0x10: uint8_t* m_bufStart
    //   +0x14: uint8_t* m_bufCur
    //   +0x18: uint8_t* m_bufEnd
    int32_t  m_size;
    int32_t  m_position;
    uint32_t m_flags;
    uint8_t* m_bufStart;
    uint8_t* m_bufCur;
    uint8_t* m_bufEnd;

    eStream();
    virtual ~eStream();                                       // vtable[0]
    virtual void      Flush();                                // vtable[1] (+0x04)
    virtual void      Trunc();                                // vtable[2] (+0x08)
    virtual void      Skip(int32_t bytes);                    // vtable[3] (+0x0C)
    virtual eStream&  CopyFrom(eStream& src, int32_t count);  // vtable[4] (+0x10)
    virtual void      Overflow();                             // vtable[5] (+0x14)
    virtual void      Underflow();                            // vtable[6] (+0x18)

    eStream& Read(void* dst, int32_t size);
    eStream& Write(const void* src, int32_t size);
    eStream& Seek(int32_t offset, eSeekDir dir);
    int32_t  Position() const;
    int32_t  Size() const;
    int32_t  Left() const;
    bool     Eos() const;
    bool     IsOpen() const;

    uint8_t     GetByte();
    uint16_t    GetWord();
    uint32_t    GetDword();
    int32_t     GetInt();
    float       GetFloat();
    uint64_t    GetQword();
    std::string GetString();
    std::string GetLine(bool trimCr = true);

    uint8_t  LookByte();
    uint16_t LookWord();
    uint32_t LookDword();
    int32_t  LookInt();
};

class GEM3_API eFileStream : public eStream {
public:
    eFileName m_fileName; // +0x1C (28 bytes MSVC8 std::string)
    int32_t   m_fd;       // +0x38 (_sopen_s file descriptor, -1 when closed)

    eFileStream();
    virtual ~eFileStream();
    bool Open(const eFileName& path, int32_t mode, int32_t bufferSize = 0x8000);
    void Close();
};

class GEM3_API eMemoryStream : public eStream {
public:
    int32_t m_capacity; // +0x1C

    eMemoryStream(int32_t initialSize = 0x1000, int32_t growStep = -1);
    virtual ~eMemoryStream();
};

class GEM3_API eBufferStream : public eStream {
public:
    eBufferStream(const void* buf, uint32_t size);
    virtual ~eBufferStream();
    void Assign(const void* buf, uint32_t size);
    uint8_t* Buffer() const;
};

// Stealth Crypto Factory Hook in io.dll!
// Registered via ?PurgeEmpty@xFile@xSystem@xIo@@YAXABV?$eName@W4eFileNameTag@xIo@@@3@@Z
// (RVA 0x00014E80 in io.dll, sets global pointer @ 0x10029F50)
class ICryptoStreamFactory {
public:
    virtual eStream* Open(const eFileName& file, int32_t mode) = 0; // vtable[0]
    virtual void     Release(eStream* stream) = 0;                  // vtable[1]
};

namespace xRepository {
    enum FolderType {
        eFolderStandard = 0,
        eFolderMod      = 1,
        eFolderPatch    = 2
    };

    GEM3_API void GEM3_CDECL Mount(const eFileName& diskPath, const eResourceName& vfsPath, FolderType type);
    GEM3_API void GEM3_CDECL MountZip(const eFileName& pakPath, const eResourceName& vfsPath, FolderType type);
    GEM3_API void GEM3_CDECL Unmount(const eResourceName& vfsPath);
    GEM3_API bool GEM3_CDECL Exist(const eResourceName& vfsPath);
} // namespace xRepository

namespace xSdl {
    class GEM3_API eReader {
    public:
        eReader(eStream* stream);
        bool Open(const eResourceName& res, int32_t flags = 0);
        bool Opened() const;
        bool Start();
        bool End();
        const std::string& GetIdent(bool allowEmpty = false);
        const std::string& GetString();
        int32_t            GetInt();
        float              GetFloat();
        bool               GetBool();
    };

    class GEM3_API eWriter {
    public:
        bool Open(const eResourceName& res);
        bool Open(eStream* stream);
        bool Opened() const;
    };
} // namespace xSdl

GEM3_API eStream* GEM3_CDECL GetStream(const eResourceName& resource, int32_t mode = 1, int32_t bufSize = 0x8000);
GEM3_API void     GEM3_CDECL RunCacheThread();
GEM3_API void     GEM3_CDECL StopCacheThread();
GEM3_API void     GEM3_CDECL SetCacheThreadPriority(bool high);

} // namespace xIo

// ============================================================================
// 4. Data-Driven Attributes & Scripting (xDda / xScript / xConsole)
// ============================================================================

namespace xDda {
    class GEM3_API eValue {
    public:
        eValue();
        ~eValue();
    };

    class GEM3_API eServer {
    public:
        eServer();
        virtual ~eServer();
    };
} // namespace xDda

namespace xScript {
    class GEM3_API eAtom {
    public:
        eAtom();
        virtual ~eAtom();
    };

    class GEM3_API eBase {
    public:
        eBase();
    };

    class GEM3_API eFunction : public eAtom {
    public:
        eFunction();
        virtual ~eFunction();
    };
} // namespace xScript

namespace xConsole {
    enum eMode {
        eModeNormal  = 0,
        eModeWarning = 1,
        eModeError   = 2
    };

    struct eOutput {
        virtual void Print(const char* text) = 0;
    };

    GEM3_API void           GEM3_CDECL ExecString(const std::string& cmd, xDda::eValue* result = nullptr);
    GEM3_API void           GEM3_CDECL ExecFile(const xIo::eResourceName& file);
    GEM3_API xScript::eBase& GEM3_CDECL StaticBase();
    GEM3_API void           GEM3_CDECL Update();
    GEM3_API eOutput*       GEM3_CDECL GetDisplay();
    GEM3_API eOutput*       GEM3_CDECL GetLog();
} // namespace xConsole

// ============================================================================
// 5. 3D Math Primitives (geometry.dll) - Target for SIMD/SSE2 Acceleration
// ============================================================================
// Exported from geometry.dll (434 exports).
// Original binary uses heavy x87 FPU instructions (fld/fmul/faddp/fstp/fsin/fcos).

struct Vec2 {
    float x, y;
};

struct Vec3 {
    float x, y, z;
};

struct Vec4 {
    float x, y, z, w;
};

struct Quat {
    float x, y, z, w;
};

struct Matrix33 {
    Vec3 x, y, z; // 3x3 row-major rotation/scale matrix (36 bytes)
};

struct Matrix34 {
    Matrix33 m;   // +0x00: 3x3 rotation/scale
    Vec3     p;   // +0x24: translation vector (total 48 bytes)
};

struct BoundBox {
    Vec3 minPt;
    Vec3 maxPt;
};

// ============================================================================
// 6. Standard GEM 3 Plugin Entry Point
// ============================================================================
// Every GEM 3 plugin DLL (ps.dll, mss.dll, entity.ed.dll, features.ed.dll, rk_theora.dll)
// exports `void __cdecl DummyFunction(void)` (?DummyFunction@@YAXXZ) to force the
// dynamic linker to load the module and run all static `eIntrusiveRegistrator` constructors.

#define GEM3_DECLARE_PLUGIN() \
    GEM3_CXX_EXPORT void GEM3_CDECL DummyFunction() {}
