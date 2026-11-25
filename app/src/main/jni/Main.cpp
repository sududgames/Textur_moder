// ===================== TexMod: auto dump + auto apply =====================
// Полностью автоматический: при запуске проверяет дамп (нет — дампит),
// применяет текстуры из texmod и повторяет применение каждые 60 секунд.

#include <jni.h>
#include <string>
#include <vector>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstring>
#include <thread>
#include <chrono>
#include <dlfcn.h>
#include <android/log.h>

#include "Includes/obfuscate.h"

#ifndef ANDROID_LOG_INFO
#define ANDROID_LOG_INFO 4
#endif

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "TEXMOD", __VA_ARGS__)

// --- il2cpp API ---
typedef void* (*il2cpp_domain_get_t)();
typedef void** (*il2cpp_domain_get_assemblies_t)(void*, size_t*);
typedef void* (*il2cpp_assembly_get_image_t)(void*);
typedef void* (*il2cpp_class_from_name_t)(void*, const char*, const char*);
typedef void* (*il2cpp_class_get_method_from_name_t)(void*, const char*, int);
typedef void* (*il2cpp_runtime_invoke_t)(void*, void*, void**, void**);
typedef void* (*il2cpp_thread_current_t)();
typedef void* (*il2cpp_thread_attach_t)(void*);
typedef const char* (*il2cpp_string_chars_t)(void*);
typedef uint32_t (*il2cpp_array_length_t)(void*);
typedef void* (*il2cpp_array_new_t)(void*, uintptr_t, uintptr_t);

static il2cpp_domain_get_t il2cpp_domain_get = nullptr;
static il2cpp_domain_get_assemblies_t il2cpp_domain_get_assemblies = nullptr;
static il2cpp_assembly_get_image_t il2cpp_assembly_get_image = nullptr;
static il2cpp_class_from_name_t il2cpp_class_from_name = nullptr;
static il2cpp_class_get_method_from_name_t il2cpp_class_get_method_from_name = nullptr;
static il2cpp_runtime_invoke_t il2cpp_runtime_invoke = nullptr;
static il2cpp_thread_current_t il2cpp_thread_current = nullptr;
static il2cpp_thread_attach_t il2cpp_thread_attach = nullptr;
static il2cpp_string_chars_t il2cpp_string_chars = nullptr;
static il2cpp_array_length_t il2cpp_array_length = nullptr;
static il2cpp_array_new_t il2cpp_array_new = nullptr;

static void* texKlass = nullptr;   // UnityEngine.Texture2D
static void* resKlass = nullptr;   // UnityEngine.Resources
static void* byteKlass = nullptr;  // System.Byte
static void* objKlass = nullptr;   // UnityEngine.Object
static void* rtKlass = nullptr;    // UnityEngine.RenderTexture
static void* gfxKlass = nullptr;   // UnityEngine.Graphics

static void* mi_findAll = nullptr;     // Resources.FindObjectsOfTypeAll(Type)
static void* mi_getTypeObj = nullptr;  // Object.GetType()
static void* mi_objGetName = nullptr;  // Object.get_name()
static void* mi_encodePNG = nullptr;   // Texture2D.EncodeToPNG()
static void* mi_loadImage = nullptr;   // Texture2D.LoadImage(byte[])
static void* mi_apply = nullptr;       // Texture2D.Apply(bool, bool)
static void* mi_isReadable = nullptr;  // Texture2D.get_isReadable()
static void* mi_texCtor = nullptr;     // Texture2D..ctor(int,int,int,int)
static void* mi_readPixels = nullptr;  // Texture2D.ReadPixels(Rect,int,int)
static void* mi_rtTemp = nullptr;      // RenderTexture.GetTemporary(int,int,int,int)
static void* mi_blit = nullptr;        // Graphics.Blit(Texture, RenderTexture)
static void* mi_rtActiveSet = nullptr; // RenderTexture.set_active
static void* mi_rtRelease = nullptr;   // RenderTexture.ReleaseTemporary
static void* mi_objDestroy = nullptr;  // Object.Destroy

