// Ground truth for JNIEnv vtable calls: `[x1 + off]` indirect calls through
// the function table every native method receives, as opposed to
// source_import.c's PLT/GOT calls. There is no `jni.h` in a standalone NDK
// toolchain install, so this file carries its own minimal copy of the JNI
// types -- laid out byte-for-byte like the real (ABI-frozen since 1.2) table
// so the offsets this compiles match the ones
// types/presets/android-ndk.hdecl declares for the same eight members. Only
// those eight are named on either side; every other slot is `void*`
// padding, which costs nothing but the count since a pointer and a function
// pointer are the same width.
//
// Baseline only, same reason as source_env.c: `inferTargetProfile` already
// supplies android-ndk (and with it, this struct) without anyone asking.

#include <stdint.h>
#include <stddef.h>

#if defined(__ANDROID__)

typedef uint8_t jboolean;
typedef int32_t jint;
typedef struct _jobject *jobject;
typedef jobject jclass;
typedef jobject jstring;
typedef struct _jmethodID *jmethodID;

typedef struct {
  const char *name;
  const char *signature;
  void *fnPtr;
} JNINativeMethod;

struct JNINativeInterface;
typedef const struct JNINativeInterface *JNIEnv;

typedef jclass (*JNI_FindClassFn)(JNIEnv *env, const char *name);
typedef jclass (*JNI_GetObjectClassFn)(JNIEnv *env, jobject obj);
typedef jmethodID (*JNI_GetMethodIDFn)(JNIEnv *env, jclass clazz,
                                       const char *name, const char *sig);
typedef jobject (*JNI_CallObjectMethodFn)(JNIEnv *env, jobject obj,
                                           jmethodID methodID, ...);
typedef jint (*JNI_CallIntMethodFn)(JNIEnv *env, jobject obj,
                                     jmethodID methodID, ...);
typedef jstring (*JNI_NewStringUTFFn)(JNIEnv *env, const char *bytes);
typedef jint (*JNI_RegisterNativesFn)(JNIEnv *env, jclass clazz,
                                      const JNINativeMethod *methods,
                                      jint nMethods);
typedef jboolean (*JNI_ExceptionCheckFn)(JNIEnv *env);

// Same order and same padding counts as android-ndk.hdecl's copy -- see that
// file for the index of each named member.
struct JNINativeInterface {
  void *pad0[6];
  JNI_FindClassFn FindClass;
  void *pad1[24];
  JNI_GetObjectClassFn GetObjectClass;
  void *pad2[1];
  JNI_GetMethodIDFn GetMethodID;
  JNI_CallObjectMethodFn CallObjectMethod;
  void *pad3[14];
  JNI_CallIntMethodFn CallIntMethod;
  void *pad4[117];
  JNI_NewStringUTFFn NewStringUTF;
  void *pad5[47];
  JNI_RegisterNativesFn RegisterNatives;
  void *pad6[12];
  JNI_ExceptionCheckFn ExceptionCheck;
  void *pad7[4];
};

// Each case is one call through the table, plus a trivial use of the result
// so the compiler cannot fold the `blr` into a tail `br` -- the same reason
// source_env.c's cases never end in a bare `return call(...)`.

// A pointer result compared only against NULL is an identity as far as the
// compiler is concerned (`x ? x : 0` is just `x`), so -O1 folds these calls
// straight into tail calls -- the `blr` becomes a `br` and the case stops
// testing member-offset resolution and starts testing recover-tailcall
// instead. Comparing against an all-ones sentinel the callee could never
// plausibly return, but which the compiler cannot rule out at compile time,
// keeps the two return paths distinct enough that neither call folds away.
#define EVAL_JNI_SENTINEL ((uintptr_t)-1)

jclass eval_jni_find_class(JNIEnv *env, const char *name) {
  jclass cls = (*env)->FindClass(env, name);
  if ((uintptr_t)cls == EVAL_JNI_SENTINEL) {
    return (jclass)0;
  }
  return cls;
}

jclass eval_jni_get_object_class(JNIEnv *env, jobject obj) {
  jclass cls = (*env)->GetObjectClass(env, obj);
  if ((uintptr_t)cls == EVAL_JNI_SENTINEL) {
    return (jclass)0;
  }
  return cls;
}

jmethodID eval_jni_get_method_id(JNIEnv *env, jclass clazz, const char *name,
                                  const char *sig) {
  jmethodID mid = (*env)->GetMethodID(env, clazz, name, sig);
  if ((uintptr_t)mid == EVAL_JNI_SENTINEL) {
    return (jmethodID)0;
  }
  return mid;
}

jobject eval_jni_call_object_method(JNIEnv *env, jobject obj,
                                     jmethodID methodID) {
  jobject result = (*env)->CallObjectMethod(env, obj, methodID);
  if ((uintptr_t)result == EVAL_JNI_SENTINEL) {
    return (jobject)0;
  }
  return result;
}

jint eval_jni_call_int_method(JNIEnv *env, jobject obj, jmethodID methodID) {
  jint result = (*env)->CallIntMethod(env, obj, methodID);
  return result >= 0 ? result : -1;
}

jstring eval_jni_new_string_utf(JNIEnv *env, const char *bytes) {
  jstring s = (*env)->NewStringUTF(env, bytes);
  if ((uintptr_t)s == EVAL_JNI_SENTINEL) {
    return (jstring)0;
  }
  return s;
}

jint eval_jni_register_natives(JNIEnv *env, jclass clazz,
                                const JNINativeMethod *methods, jint n) {
  jint rc = (*env)->RegisterNatives(env, clazz, methods, n);
  return rc == 0 ? 0 : -1;
}

jboolean eval_jni_exception_check(JNIEnv *env) {
  jboolean pending = (*env)->ExceptionCheck(env);
  return pending ? (jboolean)1 : (jboolean)0;
}

#else

// The corpus is built for arm64-v8a; these keep a host build compiling.
typedef int32_t jint;
typedef void *jclass;
typedef void *jobject;
typedef void *jstring;
typedef void *jmethodID;
typedef int32_t jboolean;
typedef struct { const char *name; const char *signature; void *fnPtr; } JNINativeMethod;
typedef void *JNIEnv;

jclass eval_jni_find_class(JNIEnv *env, const char *name) { (void)env; (void)name; return NULL; }
jclass eval_jni_get_object_class(JNIEnv *env, jobject obj) { (void)env; (void)obj; return NULL; }
jmethodID eval_jni_get_method_id(JNIEnv *env, jclass clazz, const char *name, const char *sig) {
  (void)env; (void)clazz; (void)name; (void)sig; return NULL;
}
jobject eval_jni_call_object_method(JNIEnv *env, jobject obj, jmethodID methodID) {
  (void)env; (void)obj; (void)methodID; return NULL;
}
jint eval_jni_call_int_method(JNIEnv *env, jobject obj, jmethodID methodID) {
  (void)env; (void)obj; (void)methodID; return 0;
}
jstring eval_jni_new_string_utf(JNIEnv *env, const char *bytes) { (void)env; (void)bytes; return NULL; }
jint eval_jni_register_natives(JNIEnv *env, jclass clazz, const JNINativeMethod *methods, jint n) {
  (void)env; (void)clazz; (void)methods; return n;
}
jboolean eval_jni_exception_check(JNIEnv *env) { (void)env; return 0; }

#endif
