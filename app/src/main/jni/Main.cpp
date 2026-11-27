// Main.cpp — Texture Injection by Name (inject-only, no dump)
// PNG files are placed on PC into /storage/emulated/0/texmod/<texture_name>.png
// Entry point: constructor lib_main

#include <jni.h>
#include <android/log.h>
#include <cstdarg>
#include <string>
#include <vector>
#include <unordered_set>
#include <dirent.h>
#include <strings.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <dlfcn.h>
#include <sys/stat.h>

#include "Includes/obfuscate.h"
#include "Includes/Utils.hpp"
#include "Menu/Menu.hpp"

#define targetLibName OBFUSCATE("libil2cpp.so")

#ifndef ANDROID_LOG_INFO
#define ANDROID_LOG_INFO 4
#endif
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "TEXMOD", __VA_ARGS__)

static const char* TEXMOD_DIR = "/storage/emulated/0/texmod";
static const char* LOG_PATH   = "/storage/emulated/0/texmod_log.txt";

// ================= file log =================
static FILE* g_logFile = NULL;
static pthread_mutex_t g_logMutex = PTHREAD_MUTEX_INITIALIZER;
static std::vector<std::string> g_pending;
static bool g_storageOK = false;

static void WriteLog(const char* msg) {
    pthread_mutex_lock(&g_logMutex);
    if (g_storageOK && g_logFile) {
        fprintf(g_logFile, "%s\n", msg);
        fflush(g_logFile);
    } else {
        if (g_pending.size() < 200) g_pending.push_back(msg);
    }
    pthread_mutex_unlock(&g_logMutex);
}

static void OpenLogFile() {
    pthread_mutex_lock(&g_logMutex);
    if (!g_logFile) {
        g_logFile = fopen(LOG_PATH, "a");
        if (g_logFile) {
            fprintf(g_logFile, "===== launch (inject-only build) =====\n");
            for (size_t i = 0; i < g_pending.size(); i++)
                fprintf(g_logFile, "%s\n", g_pending[i].c_str());
            fflush(g_logFile);
            g_pending.clear();
            g_storageOK = true;
        }
    }
    pthread_mutex_unlock(&g_logMutex);
}

static void LogMsg(const char* fmt, ...) {
    char msgBuf[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msgBuf, sizeof(msgBuf), fmt, args);
    va_end(args);
    LOGI("%s", msgBuf);
    WriteLog(msgBuf);
}

// ================= il2cpp API =================
typedef void* (*fn_DomainGet)();
typedef void* (*fn_ThreadAttach)(void*);
typedef void** (*fn_AssemblyGetImage)(void*);
typedef void** (*fn_DomainGetAssemblies)(void*, unsigned long long*);
typedef void* (*fn_ImageGetClass)(void*, int);
typedef int (*fn_ImageGetClassCount)(void*);
typedef const char* (*fn_ClassGetName)(void*);
typedef const char* (*fn_ClassGetNamespace)(void*);
typedef const char* (*fn_ImageGetName)(void*);
typedef void* (*fn_ClassGetMethod)(void*, const char*, int);
typedef void* (*fn_ClassFromName)(void*, const char*, const char*);
typedef void* (*fn_ObjectNew)(void*);
typedef void* (*fn_ArrayNew)(void*, unsigned int, void*);
typedef void* (*fn_ClassGetType)(void*);
typedef void* (*fn_TypeGetObject)(void*);
typedef void* (*fn_RuntimeInvoke)(const void*, void*, void**, void**);
typedef void* (*fn_ResolveIcall)(const char*);
typedef void* (*fn_FieldFromName)(void*, const char*);
typedef void (*fn_FieldGetValue)(void*, void*, void*);
typedef void (*fn_FieldSetValue)(void*, void*, void*);

static void* g_il2cppHandle = NULL;
static fn_DomainGet p_DomainGet = NULL;
static fn_ThreadAttach p_ThreadAttach = NULL;
static fn_AssemblyGetImage p_AssemblyGetImage = NULL;
static fn_DomainGetAssemblies p_DomainGetAssemblies = NULL;
static fn_ImageGetClass p_ImageGetClass = NULL;
static fn_ImageGetClassCount p_ImageGetClassCount = NULL;
static fn_ClassGetName p_ClassGetName = NULL;
static fn_ClassGetNamespace p_ClassGetNamespace = NULL;
static fn_ImageGetName p_ImageGetName = NULL;
static fn_ClassGetMethod p_ClassGetMethod = NULL;
static fn_ClassFromName p_ClassFromName = NULL;
static fn_ObjectNew p_ObjectNew = NULL;
static fn_ArrayNew p_ArrayNew = NULL;
static fn_ClassGetType p_ClassGetType = NULL;
static fn_TypeGetObject p_TypeGetObject = NULL;
static fn_RuntimeInvoke p_RuntimeInvoke = NULL;
static fn_ResolveIcall p_ResolveIcall = NULL;
static fn_FieldFromName p_FieldFromName = NULL;
static fn_FieldGetValue p_FieldGetValue = NULL;
static fn_FieldSetValue p_FieldSetValue = NULL;