typedef int (*get_int_t)(void*);
static get_int_t tex_get_width = nullptr;
static get_int_t tex_get_height = nullptr;

static std::string g_dumpDir;
static std::string g_modDir;

// Папка приложения: Android/data/<пакет>/files — без разрешений на запись
static std::string GetAppBaseDir() {
    char cmd[256] = {0};
    FILE* f = fopen("/proc/self/cmdline", "rb");
    if (f) { fread(cmd, 1, sizeof(cmd) - 1, f); fclose(f); }
    std::string pkg(cmd);
    if (pkg.empty()) {
        LOGI(OBFUSCATE("pkg not found, fallback path"));
        return "/storage/emulated/0/texmod_app";
    }
    return std::string("/storage/emulated/0/Android/data/") + pkg + "/files";
}

static void ResolveIl2Cpp() {
    void *h = dlopen(OBFUSCATE("libil2cpp.so"), RTLD_NOW | RTLD_GLOBAL);
    if (!h) return;

    il2cpp_domain_get = (il2cpp_domain_get_t) dlsym(h, OBFUSCATE("il2cpp_domain_get"));
    il2cpp_domain_get_assemblies = (il2cpp_domain_get_assemblies_t) dlsym(h, OBFUSCATE("il2cpp_domain_get_assemblies"));
    il2cpp_assembly_get_image = (il2cpp_assembly_get_image_t) dlsym(h, OBFUSCATE("il2cpp_assembly_get_image"));
    il2cpp_class_from_name = (il2cpp_class_from_name_t) dlsym(h, OBFUSCATE("il2cpp_class_from_name"));
    il2cpp_class_get_method_from_name = (il2cpp_class_get_method_from_name_t) dlsym(h, OBFUSCATE("il2cpp_class_get_method_from_name"));
    il2cpp_runtime_invoke = (il2cpp_runtime_invoke_t) dlsym(h, OBFUSCATE("il2cpp_runtime_invoke"));
    il2cpp_thread_current = (il2cpp_thread_current_t) dlsym(h, OBFUSCATE("il2cpp_thread_current"));
    il2cpp_thread_attach = (il2cpp_thread_attach_t) dlsym(h, OBFUSCATE("il2cpp_thread_attach"));
    il2cpp_string_chars = (il2cpp_string_chars_t) dlsym(h, OBFUSCATE("il2cpp_string_chars"));
    il2cpp_array_length = (il2cpp_array_length_t) dlsym(h, OBFUSCATE("il2cpp_array_length"));
    il2cpp_array_new = (il2cpp_array_new_t) dlsym(h, OBFUSCATE("il2cpp_array_new"));

    if (!il2cpp_domain_get || !il2cpp_class_from_name || !il2cpp_runtime_invoke) return;

    void *domain = il2cpp_domain_get();
    size_t count = 0;
    void **assemblies = il2cpp_domain_get_assemblies(domain, &count);
    for (size_t i = 0; i < count; i++) {
        void *image = il2cpp_assembly_get_image(assemblies[i]);
        if (!image) continue;
        if (!texKlass)  texKlass  = il2cpp_class_from_name(image, OBFUSCATE("UnityEngine"), OBFUSCATE("Texture2D"));
        if (!resKlass)  resKlass  = il2cpp_class_from_name(image, OBFUSCATE("UnityEngine"), OBFUSCATE("Resources"));
        if (!byteKlass) byteKlass = il2cpp_class_from_name(image, OBFUSCATE("System"), OBFUSCATE("Byte"));
        if (!objKlass)  objKlass  = il2cpp_class_from_name(image, OBFUSCATE("UnityEngine"), OBFUSCATE("Object"));
        if (!rtKlass)   rtKlass   = il2cpp_class_from_name(image, OBFUSCATE("UnityEngine"), OBFUSCATE("RenderTexture"));
        if (!gfxKlass)  gfxKlass  = il2cpp_class_from_name(image, OBFUSCATE("UnityEngine"), OBFUSCATE("Graphics"));
    }
    if (!texKlass || !resKlass || !byteKlass || !objKlass || !rtKlass || !gfxKlass) return;

    mi_findAll      = il2cpp_class_get_method_from_name(resKlass, OBFUSCATE("FindObjectsOfTypeAll"), 1);
    mi_getTypeObj   = il2cpp_class_get_method_from_name(objKlass, OBFUSCATE("GetType"), 0);
    mi_objGetName   = il2cpp_class_get_method_from_name(objKlass, OBFUSCATE("get_name"), 0);
    mi_encodePNG    = il2cpp_class_get_method_from_name(texKlass, OBFUSCATE("EncodeToPNG"), 0);
    mi_loadImage    = il2cpp_class_get_method_from_name(texKlass, OBFUSCATE("LoadImage"), 1);
    mi_apply        = il2cpp_class_get_method_from_name(texKlass, OBFUSCATE("Apply"), 2);
    mi_isReadable   = il2cpp_class_get_method_from_name(texKlass, OBFUSCATE("get_isReadable"), 0);
    mi_texCtor      = il2cpp_class_get_method_from_name(texKlass, OBFUSCATE(".ctor"), 4);
    mi_readPixels   = il2cpp_class_get_method_from_name(texKlass, OBFUSCATE("ReadPixels"), 3);
    mi_rtTemp       = il2cpp_class_get_method_from_name(rtKlass, OBFUSCATE("GetTemporary"), 4);
    mi_blit         = il2cpp_class_get_method_from_name(gfxKlass, OBFUSCATE("Blit"), 2);
    mi_rtActiveSet  = il2cpp_class_get_method_from_name(rtKlass, OBFUSCATE("set_active"), 1);
    mi_rtRelease    = il2cpp_class_get_method_from_name(rtKlass, OBFUSCATE("ReleaseTemporary"), 1);
    mi_objDestroy   = il2cpp_class_get_method_from_name(objKlass, OBFUSCATE("Destroy"), 1);

    auto resolve = (void* (*)(const char*)) dlsym(h, OBFUSCATE("il2cpp_resolve_icall"));
    if (resolve) {
        tex_get_width  = (get_int_t) resolve(OBFUSCATE("UnityEngine.Texture2D::get_width"));
        tex_get_height = (get_int_t) resolve(OBFUSCATE("UnityEngine.Texture2D::get_height"));
    }
}

