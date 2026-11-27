// Main.cpp — TexMod: дамп и подмена текстур через il2cpp API
// Точка входа: конструктор lib_main (без привязки к smali)
// Контракт с меню: GetFeatureList / Changes — сигнатуры должны совпадать с Menu.hpp

#include <jni.h>
#include <string>
#include <vector>
#include <set>
#include <cstring>
#include <cstdarg>
#include <cctype>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <thread>
#include <chrono>
#include <dlfcn.h>
#include <android/log.h>

#include "Includes/obfuscate.h"
#include "Includes/Utils.hpp"
#include "Menu/Menu.hpp"

#ifndef ANDROID_LOG_INFO
#define ANDROID_LOG_INFO 4
#endif

#define targetLibName OBFUSCATE("libil2cpp.so")

#define DUMP_DIR  "/storage/emulated/0/texdump"
#define APPLY_DIR "/storage/emulated/0/texmod"
#define LOG_PATH  "/storage/emulated/0/texmod_log.txt"

// ============================== лог ==============================
static FILE* g_logFile = nullptr;
static std::vector<std::string> g_logBuf;

static void LogFlushBuffer() {
    if (!g_logFile) return;
    for (size_t i = 0; i < g_logBuf.size(); i++) {
        fputs(g_logBuf[i].c_str(), g_logFile);
        fputc('\n', g_logFile);
    }
    fflush(g_logFile);
    g_logBuf.clear();
}

static void TxLog(const char* fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    __android_log_print(ANDROID_LOG_INFO, "TEXMOD", "%s", line);

    g_logBuf.push_back(line);
    while (g_logBuf.size() > 1024) g_logBuf.erase(g_logBuf.begin());

    if (!g_logFile) {
        FILE* f = fopen(LOG_PATH, "a");
        if (f) {
            g_logFile = f;
            time_t t = time(nullptr);
            struct tm lt;
            localtime_r(&t, &lt);
            char ts[32];
            strftime(ts, sizeof(ts), "%H:%M:%S", &lt);
            fprintf(g_logFile, "===== launch %s =====\n", ts);
            g_logBuf.push_back("storage access OK");
        }
    }
    LogFlushBuffer();
}

static void TxLogTs(const char* msg) {
    time_t t = time(nullptr);
    struct tm lt;
    localtime_r(&t, &lt);
    char ts[32];
    strftime(ts, sizeof(ts), "%H:%M:%S", &lt);
    TxLog("%s %s", ts, msg);
}

// ============================== png encoder (stored blocks) ==============================
static uint32_t g_crcTable[256];
static bool g_crcReady = false;

static void CrcInit() {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        g_crcTable[i] = c;
    }
    g_crcReady = true;
}

static uint32_t CrcUpdate(uint32_t crc, const uint8_t* d, size_t n) {
    if (!g_crcReady) CrcInit();
    for (size_t i = 0; i < n; i++) crc = g_crcTable[(crc ^ d[i]) & 0xFFu] ^ (crc >> 8);
    return crc;
}

static void AdlerUpdate(uint32_t* s1, uint32_t* s2, const uint8_t* d, size_t n) {
    for (size_t i = 0; i < n; i++) {
        *s1 = (*s1 + d[i]) % 65521u;
        *s2 = (*s2 + *s1) % 65521u;
    }
}

static void WriteBe32(FILE* f, uint32_t v) {
    uint8_t b[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };
    fwrite(b, 1, 4, f);
}

static void WriteChunk(FILE* f, const char* type, const uint8_t* data, size_t len) {
    WriteBe32(f, (uint32_t)len);
    fwrite(type, 1, 4, f);
    if (len && data) fwrite(data, 1, len, f);
    uint32_t crc = CrcUpdate(0xFFFFFFFFu, (const uint8_t*)type, 4);
    if (len && data) crc = CrcUpdate(crc, data, len);
    crc ^= 0xFFFFFFFFu;
    WriteBe32(f, crc);
}

