// Ground truth for the class of call a reverse engineer meets constantly and
// this project barely had a corpus for: a native library reading its own
// environment. Every function below calls exactly one libc/Bionic API (or one
// short, obviously-related pair, such as open+read of the same /proc file) and
// does something with the result that a plain `return callee(...);` would not
// -- because at -O1 that shape tail-calls straight through the PLT stub
// (eval_tailcall_import's shape), and what these cases are pinned to instead
// is the ordinary shape a real probe has: `bl <import>@plt`, then a compare on
// what came back. Every one of these is import resolution's Shape A
// (docs/10-import-resolution.md) exercised against a different Bionic symbol,
// scored by baseline alone: no `--types` argument is passed anywhere in
// eval/run.ps1, because `TargetProfile` already loads the `android-ndk`
// preset for any AArch64 ELF it opens.
//
// No parameters, no fields read back: every case takes its own buffer on the
// stack and reports success/length/identity as a plain int32_t, which keeps
// the manifest's signature check to "zero params, 32-bit return" throughout
// and puts the whole burden of the case on the one thing it exists to check --
// whether the call prints under its real name instead of `sub_<va>`.

#include <stdint.h>

#if defined(__ANDROID__)

#include <android/log.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <stddef.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/system_properties.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

// ---------------------------------------------------------------------------
// System properties: Android's device-fingerprint store.
// ---------------------------------------------------------------------------

int32_t eval_env_property_get_model(void) {
  char value[PROP_VALUE_MAX];
  const int32_t len = __system_property_get("ro.product.model", value);
  return len > 0 ? len : -1;
}

int32_t eval_env_property_get_sdk(void) {
  char value[PROP_VALUE_MAX];
  const int32_t len = __system_property_get("ro.build.version.sdk", value);
  return len > 0 ? len : -1;
}

int32_t eval_env_property_get_serial(void) {
  char value[PROP_VALUE_MAX];
  const int32_t len = __system_property_get("ro.serialno", value);
  return len > 0 ? len : -1;
}

int32_t eval_env_property_find(void) {
  return __system_property_find("ro.product.brand") != NULL;
}

// ---------------------------------------------------------------------------
// /proc and filesystem probes.
// ---------------------------------------------------------------------------

int32_t eval_env_open_cmdline(void) {
  const int fd = open("/proc/self/cmdline", O_RDONLY);
  if (fd < 0) {
    return -1;
  }
  close(fd);
  return fd;
}

int32_t eval_env_read_maps(void) {
  char buf[128];
  const int fd = open("/proc/self/maps", O_RDONLY);
  if (fd < 0) {
    return -1;
  }
  const ssize_t n = read(fd, buf, sizeof(buf));
  close(fd);
  return n < 0 ? -1 : (int32_t)n;
}

int32_t eval_env_read_status(void) {
  char buf[128];
  const int fd = open("/proc/self/status", O_RDONLY);
  if (fd < 0) {
    return -1;
  }
  const ssize_t n = read(fd, buf, sizeof(buf));
  close(fd);
  return n < 0 ? -1 : (int32_t)n;
}

int32_t eval_env_read_cpuinfo(void) {
  char buf[128];
  const int fd = open("/proc/cpuinfo", O_RDONLY);
  if (fd < 0) {
    return -1;
  }
  const ssize_t n = read(fd, buf, sizeof(buf));
  close(fd);
  return n < 0 ? -1 : (int32_t)n;
}

int32_t eval_env_access_root(void) {
  return access("/", F_OK) == 0;
}

int32_t eval_env_access_data(void) {
  return access("/data", R_OK) == 0;
}

int32_t eval_env_readlink_exe(void) {
  char buf[256];
  const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf));
  return n > 0 ? (int32_t)n : -1;
}

int32_t eval_env_stat_libc(void) {
  struct stat st;
  return stat("/system/lib64/libc.so", &st) == 0;
}

// ---------------------------------------------------------------------------
// Time.
// ---------------------------------------------------------------------------

int32_t eval_env_gettimeofday(void) {
  struct timeval tv;
  const int rc = gettimeofday(&tv, NULL);
  return rc < 0 ? -1 : 0;
}

int32_t eval_env_clock_gettime(void) {
  struct timespec ts;
  const int rc = clock_gettime(CLOCK_REALTIME, &ts);
  return rc < 0 ? -1 : 0;
}

int32_t eval_env_time(void) {
  const time_t now = time(NULL);
  return now > 0 ? 1 : 0;
}

int32_t eval_env_usleep(void) {
  const int rc = usleep(1000);
  return rc < 0 ? -1 : 0;
}