static std::vector<std::pair<std::string, void*>> g_textures;

static void ScanTextures() {
    g_textures.clear();
    if (!mi_findAll || !mi_getTypeObj || !mi_objGetName || !mi_texCtor) return;
    il2cpp_thread_attach(il2cpp_thread_current());

    // временная текстура 2x2 как якорь, через её GetType получаем Type(Texture2D)
    int cargs[4] = {2, 2, 4, 0};
    void* anchor = il2cpp_runtime_invoke(mi_texCtor, nullptr, (void**)cargs, nullptr);
    if (!anchor) return;

    void* typeObj = il2cpp_runtime_invoke(mi_getTypeObj, anchor, nullptr, nullptr);
    if (!typeObj) return;

    void* args[1] = { typeObj };
    void* arr = il2cpp_runtime_invoke(mi_findAll, nullptr, args, nullptr);
    if (!arr) return;

    uint32_t n = il2cpp_array_length(arr);
    for (uint32_t i = 0; i < n; i++) {
        void* tex = *(void**)((uintptr_t)arr + 0x20 + i * sizeof(void*));
        if (!tex) continue;
        void* sname = il2cpp_runtime_invoke(mi_objGetName, tex, nullptr, nullptr);
        if (!sname) continue;
        g_textures.emplace_back(il2cpp_string_chars(sname), tex);
    }
}

static std::string SanitizeName(std::string s) {
    for (auto& c : s)
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>')
            c = '_';
    return s;
}

static void SavePNG(void* pngArr, const std::string& name, int& saved, int& skipped) {
    if (!pngArr) { skipped++; return; }
    uint32_t len = il2cpp_array_length(pngArr);
    const uint8_t* data = (const uint8_t*)((uintptr_t)pngArr + 0x20);

    std::string path = g_dumpDir + "/" + SanitizeName(name) + ".png";
    FILE* f = fopen(path.c_str(), "wb");
    if (f) { fwrite(data, 1, len, f); fclose(f); saved++; }
    else skipped++;
}