// PNG с несжатыми stored-блоками deflate: RGBA (w*h*4) -> валидный png
static bool WritePngStored(FILE* f, int w, int h, const uint8_t* rgba) {
    static const uint8_t sig[8] = { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A };
    fwrite(sig, 1, 8, f);

    uint8_t ihdr[13];
    ihdr[0] = (uint8_t)(w >> 24); ihdr[1] = (uint8_t)(w >> 16);
    ihdr[2] = (uint8_t)(w >> 8);  ihdr[3] = (uint8_t)w;
    ihdr[4] = (uint8_t)(h >> 24); ihdr[5] = (uint8_t)(h >> 16);
    ihdr[6] = (uint8_t)(h >> 8);  ihdr[7] = (uint8_t)h;
    ihdr[8] = 8;  // bit depth
    ihdr[9] = 6;  // RGBA
    ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    WriteChunk(f, "IHDR", ihdr, 13);

    size_t rowSize = 1 + (size_t)w * 4;
    size_t payload = rowSize * (size_t)h;
    size_t numBlocks = (payload + 65534) / 65535;
    uint32_t idatLen = (uint32_t)(2 + payload + 5 * numBlocks + 4);

    WriteBe32(f, idatLen);
    fwrite("IDAT", 1, 4, f);
    uint32_t crc = CrcUpdate(0xFFFFFFFFu, (const uint8_t*)"IDAT", 4);

    const uint8_t zhdr[2] = { 0x78, 0x01 };
    fwrite(zhdr, 1, 2, f);
    crc = CrcUpdate(crc, zhdr, 2);

    std::vector<uint8_t> row(rowSize);
    uint32_t s1 = 1, s2 = 0;

    for (int y = 0; y < h; y++) {
        row[0] = 0; // filter type 0
        memcpy(&row[1], rgba + (size_t)y * (size_t)w * 4, (size_t)w * 4);
        AdlerUpdate(&s1, &s2, row.data(), row.size());

        size_t offset = 0;
        while (offset < rowSize) {
            size_t blen = rowSize - offset;
            if (blen > 65535) blen = 65535;
            bool last = (y == h - 1) && (offset + blen == rowSize);
            uint8_t hdr[5];
            hdr[0] = last ? 1 : 0;
            hdr[1] = (uint8_t)(blen & 0xFF);
            hdr[2] = (uint8_t)((blen >> 8) & 0xFF);
            uint16_t nlen = (uint16_t)(~(uint16_t)blen);
            hdr[3] = (uint8_t)(nlen & 0xFF);
            hdr[4] = (uint8_t)((nlen >> 8) & 0xFF);
            fwrite(hdr, 1, 5, f);
            crc = CrcUpdate(crc, hdr, 5);
            fwrite(row.data() + offset, 1, blen, f);
            crc = CrcUpdate(crc, row.data() + offset, blen);
            offset += blen;
        }
    }

    uint8_t adler[4];
    uint32_t ad = (s2 << 16) | s1;
    adler[0] = (uint8_t)(ad >> 24); adler[1] = (uint8_t)(ad >> 16);
    adler[2] = (uint8_t)(ad >> 8);  adler[3] = (uint8_t)ad;
    fwrite(adler, 1, 4, f);
    crc = CrcUpdate(crc, adler, 4);
    crc ^= 0xFFFFFFFFu;
    WriteBe32(f, crc);

    WriteChunk(f, "IEND", nullptr, 0);
    return ferror(f) == 0;
}

// ============================== il2cpp API ==============================
typedef void*  (*il2cpp_domain_get_t)();
typedef void** (*il2cpp_domain_get_assemblies_t)(void*, size_t*);
typedef void*  (*il2cpp_assembly_get_image_t)(void*);
typedef const char* (*il2cpp_image_get_name_t)(void*);
typedef void*  (*il2cpp_class_from_name_t)(void*, const char*, const char*);
typedef void*  (*il2cpp_class_get_method_from_name_t)(void*, const char*, int);
typedef void*  (*il2cpp_runtime_invoke_t)(void*, void*, void**, void**);
typedef void*  (*il2cpp_thread_attach_t)(void*);
typedef const char* (*il2cpp_string_chars_t)(void*);
typedef uint32_t (*il2cpp_array_length_t)(void*);
typedef void*  (*il2cpp_array_new_t)(void*, uintptr_t);
typedef void*  (*il2cpp_class_get_type_t)(void*);
typedef void*  (*il2cpp_type_get_object_t)(void*);