// resolved game side
static void* g_domain = NULL;
static bool g_resolved = false;
static int g_resolveRetry = 0;
static void* g_texClass = NULL;
static void* g_resClass = NULL;
static void* g_imgConvClass = NULL;
static void* g_byteClass = NULL;
static void* g_boolClass = NULL;
static void* g_texTypeObj = NULL;      // reflection type for FindObjectsOfTypeAll
static void* g_byteType = NULL;
static const void* g_findAll = NULL;
static const void* g_getName = NULL;
static const void* g_apply0 = NULL;
static const void* g_apply3 = NULL;
static const void* g_loadImg3 = NULL;  // static ImageConversion.LoadImage(tex, bytes, markNonReadable)
static const void* g_loadImg1 = NULL;  // instance Texture2D.LoadImage(bytes) (old Unity)
static void* g_loadImg_icall = NULL;   // direct icall fallback
static void* g_nameField = NULL;       // Texture2D.m_Name fallback
static void* g_mscorlibImg = NULL;

static bool InitApiPointers() {
    if (!g_il2cppHandle) {
        g_il2cppHandle = dlopen("libil2cpp.so", RTLD_NOW);
        if (!g_il2cppHandle) { LogMsg("dlopen libil2cpp.so failed"); return false; }
        LogMsg("libil2cpp.so loaded");
    }
#define GETSYM(var, name) if (!(var)) (var) = reinterpret_cast<decltype(var)>(dlsym(g_il2cppHandle, name));
    GETSYM(p_DomainGet, "il2cpp_domain_get");
    GETSYM(p_ThreadAttach, "il2cpp_thread_attach");
    GETSYM(p_AssemblyGetImage, "il2cpp_assembly_get_image");
    GETSYM(p_DomainGetAssemblies, "il2cpp_domain_get_assemblies");
    GETSYM(p_ImageGetClass, "il2cpp_image_get_class");
    GETSYM(p_ImageGetClassCount, "il2cpp_image_get_class_count");
    GETSYM(p_ClassGetName, "il2cpp_class_get_name");
    GETSYM(p_ClassGetNamespace, "il2cpp_class_get_namespace");
    GETSYM(p_ImageGetName, "il2cpp_image_get_name");
    GETSYM(p_ClassGetMethod, "il2cpp_class_get_method_from_name");
    GETSYM(p_ClassFromName, "il2cpp_class_from_name");
    GETSYM(p_ObjectNew, "il2cpp_object_new");
    GETSYM(p_ArrayNew, "il2cpp_array_new");
    GETSYM(p_ClassGetType, "il2cpp_class_get_type");
    GETSYM(p_TypeGetObject, "il2cpp_type_get_object");
    GETSYM(p_RuntimeInvoke, "il2cpp_runtime_invoke");
    GETSYM(p_ResolveIcall, "il2cpp_resolve_icall");
    GETSYM(p_FieldFromName, "il2cpp_class_get_field_from_name");
    GETSYM(p_FieldGetValue, "il2cpp_field_get_value");
    GETSYM(p_FieldSetValue, "il2cpp_field_set_value");
    if (!p_DomainGet) { LogMsg("il2cpp symbols not found"); return false; }
    return true;
}

static bool EnsureThread() {
    if (!g_domain || !p_ThreadAttach) return false;
    void* t = p_ThreadAttach(g_domain);
    return t != NULL;
}