// RT-blit: вытаскиваем пиксели из non-readable текстуры
static void DumpViaBlit(void* tex, const std::string& name, int& saved, int& skipped) {
    if (!mi_rtTemp || !mi_blit || !mi_readPixels || !mi_rtActiveSet || !mi_rtRelease ||
        !mi_texCtor || !mi_encodePNG || !mi_objDestroy || !tex_get_width || !tex_get_height)
    { skipped++; return; }

    int w = tex_get_width(tex), h = tex_get_height(tex);
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) { skipped++; return; }

    int rargs[4] = { w, h, 0, 0 }; // depth=0, формат по умолчанию
    void* rt = il2cpp_runtime_invoke(mi_rtTemp, nullptr, (void**)rargs, nullptr);
    if (!rt) { skipped++; return; }

    void* bargs[2] = { tex, rt };
    il2cpp_runtime_invoke(mi_blit, nullptr, bargs, nullptr);

    void* setActiveArgs[1] = { rt };
    il2cpp_runtime_invoke(mi_rtActiveSet, nullptr, setActiveArgs, nullptr);

    int cargs[4] = { w, h, 4, 0 }; // RGBA32, читаемый
    void* tex2 = il2cpp_runtime_invoke(mi_texCtor, nullptr, (void**)cargs, nullptr);

    void* pngArr = nullptr;
    if (tex2) {
        float rect[4] = { 0.0f, 0.0f, (float)w, (float)h }; // Rect — value type
        void* rpargs[3] = { rect, (void*)(intptr_t)0, (void*)(intptr_t)0 };
        il2cpp_runtime_invoke(mi_readPixels, tex2, rpargs, nullptr);

        uint8_t t = 1, upd = 0;
        void* aargs[2] = { &t, &upd };
        il2cpp_runtime_invoke(mi_apply, tex2, aargs, nullptr);

        pngArr = il2cpp_runtime_invoke(mi_encodePNG, tex2, nullptr, nullptr);

        void* destroyArgs[1] = { tex2 };
        il2cpp_runtime_invoke(mi_objDestroy, nullptr, destroyArgs, nullptr);
    }

    void* nullRt = nullptr;
    void* setNullArgs[1] = { nullRt };
    il2cpp_runtime_invoke(mi_rtActiveSet, nullptr, setNullArgs, nullptr);

    void* releaseArgs[1] = { rt };
    il2cpp_runtime_invoke(mi_rtRelease, nullptr, releaseArgs, nullptr);

    SavePNG(pngArr, name, saved, skipped);
}

// Проверка: есть ли уже дамп (папка существует и не пустая)
static bool DumpExists() {
    DIR* d = opendir(g_dumpDir.c_str());
    if (!d) return false;
    bool hasFiles = false;
    struct dirent* e;
    while ((e = readdir(d)) != nullptr) {
        if (e->d_name[0] != '.') { hasFiles = true; break; }
    }
    closedir(d);
    return hasFiles;
}

static void DumpTextures() {
    mkdir(g_dumpDir.c_str(), 0777);
    ScanTextures();
    if (!mi_encodePNG || !mi_isReadable) return;

    int saved = 0, skipped = 0;
    for (auto& [name, tex] : g_textures) {
        if (!tex) continue;

        void* readable = il2cpp_runtime_invoke(mi_isReadable, tex, nullptr, nullptr);
        bool isReadable = readable && *(uint8_t*)readable;

        if (isReadable) {
            void* pngArr = il2cpp_runtime_invoke(mi_encodePNG, tex, nullptr, nullptr);
            SavePNG(pngArr, name, saved, skipped);
        } else {
            // read/write off или сжатая — через RenderTexture
            DumpViaBlit(tex, name, saved, skipped);
        }
    }
    LOGI(OBFUSCATE("dump done: saved=%d skipped=%d"), saved, skipped);
}