static il2cpp_domain_get_t                il2cpp_domain_get = nullptr;
static il2cpp_domain_get_assemblies_t     il2cpp_domain_get_assemblies = nullptr;
static il2cpp_assembly_get_image_t        il2cpp_assembly_get_image = nullptr;
static il2cpp_image_get_name_t            il2cpp_image_get_name = nullptr;
static il2cpp_class_from_name_t           il2cpp_class_from_name = nullptr;
static il2cpp_class_get_method_from_name_t il2cpp_class_get_method_from_name = nullptr;
static il2cpp_runtime_invoke_t            il2cpp_runtime_invoke = nullptr;
static il2cpp_thread_attach_t             il2cpp_thread_attach = nullptr;
static il2cpp_string_chars_t              il2cpp_string_chars = nullptr;
static il2cpp_array_length_t              il2cpp_array_length = nullptr;
static il2cpp_array_new_t                 il2cpp_array_new = nullptr;
static il2cpp_class_get_type_t            il2cpp_class_get_type = nullptr;
static il2cpp_type_get_object_t           il2cpp_type_get_object = nullptr;

// ============================== состояние ==============================
static void* g_domain = nullptr;
static void* texKlass = nullptr;
static void* imgConvKlass = nullptr;
static void* objKlass = nullptr;
static void* byteKlass = nullptr;
static void* g_texType = nullptr;

static void* mi_findAll = nullptr;
static void* mi_encodePNG = nullptr;
static bool  mi_encodeStatic = false;
static void* mi_loadImage = nullptr;
static bool  mi_loadStatic = false;
static void* mi_loadRaw = nullptr;
static void* mi_apply = nullptr;
static void* mi_isReadable = nullptr;
static void* mi_getName = nullptr;
static void* mi_getWidth = nullptr;
static void* mi_getHeight = nullptr;
static void* mi_getPixels32 = nullptr;

static bool g_resolveDone = false;
static volatile bool g_applyEnabled = false;
static volatile bool g_dumpNow = false;
static bool g_applyDisabledMsgShown = false;
static std::set<std::string> g_appliedFiles;

// ============================== api resolve ==============================
static bool ResolveApi() {
    void* h = dlopen("libil2cpp.so", RTLD_NOW | RTLD_GLOBAL);
    if (!h) {
        TxLogTs("dlopen libil2cpp.so failed");
        return false;
    }
#define TX_SYM(var, name) do { (var) = (decltype(var))dlsym(h, name); } while (0)
    TX_SYM(il2cpp_domain_get, "il2cpp_domain_get");
    TX_SYM(il2cpp_domain_get_assemblies, "il2cpp_domain_get_assemblies");
    TX_SYM(il2cpp_assembly_get_image, "il2cpp_assembly_get_image");
    TX_SYM(il2cpp_image_get_name, "il2cpp_image_get_name");
    TX_SYM(il2cpp_class_from_name, "il2cpp_class_from_name");
    TX_SYM(il2cpp_class_get_method_from_name, "il2cpp_class_get_method_from_name");
    TX_SYM(il2cpp_runtime_invoke, "il2cpp_runtime_invoke");
    TX_SYM(il2cpp_thread_attach, "il2cpp_thread_attach");
    TX_SYM(il2cpp_string_chars, "il2cpp_string_chars");
    TX_SYM(il2cpp_array_length, "il2cpp_array_length");
    TX_SYM(il2cpp_array_new, "il2cpp_array_new");
    TX_SYM(il2cpp_class_get_type, "il2cpp_class_get_type");
    TX_SYM(il2cpp_type_get_object, "il2cpp_type_get_object");
#undef TX_SYM

    if (!il2cpp_domain_get || !il2cpp_domain_get_assemblies || !il2cpp_assembly_get_image ||
        !il2cpp_class_from_name || !il2cpp_class_get_method_from_name || !il2cpp_runtime_invoke) {
        TxLogTs("missing core il2cpp exports");
        return false;
    }
    return true;
}