static bool ResolveIl2Cpp() {
    if (!InitApiPointers()) return false;
    if (!g_domain) {
        g_domain = p_DomainGet();
        if (!g_domain) {
            if (++g_resolveRetry % 10 == 0) LogMsg("waiting for il2cpp domain...");
            return false;
        }
        LogMsg("il2cpp domain ready");
    }
    if (g_resolved) return true;

    // wait until assemblies are populated
    unsigned long long n = 0;
    void** assemblies = NULL;
    for (int tries = 0; tries < 50; tries++) {
        assemblies = p_DomainGetAssemblies(g_domain, &n);
        if (assemblies && n > 0) break;
        usleep(100000);
    }
    if (!assemblies || n == 0) { LogMsg("assemblies still empty"); return false; }

    for (unsigned long long i = 0; i < n; i++) {
        void* img = assemblies[i] ? p_AssemblyGetImage(assemblies[i]) : NULL;
        const char* imgName = img ? p_ImageGetName(img) : NULL;
        if (i < 100) LogMsg("asm[%d]: %s", (int)i, imgName ? imgName : "?");
        if (imgName && strstr(imgName, "mscorlib")) g_mscorlibImg = img;
    }
    if (g_mscorlibImg) {
        g_byteClass = p_ClassFromName(g_mscorlibImg, "System", "Byte");
        g_boolClass = p_ClassFromName(g_mscorlibImg, "System", "Boolean");
    }

    int scanned = 0;
    for (unsigned long long i = 0; i < n; i++) {
        void* img = assemblies[i] ? p_AssemblyGetImage(assemblies[i]) : NULL;
        if (!img) continue;
        int cnt = p_ImageGetClassCount(img);
        for (int c = 0; c < cnt; c++) {
            void* k = p_ImageGetClass(img, c);
            if (!k) continue;
            scanned++;
            const char* ns = p_ClassGetNamespace(k);
            const char* nm = p_ClassGetName(k);
            bool isCore = (ns && !strcmp(ns, "UnityEngine"));
            bool isSys  = (ns && !strcmp(ns, "System"));
            if (!nm) continue;
            if (!strcmp(nm, "Texture2D") && isCore && !g_texClass) g_texClass = k;
            else if (!strcmp(nm, "Resources") && isCore && !g_resClass) g_resClass = k;
            else if (!strcmp(nm, "ImageConversion") && isCore && !g_imgConvClass) g_imgConvClass = k;
            else if (!strcmp(nm, "Byte") && isSys && !g_byteClass) g_byteClass = k;
            else if (!strcmp(nm, "Boolean") && isSys && !g_boolClass) g_boolClass = k;
        }
    }
    LogMsg("classes scanned: %d", scanned);

    if (!g_texClass) { LogMsg("Texture2D NOT found"); return false; }

    g_texTypeObj = p_TypeGetObject(p_ClassGetType(g_texClass));
    if (g_byteClass) g_byteType = p_ClassGetType(g_byteClass);

    if (g_resClass)  g_findAll  = p_ClassGetMethod(g_resClass, "FindObjectsOfTypeAll", 1);
    g_getName = p_ClassGetMethod(g_texClass, "get_name", 0);
    g_nameField = p_FieldFromName(g_texClass, "m_Name");
    g_apply0 = p_ClassGetMethod(g_texClass, "Apply", 0);
    if (!g_apply0) g_apply3 = p_ClassGetMethod(g_texClass, "Apply", 3);
    if (g_imgConvClass) g_loadImg3 = p_ClassGetMethod(g_imgConvClass, "LoadImage", 3);
    g_loadImg1 = p_ClassGetMethod(g_texClass, "LoadImage", 1);
    g_loadImg_icall = p_ResolveIcall("UnityEngine.ImageConversion::LoadImage(UnityEngine.Texture2D,System.Byte[],System.Boolean)");

    LogMsg("resolved: tex=%p res=%p imgConv=%p findAll=%p getName=%p apply0=%p apply3=%p load3=%p load1=%p icall=%p",
           g_texClass, g_resClass, g_imgConvClass, g_findAll, g_getName,
           g_apply0, g_apply3, g_loadImg3, g_loadImg1, g_loadImg_icall);

    if (!g_findAll) { LogMsg("FindObjectsOfTypeAll NOT found"); return false; }
    g_resolved = true;
    return true;
}

// ================= helpers =================
static std::string Utf16ToUtf8(void* str) {
    if (!str) return "";
    int len = *(int*)((char*)str + 0x10);
    unsigned short* chars = (unsigned short*)((char*)str + 0x14);
    std::string out;
    for (int i = 0; i < len && chars[i] != 0; i++) {
        unsigned short c = chars[i];
        if (c < 0x80) out.push_back((char)c);
        else if (c < 0x800) {
            out.push_back((char)(0xC0 | (c >> 6)));
            out.push_back((char)(0x80 | (c & 0x3F)));
        } else {
            out.push_back((char)(0xE0 | (c >> 12)));
            out.push_back((char)(0x80 | ((c >> 6) & 0x3F)));
            out.push_back((char)(0x80 | (c & 0x3F)));
        }
    }
    return out;
}

