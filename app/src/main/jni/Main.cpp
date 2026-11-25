// ===================== TexMod: auto dump + auto apply =====================

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
#include <android/log.h>   // <-- обязательно, даёт ANDROID_LOG_INFO и __android_log_print

#include "Includes/obfuscate.h"

#ifndef ANDROID_LOG_INFO
#define ANDROID_LOG_INFO 4   // страховка, если log.h не подхватился
#endif

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "TEXMOD", __VA_ARGS__)