// Аттач делаем только перед реальной работой с il2cpp — ранняя фаза убивала игру
static bool EnsureThread() {
    if (!g_domain || !il2cpp_thread_attach) return false;
    il2cpp_thread_attach(g_domain);
    return true;
}

// ============================== invoke-хелперы ==============================
static int32_t ArrLen(void* arr) {
    if (!arr) return 0;
    if (il2cpp_array_length) return (int32_t)il2cpp_array_length(arr);
    return *(int32_t*)((uintptr_t)arr + 0x18);
}

static bool InvokeBool(void* method, void* obj) {
    void* exc = nullptr;
    void* r = il2cpp_runtime_invoke(method, obj, nullptr, &exc);
    if (exc || !r) return false;
    return *(uint8_t*)((uintptr_t)r + 0x10) != 0;
}

static int32_t InvokeInt(void* method, void* obj) {
    void* exc = nullptr;
    void* r = il2cpp_runtime_invoke(method, obj, nullptr, &exc);
    if (exc || !r) return -1;
    return *(int32_t*)((uintptr_t)r + 0x10);
}

static std::string InvokeString(void* method, void* obj) {
    void* exc = nullptr;
    void* r = il2cpp_runtime_invoke(method, obj, nullptr, &exc);
    if (exc || !r || !il2cpp_string_chars) return "";
    const char* c = il2cpp_string_chars(r);
    return c ? std::string(c) : std::string();
}

static void* MakeByteArray(const uint8_t* data, size_t len) {
    if (!byteKlass || !il2cpp_array_new || len == 0) return nullptr;
    void* a = il2cpp_array_new(byteKlass, len);
    if (!a) return nullptr;
    memcpy((void*)((uintptr_t)a + 0x20), data, len);
    return a;
}

static std::string SanitizeName(const std::string& in) {
    std::string out;
    for (size_t i = 0; i < in.size(); i++) {
        unsigned char u = (unsigned char)in[i];
        if (isalnum(u) || u == '_' || u == '-' || u == '.') out += (char)u;
        else out += '_';
    }
    if (out.empty()) out = "tex";
    if (out.size() > 100) out.resize(100);
    return out;
}