static void ApplyCustomTextures() {
    if (!mi_loadImage || !mi_apply || !il2cpp_array_new) return;
    ScanTextures();

    DIR* d = opendir(g_modDir.c_str());
    if (!d) { LOGI(OBFUSCATE("texmod dir missing")); return; }

    struct dirent* e;
    int applied = 0;
    while ((e = readdir(d)) != nullptr) {
        const char* fn = e->d_name;
        size_t ln = strlen(fn);
        if (ln < 5 || strcmp(fn + ln - 4, OBFUSCATE(".png")) != 0) continue;
        std::string name(fn, ln - 4);

        for (auto& [tname, tex] : g_textures) {
            if (tname != name || !tex) continue;

            std::string path = g_modDir + "/" + fn;
            FILE* f = fopen(path.c_str(), "rb");
            if (!f) break;
            fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
            std::vector<char> buf(sz);
            if (sz > 0 && fread(buf.data(), 1, sz, f) != (size_t)sz) { fclose(f); break; }
            fclose(f);

            void* marr = il2cpp_array_new(byteKlass, sz, 0);
            if (!marr) break;
            memcpy((void*)((uintptr_t)marr + 0x20), buf.data(), sz);

            void* largs[1] = { marr };
            il2cpp_runtime_invoke(mi_loadImage, tex, largs, nullptr);

            uint8_t t = 1, upd = 0;
            void* aargs[2] = { &t, &upd };
            il2cpp_runtime_invoke(mi_apply, tex, aargs, nullptr);
            applied++;
            break;
        }
    }
    closedir(d);
    LOGI(OBFUSCATE("applied=%d"), applied);
}

// ===================== Заглушки JNI (нужны Setup.cpp для линковки) =====================
// Сигнатуры сверены с ld.lld: undefined symbol из последней сборки.

void Init(JNIEnv *env, jobject thiz, jobject ctx, jobject title, jobject subtitle) {
    (void)env; (void)thiz; (void)ctx; (void)title; (void)subtitle;
    // визуала нет — весь функционал автоматический
}

jstring Icon(JNIEnv *env, jobject thiz) {
    (void)thiz;
    return env->NewStringUTF(OBFUSCATE(""));
}

jstring IconWebViewData(JNIEnv *env, jobject thiz) {
    (void)env; (void)thiz;
    return nullptr;
}

jobjectArray SettingsList(JNIEnv *env, jobject activityObject) {
    (void)activityObject;
    jclass strClass = env->FindClass(OBFUSCATE("java/lang/String"));
    return env->NewObjectArray(0, strClass, nullptr);
}

jobjectArray GetFeatureList(JNIEnv *env, jobject context) {
    (void)context;
    jclass strClass = env->FindClass(OBFUSCATE("java/lang/String"));
    return env->NewObjectArray(0, strClass, nullptr);
}

void Changes(JNIEnv *env, jclass clazz, jobject obj, jint featNum, jstring featName,
             jint value, jlong Lvalue, jboolean boolean, jstring text) {
    (void)env; (void)clazz; (void)obj; (void)featNum; (void)featName;
    (void)value; (void)Lvalue; (void)boolean; (void)text;
}

// ===================== Главный поток =====================

void hack_thread() {
    // ждём загрузку libil2cpp
    while (!dlopen(OBFUSCATE("libil2cpp.so"), RTLD_NOLOAD)) {
        sleep(1);
    }
    sleep(20); // даём игре прогрузить сцену и текстуры

    ResolveIl2Cpp();
    if (!mi_findAll) { LOGI(OBFUSCATE("resolve failed")); return; }

    g_dumpDir = GetAppBaseDir() + "/texdump";
    g_modDir  = GetAppBaseDir() + "/texmod";

    il2cpp_thread_attach(il2cpp_thread_current());

    if (!DumpExists()) {
        LOGI(OBFUSCATE("no dump found, dumping..."));
        DumpTextures();
    } else {
        LOGI(OBFUSCATE("dump exists, skip"));
    }

    ApplyCustomTextures();

    // повторяем каждые 60 сек — подхватываются скины новых сцен
    while (true) {
        sleep(60);
        ApplyCustomTextures();
    }
}