static std::string SanitizeName(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        char c = s[i];
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|' || c < 0x20)
            out.push_back('_');
        else
            out.push_back(c);
    }
    return out;
}

static std::string GetTexName(void* texObj) {
    if (g_getName) {
        void* str = p_RuntimeInvoke(g_getName, texObj, NULL, NULL);
        if (str) {
            std::string s = Utf16ToUtf8(str);
            if (!s.empty()) return s;
        }
    }
    if (!g_nameField && g_texClass) g_nameField = p_FieldFromName(g_texClass, "m_Name");
    if (g_nameField) {
        void* str = NULL;
        p_FieldGetValue(texObj, g_nameField, &str);
        return Utf16ToUtf8(str);
    }
    return "";
}

static unsigned char* ReadFileBytes(const std::string& path, int* outLen) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 64 * 1024 * 1024) { fclose(f); return NULL; }
    unsigned char* fileBuf = (unsigned char*)malloc((size_t)sz);
    if (!fileBuf) { fclose(f); return NULL; }
    size_t rd = fread(fileBuf, 1, (size_t)sz, f);
    fclose(f);
    if ((long)rd != sz) { free(fileBuf); return NULL; }
    *outLen = (int)sz;
    return fileBuf;
}

static void* NewByteArray(int len, const unsigned char* data) {
    if (!g_byteClass || !g_byteType || !p_ArrayNew) return NULL;
    void* arr = p_ArrayNew(g_byteClass, (unsigned int)len, g_byteType);
    if (!arr) return NULL;
    memcpy((char*)arr + 0x20, data, (size_t)len);
    return arr;
}

static void* NewBool(bool v) {
    if (!g_boolClass || !p_ObjectNew) return NULL;
    void* box = p_ObjectNew(g_boolClass);
    if (!box) return NULL;
    void* f = p_FieldFromName(g_boolClass, "m_value");
    if (f) p_FieldSetValue(box, f, &v);
    return box;
}

static void* InvokeApply(void* texObj) {
    if (g_apply0) {
        return p_RuntimeInvoke(g_apply0, texObj, NULL, NULL);
    }
    if (g_apply3) {
        void* b1 = NewBool(true);   // updateMipmaps
        void* b2 = NewBool(false);  // makeNoLongerReadable
        void* b3 = NewBool(false);  // linear
        if (!b1 || !b2 || !b3) return NULL;
        void* params[3] = { b1, b2, b3 };
        return p_RuntimeInvoke(g_apply3, texObj, params, NULL);
    }
    return NULL;
}

static bool InvokeLoadImage(void* texObj, void* byteArr) {
    if (g_loadImg3 && g_boolClass) {
        void* markNonReadable = NewBool(false);
        if (markNonReadable) {
            void* params[3] = { texObj, byteArr, markNonReadable };
            void* ret = p_RuntimeInvoke(g_loadImg3, NULL, params, NULL);
            if (ret) return *(bool*)((char*)ret + 0x10);
            return false;
        }
    }
    if (g_loadImg1) {
        void* params[1] = { byteArr };
        void* ret = p_RuntimeInvoke(g_loadImg1, texObj, params, NULL);
        if (ret) return *(bool*)((char*)ret + 0x10);
        return false;
    }
    if (g_loadImg_icall) {
        typedef bool (*fn_LoadImage_icall)(void*, void*, bool);
        return ((fn_LoadImage_icall)g_loadImg_icall)(texObj, byteArr, false);
    }
    return false;
}

// ================= injection =================
static std::unordered_set<void*> g_applied;
static int g_injectTotal = 0;
bool g_injectEnabled = true;
static int g_passCount = 0;

static void ScanTexmodDir(std::vector<std::string>& out) {
    DIR* d = opendir(TEXMOD_DIR);
    if (!d) return;
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        size_t len = strlen(e->d_name);
        if (len > 4 && strcasecmp(e->d_name + len - 4, ".png") == 0)
            out.push_back(e->d_name);
    }
    closedir(d);
}