// ============================== резолв классов ==============================
static void ResolveIl2Cpp() {
    if (g_resolveDone) return;
    if (!EnsureThread()) {
        TxLogTs("resolve: thread attach failed");
        return;
    }

    size_t count = 0;
    void** assemblies = nullptr;
    for (int tries = 0; tries < 50; tries++) {
        assemblies = il2cpp_domain_get_assemblies(g_domain, &count);
        if (assemblies && count > 0) break;
        usleep(200000);
    }
    if (!assemblies || count == 0) {
        TxLogTs("no assemblies available");
        return;
    }

    for (size_t i = 0; i < count; i++) {
        void* image = il2cpp_assembly_get_image(assemblies[i]);
        if (!image) continue;
        const char* nm = il2cpp_image_get_name ? il2cpp_image_get_name(image) : nullptr;
        TxLog("asm[%d]: %s", (int)i, nm ? nm : "?");
        if (!texKlass)    texKlass    = il2cpp_class_from_name(image, "UnityEngine", "Texture2D");
        if (!imgConvKlass) imgConvKlass = il2cpp_class_from_name(image, "UnityEngine", "ImageConversion");
        if (!objKlass)    objKlass    = il2cpp_class_from_name(image, "UnityEngine", "Object");
        if (!byteKlass)   byteKlass   = il2cpp_class_from_name(image, "System", "Byte");
    }

    TxLogTs(imgConvKlass ? "ImageConversion found" : "ImageConversion NOT found (stripped?)");
    if (!texKlass) {
        TxLogTs("Texture2D NOT found");
        return;
    }

    void* t = il2cpp_class_get_type(texKlass);
    if (t) g_texType = il2cpp_type_get_object(t);

    if (objKlass) mi_findAll = il2cpp_class_get_method_from_name(objKlass, "FindObjectsOfTypeAll", 1);

    if (imgConvKlass) {
        mi_encodePNG = il2cpp_class_get_method_from_name(imgConvKlass, "EncodeToPNG", 1);
        mi_loadImage = il2cpp_class_get_method_from_name(imgConvKlass, "LoadImage", 2);
        if (mi_encodePNG) mi_encodeStatic = true;
        if (mi_loadImage) mi_loadStatic = true;
    }
    if (texKlass) {
        if (!mi_encodePNG) mi_encodePNG = il2cpp_class_get_method_from_name(texKlass, "EncodeToPNG", 0);
        if (!mi_loadImage) mi_loadImage = il2cpp_class_get_method_from_name(texKlass, "LoadImage", 1);
        mi_apply       = il2cpp_class_get_method_from_name(texKlass, "Apply", 0);
        mi_isReadable  = il2cpp_class_get_method_from_name(texKlass, "get_isReadable", 0);
        mi_getName     = il2cpp_class_get_method_from_name(texKlass, "get_name", 0);
        mi_getWidth    = il2cpp_class_get_method_from_name(texKlass, "get_width", 0);
        mi_getHeight   = il2cpp_class_get_method_from_name(texKlass, "get_height", 0);
        mi_getPixels32 = il2cpp_class_get_method_from_name(texKlass, "GetPixels32", 0);
        mi_loadRaw     = il2cpp_class_get_method_from_name(texKlass, "LoadRawTextureData", 1);
    }

    TxLog("resolved: tex=%p imgConv=%p type=%p findAll=%p enc=%p(static=%d) load=%p(static=%d) raw=%p apply=%p pixels=%p",
          texKlass, imgConvKlass, g_texType, mi_findAll,
          mi_encodePNG, (int)mi_encodeStatic,
          mi_loadImage, (int)mi_loadStatic,
          mi_loadRaw, mi_apply, mi_getPixels32);

    g_resolveDone = true;
}

// ============================== дамп ==============================
static bool DumpExists() {
    DIR* d = opendir(DUMP_DIR);
    if (!d) return false;
    bool any = false;
    struct dirent* e;
    while ((e = readdir(d)) != nullptr) {
        size_t len = strlen(e->d_name);
        if (len > 4 && strcmp(e->d_name + len - 4, ".png") == 0) { any = true; break; }
    }
    closedir(d);
    return any;
}

