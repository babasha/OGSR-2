// The updater on Android (see upd_http.h, xr_update.h): the transfer, the APK install and the restart are Java's --
// com.ogsr.online.OgsrUpdate (android/java), reached through JNI. HttpURLConnection brings the system's TLS and CA
// store; PackageInstaller is the only way an app can hand the system a new version of itself.
#include "stdafx.h"

#ifdef __ANDROID__
#include "upd_http.h"
#include "upd_android.h"

#include <jni.h>
#include <SDL2/SDL_system.h>
#include <mutex>
#include <stdio.h>

namespace
{
std::mutex s_lock;
jclass s_cls{}; // global ref
jlongArray s_active_prog{}; // the transfer in flight: [0] done, [1] total (Java writes), [2] cancel (we write)
upd_progress* s_active_p{};

void seterr(char* err, int n, const char* msg)
{
    if (err && n > 0)
        snprintf(err, size_t(n), "%s", msg);
}

// ⚠FindClass from a thread the JVM did not start (the updater's worker) searches the SYSTEM class loader and does not
// see the app's classes. Go through the activity's own loader instead.
JNIEnv* env_and_class(jclass& cls)
{
    JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    if (!env)
        return nullptr;
    std::lock_guard<std::mutex> g(s_lock);
    if (!s_cls)
    {
        jobject act = static_cast<jobject>(SDL_AndroidGetActivity());
        if (!act)
            return nullptr;
        jclass act_cls = env->GetObjectClass(act);
        jmethodID get_cl = env->GetMethodID(act_cls, "getClassLoader", "()Ljava/lang/ClassLoader;");
        jobject loader = env->CallObjectMethod(act, get_cl);
        jclass loader_cls = env->FindClass("java/lang/ClassLoader");
        jmethodID load = env->GetMethodID(loader_cls, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
        jstring name = env->NewStringUTF("com.ogsr.online.OgsrUpdate");
        jobject c = env->CallObjectMethod(loader, load, name);
        if (env->ExceptionCheck())
        {
            env->ExceptionDescribe();
            env->ExceptionClear();
            c = nullptr;
        }
        if (c)
            s_cls = static_cast<jclass>(env->NewGlobalRef(c));
        env->DeleteLocalRef(name);
        env->DeleteLocalRef(loader_cls);
        env->DeleteLocalRef(loader);
        env->DeleteLocalRef(act_cls);
        env->DeleteLocalRef(act);
        if (c)
            env->DeleteLocalRef(c);
    }
    cls = s_cls;
    return cls ? env : nullptr;
}

bool clear_exception(JNIEnv* env)
{
    if (!env->ExceptionCheck())
        return false;
    env->ExceptionDescribe();
    env->ExceptionClear();
    return true;
}

void last_error(JNIEnv* env, jclass cls, char* err, int n)
{
    jmethodID m = env->GetStaticMethodID(cls, "lastError", "()Ljava/lang/String;");
    jstring s = m ? static_cast<jstring>(env->CallStaticObjectMethod(cls, m)) : nullptr;
    if (clear_exception(env) || !s)
    {
        seterr(err, n, "java error");
        return;
    }
    const char* c = env->GetStringUTFChars(s, nullptr);
    seterr(err, n, c ? c : "java error");
    if (c)
        env->ReleaseStringUTFChars(s, c);
    env->DeleteLocalRef(s);
}
} // namespace

extern "C" int upd_http_get(const char* url, const char* dst_path, upd_progress* p, char* err, int err_size)
{
    if (p)
        p->done = 0, p->total = -1;
    jclass cls;
    JNIEnv* env = env_and_class(cls);
    if (!env)
    {
        seterr(err, err_size, "no JNI / OgsrUpdate class");
        return -1;
    }
    jmethodID m = env->GetStaticMethodID(cls, "download", "(Ljava/lang/String;Ljava/lang/String;[J)I");
    if (!m)
    {
        clear_exception(env);
        seterr(err, err_size, "OgsrUpdate.download missing");
        return -1;
    }
    // [0] done, [1] total, [2] cancel: Java writes the first two and polls the third per chunk.
    jlongArray prog = env->NewLongArray(3);
    jlongArray prog_g = static_cast<jlongArray>(env->NewGlobalRef(prog));
    env->DeleteLocalRef(prog);
    jstring jurl = env->NewStringUTF(url);
    jstring jdst = env->NewStringUTF(dst_path);

    {
        std::lock_guard<std::mutex> g(s_lock);
        s_active_prog = prog_g;
        s_active_p = p;
    }
    const jint rc = env->CallStaticIntMethod(cls, m, jurl, jdst, prog_g);
    const bool threw = clear_exception(env);
    {
        std::lock_guard<std::mutex> g(s_lock);
        s_active_prog = nullptr;
        s_active_p = nullptr;
    }
    jlong v[3]{};
    env->GetLongArrayRegion(prog_g, 0, 3, v);
    if (p)
        p->done = v[0], p->total = v[1];
    if (threw || rc != 200)
    {
        if (threw)
            seterr(err, err_size, "java exception");
        else if (rc > 0)
            snprintf(err, size_t(err_size), "HTTP %d", int(rc));
        else
            last_error(env, cls, err, err_size);
        remove(dst_path);
    }
    env->DeleteLocalRef(jurl);
    env->DeleteLocalRef(jdst);
    env->DeleteGlobalRef(prog_g);
    return (!threw && rc == 200) ? 0 : -1;
}

// From the menu's thread (GetInfo): the transfer's counters into its upd_progress, our cancel flag into Java's.
void upd_android_poll()
{
    JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    if (!env)
        return;
    std::lock_guard<std::mutex> g(s_lock);
    if (!s_active_prog || !s_active_p)
        return;
    jlong v[2]{};
    env->GetLongArrayRegion(s_active_prog, 0, 2, v);
    s_active_p->done = v[0];
    s_active_p->total = v[1];
    const jlong c = s_active_p->cancel ? 1 : 0;
    env->SetLongArrayRegion(s_active_prog, 2, 1, &c);
}

long long upd_android_version_code()
{
    jclass cls;
    JNIEnv* env = env_and_class(cls);
    if (!env)
        return -1;
    jmethodID m = env->GetStaticMethodID(cls, "versionCode", "()J");
    const jlong v = m ? env->CallStaticLongMethod(cls, m) : -1;
    return clear_exception(env) ? -1 : v;
}

int upd_android_install_apk(const char* path, char* err, int err_size)
{
    jclass cls;
    JNIEnv* env = env_and_class(cls);
    if (!env)
    {
        seterr(err, err_size, "no JNI / OgsrUpdate class");
        return -1;
    }
    jmethodID m = env->GetStaticMethodID(cls, "installApk", "(Ljava/lang/String;)I");
    jstring jp = env->NewStringUTF(path);
    const jint rc = m ? env->CallStaticIntMethod(cls, m, jp) : -1;
    const bool threw = clear_exception(env);
    env->DeleteLocalRef(jp);
    if (threw)
        seterr(err, err_size, "java exception");
    else if (rc != 0)
        last_error(env, cls, err, err_size);
    return threw ? -1 : rc;
}

bool upd_android_schedule_restart()
{
    jclass cls;
    JNIEnv* env = env_and_class(cls);
    if (!env)
        return false;
    jmethodID m = env->GetStaticMethodID(cls, "scheduleRestart", "()Z");
    const jboolean ok = m ? env->CallStaticBooleanMethod(cls, m) : JNI_FALSE;
    return !clear_exception(env) && ok;
}
#endif