int32_t eval_env_nanosleep(void) {
  struct timespec req = {0, 1000000};
  const int rc = nanosleep(&req, NULL);
  return rc < 0 ? -1 : 0;
}

// ---------------------------------------------------------------------------
// Process, thread and identity.
// ---------------------------------------------------------------------------

int32_t eval_env_getpid(void) {
  const pid_t pid = getpid();
  return pid > 0 ? (int32_t)pid : -1;
}

int32_t eval_env_gettid(void) {
  const pid_t tid = gettid();
  return tid > 0 ? (int32_t)tid : -1;
}

int32_t eval_env_getuid(void) {
  // >= 10000: Android's boundary for an app UID, the emulator/root check this
  // is actually written for -- and, incidentally, why the comparison cannot
  // collapse back into a plain `return (int32_t)uid;` the way an identity
  // clamp could.
  const uid_t uid = getuid();
  return uid >= 10000;
}

int32_t eval_env_pthread_self(void) {
  const pthread_t self = pthread_self();
  return self != 0;
}

int32_t eval_env_prctl(void) {
  const int rc = prctl(PR_GET_DUMPABLE, 0, 0, 0, 0);
  return rc < 0 ? -1 : rc;
}

// ---------------------------------------------------------------------------
// Logging and dynamic-library probes.
// ---------------------------------------------------------------------------

int32_t eval_env_log_print(void) {
  const int rc = __android_log_print(ANDROID_LOG_INFO, "xdec", "probe %d", 1);
  return rc < 0 ? -1 : rc;
}

int32_t eval_env_log_write(void) {
  const int rc = __android_log_write(ANDROID_LOG_INFO, "xdec", "probe");
  return rc < 0 ? -1 : rc;
}

int32_t eval_env_dlopen(void) {
  void* handle = dlopen("liblog.so", RTLD_NOW);
  return handle != NULL;
}

int32_t eval_env_dlsym(void) {
  void* handle = dlopen("liblog.so", RTLD_NOW);
  if (handle == NULL) {
    return -1;
  }
  void* sym = dlsym(handle, "__android_log_print");
  return sym != NULL;
}

// ---------------------------------------------------------------------------
// Anti-debug, raw syscalls and memory mapping.
// ---------------------------------------------------------------------------

int32_t eval_env_ptrace(void) {
  const long rc = ptrace(PTRACE_TRACEME, 0, NULL, NULL);
  return rc < 0 ? -1 : 0;
}

int32_t eval_env_syscall_getpid(void) {
  const long pid = syscall(SYS_getpid);
  return pid > 0 ? 1 : 0;
}

int32_t eval_env_mmap(void) {
  void* p = mmap(NULL, 4096, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  return p != MAP_FAILED;
}

int32_t eval_env_getenv(void) {
  const char* path = getenv("PATH");
  return path != NULL;
}

#else

// The corpus is built for arm64-v8a; these keep a host build compiling.
int32_t eval_env_property_get_model(void) { return -1; }
int32_t eval_env_property_get_sdk(void) { return -1; }
int32_t eval_env_property_get_serial(void) { return -1; }
int32_t eval_env_property_find(void) { return 0; }
int32_t eval_env_open_cmdline(void) { return -1; }
int32_t eval_env_read_maps(void) { return -1; }
int32_t eval_env_read_status(void) { return -1; }
int32_t eval_env_read_cpuinfo(void) { return -1; }
int32_t eval_env_access_root(void) { return 0; }
int32_t eval_env_access_data(void) { return 0; }
int32_t eval_env_readlink_exe(void) { return -1; }
int32_t eval_env_stat_libc(void) { return 0; }
int32_t eval_env_gettimeofday(void) { return -1; }
int32_t eval_env_clock_gettime(void) { return -1; }
int32_t eval_env_time(void) { return 0; }
int32_t eval_env_usleep(void) { return -1; }
int32_t eval_env_nanosleep(void) { return -1; }
int32_t eval_env_getpid(void) { return -1; }
int32_t eval_env_gettid(void) { return -1; }
int32_t eval_env_getuid(void) { return 0; }
int32_t eval_env_pthread_self(void) { return 0; }
int32_t eval_env_prctl(void) { return -1; }
int32_t eval_env_log_print(void) { return -1; }
int32_t eval_env_log_write(void) { return -1; }
int32_t eval_env_dlopen(void) { return 0; }
int32_t eval_env_dlsym(void) { return -1; }
int32_t eval_env_ptrace(void) { return -1; }
int32_t eval_env_syscall_getpid(void) { return 0; }
int32_t eval_env_mmap(void) { return 0; }
int32_t eval_env_getenv(void) { return 0; }

#endif