static int DumpTextures() {
    if (!EnsureThread()) return 0;
    if (!g_texType || !mi_findAll || !texKlass) {
        TxLogTs("dump: core methods not resolved");
        return 0;
    }

    void* exc = nullptr;
    void* params[1] = { g_texType };
    void* arr = il2cpp_runtime_invoke(mi_findAll, nullptr, params, &exc);
    if (exc || !arr) {
        TxLogTs("FindObjectsOfTypeAll failed");
        return 0;
    }

    int32_t n = ArrLen(arr);
    TxLog("objects found: %d", (int)n);
    if (n <= 0) return 0;

    void** items = (void**)((uintptr_t)arr + 0x20);
    mkdir(DUMP_DIR, 0777);

    int saved = 0, skipped = 0, unreadable = 0;

    for (int32_t i = 0; i < n; i++) {
        void* tex = items[i];
        if (!tex) { skipped++; continue; }
        if (mi_isReadable && !InvokeBool(mi_isReadable, tex)) { unreadable++; continue; }

        int w = mi_getWidth  ? (int)InvokeInt(mi_getWidth, tex)  : 0;
        int h = mi_getHeight ? (int)InvokeInt(mi_getHeight, tex) : 0;
        if (w <= 0 || h <= 0) { skipped++; continue; }

        std::string name = mi_getName ? InvokeString(mi_getName, tex) : "";
        std::string fname = SanitizeName(name);

        char path[512];
        bool exists = false;
        for (int dup = 1; dup < 100; dup++) {
            if (dup == 1) snprintf(path, sizeof(path), "%s/%s.png", DUMP_DIR, fname.c_str());
            else snprintf(path, sizeof(path), "%s/%s_%d.png", DUMP_DIR, fname.c_str(), dup);
            struct stat st;
            if (stat(path, &st) != 0) { exists = false; break; }
            exists = true;
        }
        if (exists) { skipped++; continue; }

        if (mi_encodePNG) {
            void* png = nullptr;
            exc = nullptr;
            if (mi_encodeStatic) {
                void* p[1] = { tex };
                png = il2cpp_runtime_invoke(mi_encodePNG, nullptr, p, &exc);
            } else {
                png = il2cpp_runtime_invoke(mi_encodePNG, tex, nullptr, &exc);
            }
            if (exc || !png) { skipped++; continue; }
            int32_t len = ArrLen(png);
            const uint8_t* data = (const uint8_t*)((uintptr_t)png + 0x20);
            FILE* fp = fopen(path, "wb");
            if (!fp) { skipped++; continue; }
            fwrite(data, 1, (size_t)len, fp);
            fclose(fp);
            saved++;
        } else if (mi_getPixels32) {
            void* cols = nullptr;
            exc = nullptr;
            cols = il2cpp_runtime_invoke(mi_getPixels32, tex, nullptr, &exc);
            if (exc || !cols) { unreadable++; continue; }
            int32_t cn = ArrLen(cols);
            if ((int32_t)w * (int32_t)h != cn) { skipped++; continue; }
            if (w * h > 16 * 1024 * 1024) {
                TxLog("skip too large: %s %dx%d", fname.c_str(), w, h);
                skipped++;
                continue;
            }
            const uint8_t* px = (const uint8_t*)((uintptr_t)cols + 0x20);
            FILE* fp = fopen(path, "wb");
            if (!fp) { skipped++; continue; }
            if (!WritePngStored(fp, w, h, px)) {
                fclose(fp);
                remove(path);
                skipped++;
            } else {
                fclose(fp);
                saved++;
            }
        } else {
            skipped++;
        }
    }

    TxLog("dump done: saved=%d skipped=%d unreadable=%d", saved, skipped, unreadable);
    return saved;
}