static void InjectPass() {
    std::vector<std::string> files;
    ScanTexmodDir(files);
    if (files.empty()) return;
    if (!EnsureThread()) return;
    if (!g_findAll || !g_texTypeObj) return;

    void* params[1] = { g_texTypeObj };
    void* arr = p_RuntimeInvoke(g_findAll, NULL, params, NULL);
    if (!arr) return;
    int count = *(int*)((char*)arr + 0x18);
    if (count <= 0) return;

    int injected = 0;
    for (int i = 0; i < count; i++) {
        void* tex = *(void**)((char*)arr + 0x20 + 8 * (size_t)i);
        if (!tex) continue;
        if (g_applied.count(tex)) continue;

        std::string name = GetTexName(tex);
        if (name.empty()) continue;
        std::string sName = SanitizeName(name);

        for (size_t f = 0; f < files.size(); f++) {
            std::string base = files[f].substr(0, files[f].size() - 4);
            if (base != sName) continue;

            std::string path = std::string(TEXMOD_DIR) + "/" + files[f];
            int len = 0;
            unsigned char* data = ReadFileBytes(path, &len);
            if (!data) { LogMsg("read failed: %s", path.c_str()); continue; }
            void* byteArr = NewByteArray(len, data);
            free(data);
            if (!byteArr) { LogMsg("array alloc failed"); continue; }

            if (InvokeLoadImage(tex, byteArr)) {
                InvokeApply(tex);
                g_applied.insert(tex);
                injected++;
                g_injectTotal++;
                LogMsg("injected: %s", name.c_str());
            } else {
                LogMsg("LoadImage failed: %s", name.c_str());
            }
        }
    }
    if (injected > 0) LogMsg("inject pass: +%d (total %d)", injected, g_injectTotal);
    if (++g_passCount % 20 == 0)
        LogMsg("heartbeat: files=%d applied=%d", (int)files.size(), g_injectTotal);
}

// ================= thread =================
static bool IsLibLoaded(const char* libName) {
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return false;
    char mapsBuf[512];
    bool found = false;
    while (fgets(mapsBuf, sizeof(mapsBuf), f)) {
        if (strstr(mapsBuf, libName)) { found = true; break; }
    }
    fclose(f);
    return found;
}

static void* ModThread(void*) {
    LogMsg("mod start (inject-only build)");
    for (int i = 0; i < 600 && !g_storageOK; i++) {
        OpenLogFile();
        usleep(500000);
    }
    if (g_storageOK) LogMsg("storage access OK");
    else LogMsg("storage access NOT granted, log file unavailable");

    int waited = 0;
    while (!IsLibLoaded("libil2cpp.so")) {
        usleep(500000);
        if (++waited % 20 == 0) LogMsg("waiting for libil2cpp.so...");
        if (waited > 1200) { LogMsg("libil2cpp.so not loaded, exit"); return NULL; }
    }
    LogMsg("libil2cpp.so loaded");

    while (!g_resolved) {
        usleep(500000);
        ResolveIl2Cpp();
    }

    while (true) {
        if (g_injectEnabled) InjectPass();
        usleep(3000000); // every 3 seconds
    }
    return NULL;
}

__attribute__((constructor))
void lib_main() {
    pthread_t modThread;
    pthread_create(&modThread, NULL, ModThread, NULL);
}

// ================= menu contract =================
// Signatures must match Menu.hpp exactly:
//   jobjectArray GetFeatureList(JNIEnv *env, jobject context);
//   void Changes(JNIEnv *env, jclass clazz, jobject obj,
//                int featNum, jstring featName, int value,
//                long changes, unsigned char boolean, jstring str);
// No extern "C": Setup.cpp calls the C++-mangled symbols from Menu.hpp.

jobjectArray GetFeatureList(JNIEnv *env, jobject context) {
    jclass stringCls = env->FindClass("java/lang/String");
    const char* items[] = {
        "Inject Textures_0_on"   // toggle: name_index_default
    };
    jobjectArray arr = env->NewObjectArray(1, stringCls, NULL);
    for (int i = 0; i < 1; i++)
        env->SetObjectArrayElement(arr, i, env->NewStringUTF(items[i]));
    LOGI("GetFeatureList: 1 feature");
    return arr;
}

void Changes(JNIEnv *env, jclass clazz, jobject obj, int featNum,
             jstring featName, int value, long changes,
             unsigned char boolean, jstring str) {
    if (featNum == 0) {
        g_injectEnabled = (boolean != 0);
        LogMsg("inject enabled: %d", g_injectEnabled ? 1 : 0);
    }
    LOGI("Changes: featNum=%d boolean=%d", featNum, (int)boolean);
}