// ============================== применение ==============================
static void ApplyCustomTextures() {
    if (!EnsureThread()) return;
    if (!texKlass || !mi_apply || !g_texType || !mi_findAll) return;

    if (!mi_loadImage && !mi_loadRaw) {
        if (!g_applyDisabledMsgShown) {
            g_applyDisabledMsgShown = true;
            TxLogTs("apply disabled: LoadImage/LoadRawTextureData not available");
        }
        return;
    }

    void* exc = nullptr;
    void* params[1] = { g_texType };
    void* arr = il2cpp_runtime_invoke(mi_findAll, nullptr, params, &exc);
    if (exc || !arr) return;
    int32_t n = ArrLen(arr);
    if (n <= 0) return;
    void** items = (void**)((uintptr_t)arr + 0x20);

    DIR* d = opendir(APPLY_DIR);
    if (!d) return;

    struct dirent* e;
    while ((e = readdir(d)) != nullptr) {
        std::string fn(e->d_name);
        bool isPng = fn.size() > 4 && fn.compare(fn.size() - 4, 4, ".png") == 0;
        bool isRaw = fn.size() > 4 && fn.compare(fn.size() - 4, 4, ".raw") == 0;
        if (!isPng && !isRaw) continue;

        bool usePng = isPng && mi_loadImage;
        bool useRaw = isRaw && mi_loadRaw;
        if (!usePng && !useRaw) continue;

        std::string base = fn.substr(0, fn.size() - 4);

        char fpath[512];
        snprintf(fpath, sizeof(fpath), "%s/%s", APPLY_DIR, fn.c_str());
        FILE* f = fopen(fpath, "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz <= 0) { fclose(f); continue; }
        std::vector<uint8_t> bytes((size_t)sz);
        fread(bytes.data(), 1, bytes.size(), f);
        fclose(f);

        for (int32_t i = 0; i < n; i++) {
            void* tex = items[i];
            if (!tex) continue;
            std::string tname = mi_getName ? InvokeString(mi_getName, tex) : "";
            if (tname != base) continue;
            if (g_appliedFiles.count(base)) break;

            void* exc2 = nullptr;
            if (usePng) {
                void* bArr = MakeByteArray(bytes.data(), bytes.size());
                if (!bArr) break;
                if (mi_loadStatic) {
                    void* p[2] = { tex, bArr };
                    il2cpp_runtime_invoke(mi_loadImage, nullptr, p, &exc2);
                } else {
                    void* p[1] = { bArr };
                    il2cpp_runtime_invoke(mi_loadImage, tex, p, &exc2);
                }
            } else {
                int w = (int)InvokeInt(mi_getWidth, tex);
                int h = (int)InvokeInt(mi_getHeight, tex);
                if (w <= 0 || h <= 0) break;
                if ((size_t)sz != (size_t)w * (size_t)h * 4) break;
                void* bArr = MakeByteArray(bytes.data(), bytes.size());
                if (!bArr) break;
                void* p[1] = { bArr };
                il2cpp_runtime_invoke(mi_loadRaw, tex, p, &exc2);
            }

            if (!exc2) {
                il2cpp_runtime_invoke(mi_apply, tex, nullptr, &exc2);
                if (!exc2) {
                    g_appliedFiles.insert(base);
                    TxLog("applied: %s", base.c_str());
                }
            }
            break;
        }
    }
    closedir(d);
}

static void DumpLoop() {
    for (int attempt = 1; attempt <= 12; attempt++) {
        if (DumpExists()) {
            TxLogTs("dump exists, skip");
            return;
        }
        TxLog("dump attempt %d/12", attempt);
        int saved = DumpTextures();
        if (saved > 0) return;
        if (attempt < 12) sleep(30);
    }
    TxLogTs("dump gave up after 12 attempts");
}

// ============================== контракт с меню ==============================
// Сигнатуры должны совпадать с декларациями в Menu.hpp.
// Если при сборке появится "multiple definition" — значит, эти функции
// уже определены в Menu.cpp, тогда удали их оттуда (или скажи мне).
extern "C" jobjectArray GetFeatureList(JNIEnv *env, jclass clazz) {
    const char* features[2];
    features[0] = OBFUSCATE("Toggle_Apply custom textures");
    features[1] = OBFUSCATE("Button_Dump textures now");
    int count = 2;

    jclass strClass = env->FindClass(OBFUSCATE("java/lang/String"));
    jobjectArray ret = (jobjectArray)env->NewObjectArray(count, strClass, env->NewStringUTF(OBFUSCATE("")));
    for (int i = 0; i < count; i++)
        env->SetObjectArrayElement(ret, i, env->NewStringUTF(features[i]));
    return ret;
}

extern "C" void* Changes(JNIEnv *env, jclass clazz, jobject obj, jint featNum, jstring featName, jint value, jboolean boolean, jstring toastErrorMsg) {
    switch (featNum) {
        case 0:
            g_applyEnabled = (bool)boolean;
            TxLog("apply toggled: %s", g_applyEnabled ? "ON" : "OFF");
            break;
        case 1:
            g_dumpNow = true;
            break;
        default:
            break;
    }
    return nullptr;
}

// ============================== главный поток ==============================
static void HackThread() {
    while (!isLibraryLoaded(targetLibName)) {
        sleep(1);
    }
    TxLogTs("libil2cpp.so loaded");

    if (!ResolveApi()) return;

    for (;;) {
        void* d = il2cpp_domain_get ? il2cpp_domain_get() : nullptr;
        if (d) { g_domain = d; break; }
        sleep(1);
    }
    TxLogTs("il2cpp domain ready");

    ResolveIl2Cpp();

    std::thread(DumpLoop).detach();

    while (true) {
        if (g_dumpNow) {
            g_dumpNow = false;
            DumpTextures();
        }
        if (g_applyEnabled) ApplyCustomTextures();
        sleep(2);
    }
}

__attribute__((constructor)) static void lib_main() {
    std::thread(HackThread).detach();
}

