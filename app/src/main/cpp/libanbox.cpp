#include <jni.h>
#include <string>
#include <cstdint>
#include <cstdlib>
#include <unistd.h>
#include <android/input.h>
#include "anbox/graphics/emugl/Renderer.h"
#include "anbox/graphics/emugl/RenderApi.h"
#include "anbox/graphics/emugl/RenderControl.h"
#include "anbox/network/published_socket_connector.h"
#include "anbox/qemu/pipe_connection_creator.h"
#include "anbox/runtime.h"
#include "anbox/common/dispatcher.h"
#include "anbox/input/manager.h"
#include "anbox/input/device.h"
#include "anbox/graphics/layer_composer.h"
#include "anbox/graphics/emugl/DisplayManager.h"
#include "external/android-emugl/shared/emugl/common/logging.h"
#include <android/log.h>
#include <android/native_window_jni.h>

// Server mode includes
#include <iostream>
#include <sstream>
#include <csignal>
#include <cstring>
#include <getopt.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <limits.h>
#include <thread>
#include <linux/input.h>
#include <errno.h>
#include <fcntl.h>
#include "anbox/logger.h"
#include "anbox/application/sensors_state.h"
#include "anbox/application/gps_info_broker.h"
#include "server/streaming_server.h"
#include "server/streaming_protocol.h"
#include "server/streaming_layer_composer.h"
#include "server/adb_forwarder.h"
#include <boost/exception/diagnostic_information.hpp>

#define TAG "libAnbox"

//const char *const path = "/data/data/com.github.ananbox/files";

// ============================================================================
// Global variables shared between JNI mode and standalone server mode
// All functions and variables are accessible in both modes for maximum reuse
// ============================================================================
static const int MAX_FINGERS = 10;
static const int MAX_TRACKING_ID = 10;
static int touch_slots[MAX_FINGERS];
static int last_slot = -1;

// Core runtime components - shared between both modes
static std::shared_ptr<anbox::Runtime> rt;
static std::shared_ptr<anbox::graphics::Rect> frame = std::make_shared<anbox::graphics::Rect>();
static std::shared_ptr<::Renderer> renderer_;
static std::shared_ptr<anbox::network::PublishedSocketConnector> qemu_pipe_connector_;
static std::shared_ptr<anbox::input::Device> touch_;
static std::shared_ptr<anbox::server::StreamingLayerComposer> streaming_composer_;

// JNI-specific globals
static ANativeWindow* native_window;
static char path[255];

// Server-specific globals
static volatile bool running = true;
static pid_t container_pid = -1;
static pid_t logcat_pid = -1;

// Unified logging callback for emugl - works in both JNI and server modes
void logger_write(const emugl::LogLevel &level, const char *format, ...) {
    char message[2048];
    va_list args;

    va_start(args, format);
    vsnprintf(message, sizeof(message) - 1, format, args);
    va_end(args);

    switch (level) {
        case emugl::LogLevel::WARNING:
#ifdef __ANDROID__
            __android_log_print(ANDROID_LOG_WARN, TAG, "%s", message);
#else
            WARNING("%s", message);
#endif
            break;
        case emugl::LogLevel::ERROR:
        case emugl::LogLevel::FATAL:
#ifdef __ANDROID__
            __android_log_print(ANDROID_LOG_ERROR, TAG, "%s", message);
#else
            ERROR("%s", message);
#endif
            break;
        case emugl::LogLevel::DEBUG:
#ifdef __ANDROID__
            __android_log_print(ANDROID_LOG_DEBUG, TAG, "%s", message);
#else
            DEBUG("%s", message);
#endif
            break;
        case emugl::LogLevel::TRACE:
            // Verbose logging disabled by default
            break;
        default:
            break;
    }
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_github_ananbox_Anbox_stringFromJNI(
        JNIEnv* env,
        jobject /* this */) {
    std::string hello = "Hello from C++";
    return env->NewStringUTF(hello.c_str());
}

extern "C"
JNIEXPORT void JNICALL
Java_com_github_ananbox_Anbox_startRuntime(
        JNIEnv *env,
        jobject thiz) {
    rt->start();
}

extern "C"
JNIEXPORT void JNICALL
Java_com_github_ananbox_Anbox_stopRuntime(JNIEnv *env, jobject thiz) {
    if (rt != nullptr) {
        rt->stop();
        rt = nullptr;
    }
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_github_ananbox_Anbox_initRuntime(
        JNIEnv* env,
        jobject thiz,
        jint width,
        jint height,
        jint dpi) {
//    auto gl_libs = anbox::graphics::emugl::default_gl_libraries();
//    if (!anbox::graphics::emugl::initialize(gl_libs, nullptr, nullptr)) {
//        __android_log_print(ANDROID_LOG_ERROR, TAG, "Failed to initialize OpenGL renderer");
//        return false;
//    }
    if (rt != NULL)
        return false;
    set_emugl_logger(logger_write);
    set_emugl_cxt_logger(logger_write);

    std::uint32_t flags = 0;

    rt = anbox::Runtime::create();

    renderer_ = std::make_shared<::Renderer>();
//    native_window = ANativeWindow_fromSurface(env, surface);
//    int32_t width_ = ANativeWindow_getWidth(native_window);
//    int32_t height_ = ANativeWindow_getHeight(native_window);
    frame->resize(width, height);
    auto display_info_ = anbox::graphics::emugl::DisplayInfo::get();
    display_info_->set_resolution(width, height);
    display_info_->set_dpi(dpi);

    renderer_->initialize(EGL_DEFAULT_DISPLAY);
    registerRenderer(renderer_);

    auto sensors_state = std::make_shared<anbox::application::SensorsState>();
    auto gps_info_broker = std::make_shared<anbox::application::GpsInfoBroker>();

    auto input_manager = std::make_shared<anbox::input::Manager>(rt, anbox::utils::string_format("%s/rootfs/dev/input", path));
//    auto pointer_ = input_manager->create_device();
//    pointer_->set_name("anbox-pointer");
//    pointer_->set_driver_version(1);
//    pointer_->set_input_id({BUS_VIRTUAL, 2, 2, 2});
//    pointer_->set_physical_location("none");
//    pointer_->set_key_bit(BTN_MOUSE);
//    // NOTE: We don't use REL_X/REL_Y in reality but have to specify them here
//    // to allow InputFlinger to detect we're a cursor device.
//    pointer_->set_rel_bit(REL_X);
//    pointer_->set_rel_bit(REL_Y);
//    pointer_->set_rel_bit(REL_HWHEEL);
//    pointer_->set_rel_bit(REL_WHEEL);
//    pointer_->set_prop_bit(INPUT_PROP_POINTER);

//    auto keyboard_ = input_manager->create_device();
//    keyboard_->set_name("anbox-keyboard");
//    keyboard_->set_driver_version(1);
//    keyboard_->set_input_id({BUS_VIRTUAL, 3, 3, 3});
//    keyboard_->set_physical_location("none");
//    keyboard_->set_key_bit(BTN_MISC);
//    keyboard_->set_key_bit(KEY_OK);

    touch_ = input_manager->create_device();
    touch_->set_name("anbox-touch");
    touch_->set_driver_version(1);
    touch_->set_input_id({BUS_VIRTUAL, 4, 4, 4});
    touch_->set_physical_location("none");
    touch_->set_abs_bit(ABS_MT_SLOT);
    touch_->set_abs_max(ABS_MT_SLOT, 10);
    touch_->set_abs_bit(ABS_MT_TOUCH_MAJOR);
    touch_->set_abs_max(ABS_MT_TOUCH_MAJOR, 127);
    touch_->set_abs_bit(ABS_MT_TOUCH_MINOR);
    touch_->set_abs_max(ABS_MT_TOUCH_MINOR, 127);
    touch_->set_abs_bit(ABS_MT_POSITION_X);
    touch_->set_abs_max(ABS_MT_POSITION_X, width);
    touch_->set_abs_bit(ABS_MT_POSITION_Y);
    touch_->set_abs_max(ABS_MT_POSITION_Y, height);
    touch_->set_abs_bit(ABS_MT_TRACKING_ID);
    touch_->set_abs_max(ABS_MT_TRACKING_ID, MAX_TRACKING_ID);
    touch_->set_prop_bit(INPUT_PROP_DIRECT);

    // delete qemu_pipe if exists
    std::string socket_file = anbox::utils::string_format("%s/qemu_pipe", path);
    unlink(socket_file.c_str());
    qemu_pipe_connector_ =
            std::make_shared<anbox::network::PublishedSocketConnector>(
                    anbox::utils::string_format("%s/qemu_pipe", path), rt,
                    std::make_shared<anbox::qemu::PipeConnectionCreator>(renderer_, rt, sensors_state, gps_info_broker));

    return true;
}
extern "C"
JNIEXPORT void JNICALL
Java_com_github_ananbox_Anbox_startContainer(JNIEnv *env, jobject thiz, jstring proot_, jint verbose_level, jstring init_path_) {
    if (fork() != 0) {
        return;
    }
    sigset_t signals_to_unblock;
    sigfillset(&signals_to_unblock);
    sigprocmask(SIG_UNBLOCK, &signals_to_unblock, 0);
    
    const char *proot = env->GetStringUTFChars(proot_, 0);
    const char *init_path = env->GetStringUTFChars(init_path_, 0);
    
    // Convert verbose level to string for proot -v option
    char verbose_str[8];
    snprintf(verbose_str, sizeof(verbose_str), "%d", verbose_level);
    
    // Extract the native library directory from the proot path
    // proot path is like "/data/app/.../lib/arm64/libproot.so"
    char native_lib_dir[PATH_MAX];
    size_t proot_len = strlen(proot);
    
    // Validate the path length to prevent buffer overflow
    if (proot_len >= PATH_MAX) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "proot path too long");
        env->ReleaseStringUTFChars(proot_, proot);
        env->ReleaseStringUTFChars(init_path_, init_path);
        _exit(1);
    }
    
    // Use snprintf for safe string copying
    snprintf(native_lib_dir, sizeof(native_lib_dir), "%s", proot);
    char *last_slash = strrchr(native_lib_dir, '/');
    if (last_slash != nullptr) {
        *last_slash = '\0';  // Remove the filename, keep directory
    } else {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "Invalid proot path: no directory separator found");
        env->ReleaseStringUTFChars(proot_, proot);
        env->ReleaseStringUTFChars(init_path_, init_path);
        _exit(1);
    }
    
    // Build rootfs path
    char rootfs_path[PATH_MAX];
    snprintf(rootfs_path, sizeof(rootfs_path), "%s/rootfs", path);
    
    // Prepare container environment (matching run.sh script)
    __android_log_print(ANDROID_LOG_INFO, TAG, "Preparing container environment...");
    
    // Clean up old files
    char system_log[PATH_MAX];
    snprintf(system_log, sizeof(system_log), "%s/data/system.log", rootfs_path);
    unlink(system_log);
    
    // Remove and recreate socket directories
    char dev_socket[PATH_MAX];
    snprintf(dev_socket, sizeof(dev_socket), "%s/dev/socket", rootfs_path);
    char rm_cmd[PATH_MAX + 32];
    snprintf(rm_cmd, sizeof(rm_cmd), "rm -rf %s", dev_socket);
    system(rm_cmd);
    mkdir(dev_socket, 0755);
    
    char dev_properties[PATH_MAX];
    snprintf(dev_properties, sizeof(dev_properties), "%s/dev/__properties__", rootfs_path);
    snprintf(rm_cmd, sizeof(rm_cmd), "rm -rf %s", dev_properties);
    system(rm_cmd);
    mkdir(dev_properties, 0755);
    
    // Remove and recreate device files
    char dev_kmsg[PATH_MAX];
    char dev_pmsg0[PATH_MAX];
    snprintf(dev_kmsg, sizeof(dev_kmsg), "%s/dev/kmsg", rootfs_path);
    snprintf(dev_pmsg0, sizeof(dev_pmsg0), "%s/dev/pmsg0", rootfs_path);
    unlink(dev_kmsg);
    unlink(dev_pmsg0);
    
    // Create empty device files
    int fd = open(dev_kmsg, O_CREAT | O_WRONLY, 0644);
    if (fd >= 0) close(fd);
    fd = open(dev_pmsg0, O_CREAT | O_WRONLY, 0644);
    if (fd >= 0) close(fd);
    
    // Create necessary data directories
    char data_media[PATH_MAX];
    char data_system_ce[PATH_MAX];
    char data_misc_ce[PATH_MAX];
    snprintf(data_media, sizeof(data_media), "%s/data/media/0", rootfs_path);
    snprintf(data_system_ce, sizeof(data_system_ce), "%s/data/system_ce/0", rootfs_path);
    snprintf(data_misc_ce, sizeof(data_misc_ce), "%s/data/misc_ce/0", rootfs_path);
    
    char mkdir_cmd[PATH_MAX + 32];
    snprintf(mkdir_cmd, sizeof(mkdir_cmd), "mkdir -p %s", data_media);
    system(mkdir_cmd);
    snprintf(mkdir_cmd, sizeof(mkdir_cmd), "mkdir -p %s", data_system_ce);
    system(mkdir_cmd);
    snprintf(mkdir_cmd, sizeof(mkdir_cmd), "mkdir -p %s", data_misc_ce);
    system(mkdir_cmd);
    
    __android_log_print(ANDROID_LOG_INFO, TAG, "Container environment prepared");
    
    // Change to rootfs directory before running proot (as in run.sh)
    if (chdir(rootfs_path) != 0) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "Failed to change to rootfs directory %s: %s", 
                           rootfs_path, strerror(errno));
        env->ReleaseStringUTFChars(proot_, proot);
        env->ReleaseStringUTFChars(init_path_, init_path);
        _exit(1);
    }
    
    // Set Android environment variables (as in run.sh)
    setenv("PATH", "/sbin:/system/bin:/system/sbin:/system/xbin:/system/vendor/bin", 1);
    setenv("ANDROID_ASSETS", "/assets", 1);
    setenv("ANDROID_DATA", "/data", 1);
    setenv("ANDROID_ROOT", "/system", 1);
    setenv("ANDROID_STORAGE", "/storage", 1);
    setenv("ASEC_MOUNTPOINT", "/mnt/asec", 1);
    setenv("EXTERNAL_STORAGE", "/sdcard", 1);
    
    // Set PROOT_TMP_DIR to ./tmp (relative to rootfs, matching run.sh)
    setenv("PROOT_TMP_DIR", "./tmp", 1);
    
    // Set PROOT_LOADER to the pre-built loader in the native library directory
    // The native lib dir has exec permission, so the loader can run from there
    char loader_path[PATH_MAX];
    snprintf(loader_path, sizeof(loader_path), "%s/libproot-loader.so", native_lib_dir);
    setenv("PROOT_LOADER", loader_path, 1);
    __android_log_print(ANDROID_LOG_INFO, TAG, "Using PROOT_LOADER: %s", loader_path);
    
    // Build proot command matching run.sh exactly (now with configurable options)
    // Since we chdir to rootfs, paths are relative to rootfs
    // qemu_pipe is in parent directory, so use ../qemu_pipe
    const char* proot_args[] = {
        proot,
        "--kill-on-exit",
        "-r", ".",  // Current directory (rootfs)
        "-0",       // Fake root
        "-w", "/",  // Working directory inside container
        "-b", "/dev",
        "-b", "/proc",
        "-b", "dev/kmsg:/dev/kmsg",
        "-b", "dev/pmsg0:/dev/pmsg0",
        "-b", "system/vendor:/vendor",
        "-b", "dev/__properties__:/dev/__properties__",
        "-b", "dev/socket:/dev/socket",
        "-b", "/dev/binder",
        "-b", "/dev/ashmem",
        "-b", "../qemu_pipe:/dev/qemu_pipe",  // qemu_pipe is in parent directory
        "-b", "dev/input:/dev/input",
        "-b", "mnt/user/0:/storage/self",
        "-v", verbose_str,  // Configurable verbose level
        init_path,          // Configurable init path
        nullptr
    };
    
    __android_log_print(ANDROID_LOG_INFO, TAG, "Starting container with proot (JNI mode)");
    __android_log_print(ANDROID_LOG_INFO, TAG, "Working directory: %s", rootfs_path);
    __android_log_print(ANDROID_LOG_INFO, TAG, "Verbose level: %s", verbose_str);
    __android_log_print(ANDROID_LOG_INFO, TAG, "Init path: %s", init_path);
    __android_log_print(ANDROID_LOG_INFO, TAG, "PROOT_TMP_DIR: ./tmp");
    
    // Display full proot command for debugging
    __android_log_print(ANDROID_LOG_INFO, TAG, "Command: %s --kill-on-exit -r . -0 -w / -b /dev -b /proc -b dev/kmsg:/dev/kmsg -b dev/pmsg0:/dev/pmsg0 -b system/vendor:/vendor -b dev/__properties__:/dev/__properties__ -b dev/socket:/dev/socket -b /dev/binder -b /dev/ashmem -b ../qemu_pipe:/dev/qemu_pipe -b dev/input:/dev/input -b mnt/user/0:/storage/self -v %s %s", proot, verbose_str, init_path);
    
    // Redirect stdout/stderr to a log file for debugging
    char log_path[PATH_MAX];
    snprintf(log_path, sizeof(log_path), "%s/proot.log", path);
    int log_fd = open(log_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (log_fd >= 0) {
        dup2(log_fd, STDOUT_FILENO);
        dup2(log_fd, STDERR_FILENO);
        close(log_fd);
        __android_log_print(ANDROID_LOG_INFO, TAG, "Redirected proot output to %s", log_path);
    } else {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "Failed to open log file %s: %s", log_path, strerror(errno));
    }

    env->ReleaseStringUTFChars(proot_, proot);
    env->ReleaseStringUTFChars(init_path_, init_path);
    execvp(proot_args[0], const_cast<char* const*>(proot_args));
    
    // If execvp fails, write error to log file too
    fprintf(stderr, "Failed to start container: %s\n", strerror(errno));
    __android_log_print(ANDROID_LOG_ERROR, TAG, "Failed to start container: %s", strerror(errno));
    _exit(1);
 }
extern "C"
JNIEXPORT void JNICALL
Java_com_github_ananbox_Anbox_resetWindow(JNIEnv *env, jobject thiz, jint height, jint width) {
    // TODO: check why change frame size cause nothing to be displayed
//    frame->resize(width, height);
    anbox::graphics::emugl::DisplayInfo::get()->set_resolution(height, width);
}

int find_touch_slot(int id){
    for (int i = 0; i < MAX_FINGERS; i++) {
        if (touch_slots[i] == id)
            return i;
    }
    return -1;
}

void push_slot(std::vector<anbox::input::Event> &touch_events, int slot){
    if (last_slot != slot) {
        touch_events.push_back({EV_ABS, ABS_MT_SLOT, slot});
        last_slot = slot;
    }
}

extern "C"
JNIEXPORT void JNICALL
Java_com_github_ananbox_Anbox_pushFingerUp(JNIEnv *env, jobject thiz, jint finger_id) {
    std::vector<anbox::input::Event> touch_events;
    int slot = find_touch_slot(finger_id);
    if (slot == -1)
        return;
    push_slot(touch_events, slot);
    touch_events.push_back({EV_ABS, ABS_MT_TRACKING_ID, -1});
    touch_events.push_back({EV_SYN, SYN_REPORT, 0});
    touch_slots[slot] = -1;
    touch_->send_events(touch_events);
}

extern "C"
JNIEXPORT void JNICALL
Java_com_github_ananbox_Anbox_pushFingerDown(JNIEnv *env, jobject thiz, jint x, jint y, jint finger_id) {
    std::vector<anbox::input::Event> touch_events;
    int slot = find_touch_slot(-1);
    if (slot == -1) {
        DEBUG("no free slot!");
        return;
    }
    touch_slots[slot] = finger_id;
    push_slot(touch_events, slot);
    touch_events.push_back({EV_ABS, ABS_MT_TRACKING_ID, static_cast<std::int32_t>(finger_id % MAX_TRACKING_ID + 1)});
    touch_events.push_back({EV_ABS, ABS_MT_POSITION_X, x});
    touch_events.push_back({EV_ABS, ABS_MT_POSITION_Y, y});
    touch_events.push_back({EV_SYN, SYN_REPORT, 0});
    touch_->send_events(touch_events);
}

extern "C"
JNIEXPORT void JNICALL
Java_com_github_ananbox_Anbox_pushFingerMotion(JNIEnv *env, jobject thiz, jint x, jint y,
                                               jint finger_id) {
    std::vector<anbox::input::Event> touch_events;
    int slot = find_touch_slot(finger_id);
    if (slot == -1)
        return;
    push_slot(touch_events, slot);
    touch_events.push_back({EV_ABS, ABS_MT_POSITION_X, x});
    touch_events.push_back({EV_ABS, ABS_MT_POSITION_Y, y});
    touch_events.push_back({EV_SYN, SYN_REPORT, 0});
    touch_->send_events(touch_events);
}

extern "C"
JNIEXPORT void JNICALL
Java_com_github_ananbox_Anbox_destroyWindow(JNIEnv *env, jobject thiz) {
//    getRenderer()->destroyAllNativeWindow();
}

extern "C"
JNIEXPORT void JNICALL
Java_com_github_ananbox_Anbox_createSurface(JNIEnv *env, jobject thiz, jobject surface) {
    native_window = ANativeWindow_fromSurface(env, surface);
    renderer_->createNativeWindow(native_window);
    auto composer_ = std::make_shared<anbox::graphics::LayerComposer>(renderer_, frame, native_window);
    registerLayerComposer(composer_);
}

extern "C"
JNIEXPORT void JNICALL
Java_com_github_ananbox_Anbox_destroySurface(JNIEnv *env, jobject thiz) {
    unRegisterLayerComposer();
    renderer_->destroyNativeWindow(native_window);
    ANativeWindow_release(native_window);
    native_window = NULL;
}

extern "C"
JNIEXPORT void JNICALL
Java_com_github_ananbox_Anbox_setPath(JNIEnv *env, jobject thiz, jstring path_) {
    const char *pathStr = env->GetStringUTFChars(path_, 0);
    memcpy(path, pathStr, strlen(pathStr) + 1);
    env->ReleaseStringUTFChars(path_, pathStr);
}

// ============================================================================
// Standalone Server Mode
// When executed as a standalone binary (not loaded as JNI library), this
// provides a complete server for running Android containers with network
// streaming support.
// Merged from anbox/src/server/main.cpp - all globals and functions are now
// shared between JNI mode and server mode for maximum code reuse.
// ============================================================================

void signal_handler(int signum) {
    INFO("Received signal %d, shutting down...", signum);
    running = false;
    
    // Forward signal to child processes if running
    if (logcat_pid > 0) {
        kill(logcat_pid, signum);
    }
    if (container_pid > 0) {
        kill(container_pid, signum);
    }
}

void print_usage(const char* program) {
    std::cout << "Usage: " << program << " [options]\n"
              << "\n"
              << "Ananbox - Rootless Android container server with network streaming\n"
              << "Run Android containers in Termux and stream display/input over the network\n"
              << "\n"
              << "═══════════════════════════════════════════════════════════════════════\n"
              << "DISPLAY & CONTAINER\n"
              << "═══════════════════════════════════════════════════════════════════════\n"
              << "  -s, --size <WxHxD>       Display size (WIDTH x HEIGHT x DPI)\n"
              << "                           Default: 1280x720x160\n"
              << "                           Examples: 1080x1920x320, 720x1280x240\n"
              << "\n"
              << "  -r, --rootfs <path>      Android rootfs directory (REQUIRED)\n"
              << "                           Must contain: init, system/, data/, dev/, etc.\n"
              << "                           Example: /data/data/com.termux/ananbox/rootfs\n"
              << "\n"
              << "  -o, --proot <path>       Path to proot or libproot.so\n"
              << "                           Default: ./libproot.so\n"
              << "\n"
              << "  -t, --proottmp <path>    Proot temporary directory\n"
              << "                           Default: <rootfs>/tmp\n"
              << "\n"
              << "  -q, --qemu <command>     QEMU command for cross-platform execution\n"
              << "                           Used when running non-native binaries\n"
              << "                           Example: qemu-arm, qemu-aarch64\n"
              << "\n"
              << "  -i, --init <path>        Init binary to execute in container\n"
              << "                           Default: /init\n"
              << "\n"
              << "  -u, --runsh <path>       Use custom run.sh script instead of direct proot\n"
              << "                           When specified, invokes the script with rootfs parent\n"
              << "                           and proot paths as arguments\n"
              << "\n"
              << "═══════════════════════════════════════════════════════════════════════\n"
              << "ADB FORWARDING\n"
              << "═══════════════════════════════════════════════════════════════════════\n"
              << "  -p, --adbport <port>     ADB listen port\n"
              << "                           Default: 5555 (0 = disable)\n"
              << "\n"
              << "  -b, --adbbind <ip>       ADB bind address\n"
              << "                           Default: 127.0.0.1\n"
              << "\n"
              << "  -k, --adbsocket <path>   ADB socket path in container\n"
              << "                           Default: /dev/socket/adbd\n"
              << "\n"
              << "═══════════════════════════════════════════════════════════════════════\n"
              << "STREAMING SERVER\n"
              << "═══════════════════════════════════════════════════════════════════════\n"
              << "  -m, --streamport <port>  Streaming server port\n"
              << "                           Default: " << anbox::server::DEFAULT_PORT << "\n"
              << "\n"
              << "  -n, --streambind <ip>    Streaming server bind address\n"
              << "                           Default: 0.0.0.0 (all interfaces)\n"
              << "\n"
              << "═══════════════════════════════════════════════════════════════════════\n"
              << "LOGGING\n"
              << "═══════════════════════════════════════════════════════════════════════\n"
              << "  -l, --prootlog <dest>    Proot process log output\n"
              << "                           Use ?prefix for special destinations:\n"
              << "                             ?stderr  - standard error\n"
              << "                             ?stdout  - standard output\n"
              << "                             ?null    - suppress output\n"
              << "                           Without ?: write to file with that name\n"
              << "\n"
              << "  -c, --containerlogcat <dest>\n"
              << "                           Container logcat output\n"
              << "                           Same syntax as --prootlog\n"
              << "\n"
              << "  -v, --verbose <level>    Verbosity level\n"
              << "                           Options: quiet, normal, verbose, extra\n"
              << "                           Default: verbose\n"
              << "\n"
              << "  -a, --customparameter <params>\n"
              << "                           Custom proot parameters (overrides defaults)\n"
              << "                           When specified, replaces all default proot options\n"
              << "                           Example: \"--kill-on-exit -r . -b /dev -b /proc\"\n"
              << "\n"
              << "  -d, --directory <path>   Working directory for proot launch\n"
              << "                           Default: rootfs directory\n"
              << "                           Example: /data/data/com.termux/ananbox\n"
              << "\n"
              << "  -e, --end                Stop parsing options - all remaining args passed to proot\n"
              << "                           Useful for passing additional arguments directly to proot\n"
              << "                           Example: ... -e -b /custom:/custom /custom/init\n"
              << "\n"
              << "═══════════════════════════════════════════════════════════════════════\n"
              << "OTHER\n"
              << "═══════════════════════════════════════════════════════════════════════\n"
              << "  -h, --help               Show this help message\n"
              << "\n"
              << "═══════════════════════════════════════════════════════════════════════\n"
              << "EXAMPLES\n"
              << "═══════════════════════════════════════════════════════════════════════\n"
              << "Basic usage:\n"
              << "  " << program << " -r /data/data/com.termux/ananbox/rootfs\n"
              << "\n"
              << "With custom display and ports:\n"
              << "  " << program << " -r ~/ananbox/rootfs -s 1080x1920x320 -m 55558 -p 55551\n"
              << "\n"
              << "With logging:\n"
              << "  " << program << " -r ~/ananbox/rootfs -l ?stderr -c ?stdout\n"
              << "\n"
              << "Cross-platform with QEMU:\n"
              << "  " << program << " -r ~/ananbox/rootfs -q qemu-arm\n"
              << "\n"
              << "Quiet mode:\n"
              << "  " << program << " -r ~/ananbox/rootfs -v quiet\n"
              << "\n"
              << "Custom proot parameters:\n"
              << "  " << program << " -r ~/ananbox/rootfs -a \"-r . -0 -w / -b /dev\"\n"
              << "\n"
              << "Custom working directory:\n"
              << "  " << program << " -r ~/ananbox/rootfs -d /data/data/com.termux\n"
              << "\n"
              << "Pass extra args to proot:\n"
              << "  " << program << " -r ~/ananbox/rootfs -e -b /custom:/custom /system/bin/sh\n"
              << "\n"
              << "═══════════════════════════════════════════════════════════════════════\n"
              << "NOTES\n"
              << "═══════════════════════════════════════════════════════════════════════\n"
              << "• The server requires proot for rootless container execution\n"
              << "• Arguments can be stacked: -bpm 127.0.0.1 55551 55558\n"
              << "• Container startup may take several seconds on first run\n"
              << "• Press Ctrl+C to gracefully shutdown the server and container\n"
              << "• Use -a for complete control over proot invocation\n"
              << "• Use -e to append additional arguments to the default proot command\n"
              << "\n"
              << "For more information, see the documentation in the repository.\n"
              << std::endl;
}

// Normalize a path - removes trailing slashes and handles relative paths
std::string normalize_path(const std::string& path) {
    std::string result = path;
    
    // Remove trailing slashes
    while (result.length() > 1 && result.back() == '/') {
        result.pop_back();
    }
    
    return result;
}

// Get the parent directory of a path
std::string get_parent_dir(const std::string& path) {
    size_t pos = path.rfind('/');
    if (pos == std::string::npos || pos == 0) {
        return ".";
    }
    return path.substr(0, pos);
}

// Create directory if it doesn't exist
bool ensure_directory(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) == 0) {
        return S_ISDIR(st.st_mode);
    }
    return mkdir(path.c_str(), 0755) == 0;
}

// Main entry point for standalone server mode
int main(int argc, char* argv[]) {
    try {
        // Print startup banner
        std::cout << "========================================" << std::endl;
        std::cout << "Ananbox Server Starting" << std::endl;
        std::cout << "Version: 0.2.9 (Merged PIE)" << std::endl;
        std::cout << "========================================" << std::endl;
        
        // Show command line
        std::cout << "Command line: ";
        for (int i = 0; i < argc; i++) {
            std::cout << argv[i] << " ";
        }
        std::cout << std::endl << std::endl;
        
        // Get current working directory as default base
        char cwd[PATH_MAX];
        if (getcwd(cwd, sizeof(cwd)) == nullptr) {
            std::cerr << "ERROR: Failed to get current directory" << std::endl;
            return 1;
        }
        std::cout << "Current working directory: " << cwd << std::endl;
    
    // Default values
    std::string display_spec = "1280x720x160";  // WIDTHxHEIGHTxDPI format
    std::string rootfs_path = "";  // Rootfs directory (required)
    std::string proot_path = "./libproot.so";  // Default to local libproot.so
    std::string proot_tmp_dir;  // Will default to <rootfs>/tmp if not specified
    std::string qemu_command;  // QEMU command for cross-platform execution (e.g., "qemu-arm")
    std::string init_path = "/init";  // Init binary to execute (default: /init)
    std::string runsh_path;  // Optional run.sh script path (empty = use direct proot invocation)
    uint16_t adb_port = 5555;  // Default ADB port (0 to disable)
    std::string adb_bind = "127.0.0.1";  // Default ADB bind address
    std::string adb_socket_path = "/dev/socket/adbd";  // Default ADB socket path (in container)
    uint16_t stream_port = anbox::server::DEFAULT_PORT;  // Streaming server port
    std::string stream_bind = "0.0.0.0";  // Streaming server bind address
    std::string proot_log_dest;  // Proot log destination (empty = disabled)
    std::string container_logcat_dest;  // Container logcat destination (empty = disabled)
    std::string verbose_level = "verbose";  // Default: verbose, options: quiet, normal, verbose, extra
    std::string custom_parameters;  // Custom proot parameters (overrides default proot args)
    std::string working_directory;  // Directory to launch proot in (empty = use rootfs)
    bool stop_parsing = false;  // Stop argument parsing and pass remaining args to proot

    static struct option long_options[] = {
        {"size",        required_argument, 0, 's'},
        {"rootfs",      required_argument, 0, 'r'},
        {"proot",       required_argument, 0, 'o'},
        {"proottmp",    required_argument, 0, 't'},
        {"qemu",        required_argument, 0, 'q'},
        {"init",        required_argument, 0, 'i'},
        {"runsh",       required_argument, 0, 'u'},
        {"adbport",     required_argument, 0, 'p'},
        {"adbbind",     required_argument, 0, 'b'},
        {"adbsocket",   required_argument, 0, 'k'},
        {"streamport",  required_argument, 0, 'm'},
        {"streambind",  required_argument, 0, 'n'},
        {"prootlog",    required_argument, 0, 'l'},
        {"containerlogcat", required_argument, 0, 'c'},
        {"verbose",     required_argument, 0, 'v'},
        {"customparameter", required_argument, 0, 'a'},
        {"directory",   required_argument, 0, 'd'},
        {"end",         no_argument,       0, 'e'},
        {"help",        no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    int option_index = 0;
    while ((opt = getopt_long(argc, argv, "s:r:o:t:q:i:u:p:b:k:m:n:l:c:v:a:d:eh", long_options, &option_index)) != -1) {
        switch (opt) {
            case 's':  // -s, --size
                display_spec = optarg;
                break;
            case 'r':  // -r, --rootfs
                rootfs_path = normalize_path(optarg);
                break;
            case 'o':  // -o, --proot
                proot_path = optarg;
                break;
            case 't':  // -t, --proottmp
                proot_tmp_dir = optarg;
                break;
            case 'q':  // -q, --qemu
                qemu_command = optarg;
                break;
            case 'i':  // -i, --init
                init_path = optarg;
                break;
            case 'u':  // -u, --runsh
                runsh_path = optarg;
                break;
            case 'p':  // -p, --adbport
                adb_port = static_cast<uint16_t>(std::stoi(optarg));
                break;
            case 'b':  // -b, --adbbind
                adb_bind = optarg;
                break;
            case 'k':  // -k, --adbsocket
                adb_socket_path = optarg;
                break;
            case 'm':  // -m, --streamport
                stream_port = static_cast<uint16_t>(std::stoi(optarg));
                break;
            case 'n':  // -n, --streambind
                stream_bind = optarg;
                break;
            case 'l':  // -l, --prootlog
                proot_log_dest = optarg;
                break;
            case 'c':  // -c, --containerlogcat
                container_logcat_dest = optarg;
                break;
            case 'v':  // -v, --verbose
                verbose_level = optarg;
                break;
            case 'a':  // -a, --customparameter
                custom_parameters = optarg;
                break;
            case 'd':  // -d, --directory
                working_directory = optarg;
                break;
            case 'e':  // -e, --end
                stop_parsing = true;
                break;
            case 'h':  // -h, --help
                print_usage(argv[0]);
                return 0;
            default:
                print_usage(argv[0]);
                return 1;
        }
        
        // If -e/--end was specified, stop parsing and collect remaining args
        if (stop_parsing) {
            break;
        }
    }
    
    // Collect remaining arguments after -e/--end for passing to proot
    std::vector<std::string> extra_proot_args;
    if (stop_parsing) {
        for (int i = optind; i < argc; i++) {
            extra_proot_args.push_back(argv[i]);
        }
    }

    // Parse verbose level
    enum class VerboseLevel {
        QUIET,    // No output except errors (default)
        NORMAL,   // Normal output
        VERBOSE,  // Verbose output
        EXTRA     // Extra verbose output
    };
    
    VerboseLevel verbose = VerboseLevel::VERBOSE;  // Default to verbose
    if (verbose_level == "quiet") {
        verbose = VerboseLevel::QUIET;
    } else if (verbose_level == "normal") {
        verbose = VerboseLevel::NORMAL;
    } else if (verbose_level == "verbose") {
        verbose = VerboseLevel::VERBOSE;
    } else if (verbose_level == "extra") {
        verbose = VerboseLevel::EXTRA;
    } else {
        std::cerr << "ERROR: Invalid verbose level: " << verbose_level << std::endl;
        std::cerr << "Valid options: quiet, normal, verbose, extra" << std::endl;
        return 1;
    }

    // Parse display specification (WIDTHxHEIGHTxDPI format)
    int display_width = 1280;
    int display_height = 720;
    int display_dpi = 160;
    
    if (!display_spec.empty()) {
        size_t first_x = display_spec.find('x');
        size_t second_x = display_spec.find('x', first_x + 1);
        
        if (first_x != std::string::npos && second_x != std::string::npos) {
            try {
                display_width = std::stoi(display_spec.substr(0, first_x));
                display_height = std::stoi(display_spec.substr(first_x + 1, second_x - first_x - 1));
                display_dpi = std::stoi(display_spec.substr(second_x + 1));
            } catch (const std::exception& e) {
                std::cerr << "ERROR: Invalid display specification: " << display_spec << std::endl;
                std::cerr << "Expected format: WIDTHxHEIGHTxDPI (e.g., 1080x1920x320)" << std::endl;
                return 1;
            }
        } else {
            std::cerr << "ERROR: Invalid display specification: " << display_spec << std::endl;
            std::cerr << "Expected format: WIDTHxHEIGHTxDPI (e.g., 1080x1920x320)" << std::endl;
            return 1;
        }
    }
    
    // Parse log destinations (handle ?prefix for special destinations)
    auto parse_log_dest = [](const std::string& dest) -> std::pair<std::string, bool> {
        if (dest.empty()) {
            return {"", false};
        }
        if (dest[0] == '?') {
            std::string special = dest.substr(1);
            if (special == "null") {
                return {"/dev/null", true};
            } else if (special == "stdout") {
                return {"stdout", true};
            } else if (special == "stderr") {
                return {"stderr", true};
            } else {
                return {dest, false};  // Invalid ?prefix, treat as filename
            }
        }
        return {dest, false};  // Regular filename
    };
    
    bool proot_log_is_special = false;
    bool container_logcat_is_special = false;
    if (!proot_log_dest.empty()) {
        auto parsed = parse_log_dest(proot_log_dest);
        proot_log_dest = parsed.first;
        proot_log_is_special = parsed.second;
    }
    if (!container_logcat_dest.empty()) {
        auto parsed = parse_log_dest(container_logcat_dest);
        container_logcat_dest = parsed.first;
        container_logcat_is_special = parsed.second;
    }

    // Validate required arguments
    if (rootfs_path.empty()) {
        std::cerr << "ERROR: Rootfs directory is required. Use -r or --rootfs" << std::endl;
        print_usage(argv[0]);
        return 1;
    }

    // Setup signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGCHLD, SIG_IGN);  // Ignore child exit to avoid zombies

    // Validate paths to prevent path traversal attacks
    auto validate_path = [](const std::string& path, const std::string& name) -> bool {
        // Check for null bytes
        if (path.find('\0') != std::string::npos) {
            ERROR("Invalid %s path: contains null byte", name.c_str());
            return false;
        }
        // Path should not be empty
        if (path.empty()) {
            ERROR("Invalid %s path: empty", name.c_str());
            return false;
        }
        return true;
    };
    
    if (!validate_path(rootfs_path, "rootfs") || !validate_path(proot_path, "proot")) {
        return 1;
    }

    // Normalize rootfs path
    rootfs_path = normalize_path(rootfs_path);

    // Check if rootfs directory exists
    struct stat st;
    if (stat(rootfs_path.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
        ERROR("Rootfs directory does not exist: %s", rootfs_path.c_str());
        return 1;
    }

    // Derive base_path from rootfs (parent directory)
    std::string base_path = get_parent_dir(rootfs_path);

    // Set default proot tmp directory if not specified
    if (proot_tmp_dir.empty()) {
        proot_tmp_dir = rootfs_path + "/tmp";
    }
    
    // Set qemu_pipe path (in parent of rootfs) for Android container communication
    std::string qemu_pipe_path = base_path + "/qemu_pipe";
    
    // Create proot tmp directory if it doesn't exist
    if (!ensure_directory(proot_tmp_dir)) {
        ERROR("Failed to create proot temporary directory: %s", proot_tmp_dir.c_str());
        return 1;
    }
    
    // Also create qemu_pipe parent directory
    std::string qemu_pipe_dir = get_parent_dir(qemu_pipe_path);
    ensure_directory(qemu_pipe_dir);

    INFO("========================================");
    // Show configuration (only if not quiet mode)
    const char* verbose_level_str = "quiet";  // Default matches VerboseLevel::QUIET
    if (verbose == VerboseLevel::QUIET) verbose_level_str = "quiet";
    else if (verbose == VerboseLevel::NORMAL) verbose_level_str = "normal";
    else if (verbose == VerboseLevel::VERBOSE) verbose_level_str = "verbose";
    else if (verbose == VerboseLevel::EXTRA) verbose_level_str = "extra";
    
    if (verbose != VerboseLevel::QUIET) {
        INFO("Ananbox Server starting...");
        INFO("========================================");
        INFO("Configuration:");
        INFO("  Streaming bind: %s", stream_bind.c_str());
        INFO("  Streaming port: %d", stream_port);
        INFO("  Display:        %dx%dx%d (WxHxD)", display_width, display_height, display_dpi);
        INFO("  Rootfs:         %s", rootfs_path.c_str());
        INFO("  Proot:          %s", proot_path.c_str());
        INFO("  Proot tmp:      %s", proot_tmp_dir.c_str());
        if (!qemu_command.empty()) {
            INFO("  QEMU command:   %s", qemu_command.c_str());
        }
        INFO("  QEMU pipe:      %s", qemu_pipe_path.c_str());
        if (!container_logcat_dest.empty()) {
            INFO("  Container log:  %s%s", container_logcat_is_special ? "?":"", container_logcat_dest.c_str());
        }
        if (!proot_log_dest.empty()) {
            INFO("  Proot log:      %s%s", proot_log_is_special ? "?":"", proot_log_dest.c_str());
        }
        if (adb_port > 0) {
            INFO("  ADB bind:       %s", adb_bind.c_str());
            INFO("  ADB port:       %d", adb_port);
            INFO("  ADB socket:     %s (in container)", adb_socket_path.c_str());
        } else {
            INFO("  ADB forwarding: disabled");
        }
        INFO("  Verbose level:  %s", verbose_level_str);
        INFO("========================================");
        INFO("Paths validated successfully");
        INFO("========================================");
    }

    // Initialize emugl logging with unified logger
    INFO("Initializing emugl logging...");
    set_emugl_logger(logger_write);
    set_emugl_cxt_logger(logger_write);
    INFO("emugl logging initialized");

    try {
        // Create runtime for async I/O
        INFO("Creating async I/O runtime...");
        INFO("Pool size: default");
        rt = anbox::Runtime::create();
        INFO("Runtime created successfully");
    } catch (const boost::exception& e) {
        ERROR("Failed to create runtime (boost::exception)");
        ERROR("Exception details: %s", boost::diagnostic_information(e).c_str());
        ERROR("This may indicate a system configuration issue or resource limitation");
        return 1;
    } catch (const std::exception& e) {
        ERROR("Failed to create runtime: %s", e.what());
        ERROR("This may indicate a system configuration issue or resource limitation");
        return 1;
    }

    // Initialize the Renderer (OpenGL ES emulation)
    INFO("Initializing OpenGL ES renderer...");
    renderer_ = std::make_shared<::Renderer>();
    frame = std::make_shared<anbox::graphics::Rect>();
    frame->resize(display_width, display_height);
    
    auto display_info = anbox::graphics::emugl::DisplayInfo::get();
    display_info->set_resolution(display_width, display_height);
    display_info->set_dpi(display_dpi);
    INFO("Display info configured: %dx%d @ %d DPI", display_width, display_height, display_dpi);

    // Initialize renderer with default display (may fail in headless mode)
    INFO("Attempting EGL initialization...");
    bool egl_initialized = renderer_->initialize(EGL_DEFAULT_DISPLAY);
    if (!egl_initialized) {
        WARNING("========================================");
        WARNING("EGL initialization failed!");
        WARNING("Running in software-only mode.");
        WARNING("========================================");
        INFO("In this mode:");
        INFO("  - Test pattern frames will be sent to clients");
        INFO("  - Container graphics require EGL/swiftshader");
        INFO("  - Install swiftshader in Termux for full graphics support");
        // Enable software renderer mode - stores color buffers in memory
        enableSoftwareRenderer(true);
    } else {
        INFO("========================================");
        INFO("EGL initialized successfully!");
        INFO("Container graphics will be captured and streamed.");
        INFO("========================================");
        // Register renderer for use
        registerRenderer(renderer_);
    }

    // Create streaming server
    std::shared_ptr<anbox::server::StreamingServer> streaming_server;
    std::shared_ptr<anbox::server::AdbForwarder> adb_forwarder;
    
    try {
        // Start the runtime (this spawns worker threads for async I/O)
        INFO("Starting async I/O runtime...");
        INFO("This will create worker threads for handling network I/O");
        rt->start();
        INFO("Runtime started successfully");
        
        // Create streaming server
        INFO("Creating streaming server...");
        INFO("Stream bind: %s, port: %d", stream_bind.c_str(), stream_port);
        streaming_server = std::make_shared<anbox::server::StreamingServer>(
            rt, stream_bind, stream_port);
        
        // Set display configuration
        INFO("Configuring display for streaming server...");
        streaming_server->set_display_config(display_width, display_height, display_dpi);
        INFO("Streaming server created");
        
        // Create streaming layer composer that will capture frames and send to clients
        // Pass nullptr for renderer when using software mode - it will use SoftwareColorBufferStore
        INFO("Creating streaming layer composer...");
        INFO("Using %s renderer", egl_initialized ? "hardware (EGL)" : "software");
        streaming_composer_ = std::make_shared<anbox::server::StreamingLayerComposer>(
            egl_initialized ? renderer_ : nullptr, frame);
        streaming_composer_->set_frame_callback(
            [streaming_server](const void* data, uint32_t width, uint32_t height, uint32_t stride) {
                streaming_server->send_frame(data, width, height, 
                    anbox::server::PIXEL_FORMAT_RGBA8888, stride);
            });
        INFO("Layer composer created and frame callback registered");
        
        // Register the streaming layer composer
        INFO("Registering layer composer with RenderControl...");
        registerLayerComposer(streaming_composer_);
        INFO("Layer composer registered with RenderControl");
        
        // Start the streaming server
        INFO("Starting streaming server listener...");
        streaming_server->start();
        
        INFO("========================================");
        INFO("Streaming server ready!");
        INFO("Listening on %s:%d", stream_bind.c_str(), stream_port);
        INFO("Waiting for client connections...");
        INFO("========================================");
        
        // Create and start ADB forwarder if enabled
        if (adb_port > 0) {
            INFO("Creating ADB forwarder...");
            INFO("ADB will forward connections from %s:%d to container socket", 
                 adb_bind.c_str(), adb_port);
            adb_forwarder = std::make_shared<anbox::server::AdbForwarder>(
                rt, adb_bind, adb_port, adb_socket_path, rootfs_path);
            adb_forwarder->start();
            
            INFO("========================================");
            INFO("ADB forwarding ready!");
            INFO("  ADB address: %s:%d", adb_bind.c_str(), adb_port);
            INFO("  To connect: adb connect %s:%d", adb_bind.c_str(), adb_port);
            INFO("  For scrcpy: scrcpy -s %s:%d", adb_bind.c_str(), adb_port);
            INFO("========================================");
        }
    } catch (const boost::exception& e) {
        ERROR("Failed to start streaming server (boost::exception)");
        ERROR("Exception details: %s", boost::diagnostic_information(e).c_str());
        // Kill container if running
        if (container_pid > 0) {
            kill(container_pid, SIGTERM);
        }
        if (rt) {
            rt->stop();
        }
        return 1;
    } catch (const std::exception& e) {
        ERROR("Failed to start streaming server: %s", e.what());
        // Kill container if running
        if (container_pid > 0) {
            kill(container_pid, SIGTERM);
        }
        if (rt) {
            rt->stop();
        }
        return 1;
    }

    // Initialize input manager for touch/key events from clients
    INFO("Setting up input device manager...");
    auto input_manager = std::make_shared<anbox::input::Manager>(
        rt, anbox::utils::string_format("%s/dev/input", rootfs_path.c_str()));
    
    touch_ = input_manager->create_device();
    touch_->set_name("anbox-touch");
    touch_->set_driver_version(1);
    touch_->set_input_id({BUS_VIRTUAL, 4, 4, 4});
    touch_->set_physical_location("none");
    touch_->set_abs_bit(ABS_MT_SLOT);
    touch_->set_abs_max(ABS_MT_SLOT, 10);
    touch_->set_abs_bit(ABS_MT_TOUCH_MAJOR);
    touch_->set_abs_max(ABS_MT_TOUCH_MAJOR, 127);
    touch_->set_abs_bit(ABS_MT_TOUCH_MINOR);
    touch_->set_abs_max(ABS_MT_TOUCH_MINOR, 127);
    touch_->set_abs_bit(ABS_MT_POSITION_X);
    touch_->set_abs_max(ABS_MT_POSITION_X, display_width);
    touch_->set_abs_bit(ABS_MT_POSITION_Y);
    touch_->set_abs_max(ABS_MT_POSITION_Y, display_height);
    touch_->set_abs_bit(ABS_MT_TRACKING_ID);
    touch_->set_abs_max(ABS_MT_TRACKING_ID, 10);
    touch_->set_prop_bit(INPUT_PROP_DIRECT);
    INFO("Touch input device created: anbox-touch (supports %d fingers)", 10);

    // Set up input callback from streaming server
    streaming_server->set_input_callback(
        [](const anbox::server::TouchEvent& event) {
            if (!touch_) return;
            std::vector<anbox::input::Event> touch_events;
            
            // Handle slot
            static int last_slot = -1;
            int slot = event.finger_id % 10;
            if (last_slot != slot) {
                touch_events.push_back({EV_ABS, ABS_MT_SLOT, slot});
                last_slot = slot;
            }
            
            switch (event.action) {
                case anbox::server::TOUCH_ACTION_DOWN:
                    DEBUG("Touch DOWN: finger=%d pos=(%d,%d)", event.finger_id, event.x, event.y);
                    touch_events.push_back({EV_ABS, ABS_MT_TRACKING_ID, static_cast<int32_t>(event.finger_id % 10 + 1)});
                    touch_events.push_back({EV_ABS, ABS_MT_POSITION_X, static_cast<int32_t>(event.x)});
                    touch_events.push_back({EV_ABS, ABS_MT_POSITION_Y, static_cast<int32_t>(event.y)});
                    break;
                case anbox::server::TOUCH_ACTION_MOVE:
                    touch_events.push_back({EV_ABS, ABS_MT_POSITION_X, static_cast<int32_t>(event.x)});
                    touch_events.push_back({EV_ABS, ABS_MT_POSITION_Y, static_cast<int32_t>(event.y)});
                    break;
                case anbox::server::TOUCH_ACTION_UP:
                    DEBUG("Touch UP: finger=%d", event.finger_id);
                    touch_events.push_back({EV_ABS, ABS_MT_TRACKING_ID, -1});
                    break;
            }
            
            touch_events.push_back({EV_SYN, SYN_REPORT, 0});
            touch_->send_events(touch_events);
        });
    INFO("Touch input callback registered");

    // Initialize qemu_pipe for Android container communication
    INFO("========================================");
    INFO("Setting up qemu_pipe for container communication...");
    INFO("========================================");
    auto sensors_state = std::make_shared<anbox::application::SensorsState>();
    auto gps_info_broker = std::make_shared<anbox::application::GpsInfoBroker>();
    
    std::string socket_file = qemu_pipe_path;
    INFO("qemu_pipe socket path: %s", socket_file.c_str());
    
    // Check if socket file exists
    struct stat socket_stat;
    if (stat(socket_file.c_str(), &socket_stat) == 0) {
        INFO("Existing file found at socket path");
        INFO("  File type: %s", S_ISSOCK(socket_stat.st_mode) ? "socket" : 
                                 S_ISREG(socket_stat.st_mode) ? "regular file" :
                                 S_ISDIR(socket_stat.st_mode) ? "directory" : "other");
        INFO("  Permissions: %04o", socket_stat.st_mode & 0777);
        INFO("  Size: %ld bytes", socket_stat.st_size);
        
        // Try to remove it
        INFO("Attempting to remove existing file...");
        if (unlink(socket_file.c_str()) != 0) {
            ERROR("Failed to unlink socket file: %s (errno=%d)", strerror(errno), errno);
            ERROR("This might cause issues when creating the socket");
        } else {
            INFO("Successfully removed old file");
        }
    } else {
        INFO("No existing file at socket path (errno=%d: %s)", errno, strerror(errno));
    }
    
    // Check parent directory permissions
    std::string parent_dir = base_path;
    struct stat dir_stat;
    if (stat(parent_dir.c_str(), &dir_stat) == 0) {
        INFO("Parent directory: %s", parent_dir.c_str());
        INFO("  Permissions: %04o", dir_stat.st_mode & 0777);
        INFO("  Writable: %s", (access(parent_dir.c_str(), W_OK) == 0) ? "yes" : "no");
    }
    
    try {
        INFO("Creating qemu_pipe connector...");
        INFO("This will attempt to create a Unix domain socket");
        qemu_pipe_connector_ = std::make_shared<anbox::network::PublishedSocketConnector>(
            socket_file, rt,
            std::make_shared<anbox::qemu::PipeConnectionCreator>(
                renderer_, rt, sensors_state, gps_info_broker));
        INFO("qemu_pipe socket created successfully at: %s", socket_file.c_str());
        INFO("========================================");
    } catch (const boost::exception& e) {
        ERROR("========================================");
        ERROR("Failed to create qemu_pipe socket (boost::exception)");
        ERROR("Exception details:");
        ERROR("%s", boost::diagnostic_information(e).c_str());
        ERROR("Socket path: %s", socket_file.c_str());
        ERROR("This usually means:");
        ERROR("  1. Permission denied - check directory permissions");
        ERROR("  2. Stale socket file couldn't be removed");
        ERROR("  3. Filesystem doesn't support Unix sockets");
        ERROR("========================================");
        // Continue without qemu_pipe - it's not critical for basic server operation
        WARNING("Continuing without qemu_pipe support");
        WARNING("Container graphics and sensors may not work properly");
        WARNING("========================================");
    } catch (const std::exception& e) {
        ERROR("========================================");
        ERROR("Failed to create qemu_pipe socket (std::exception)");
        ERROR("Error: %s", e.what());
        ERROR("Socket path: %s", socket_file.c_str());
        ERROR("========================================");
        // Continue without qemu_pipe - it's not critical for basic server operation
        WARNING("Continuing without qemu_pipe support");
        WARNING("========================================");
    }

    // Start the container if startup script is available
    // Start the container if rootfs exists
    if (stat(rootfs_path.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
        INFO("========================================");
        INFO("Starting Android container...");
        INFO("Rootfs: %s", rootfs_path.c_str());
        INFO("Proot: %s", proot_path.c_str());
        INFO("========================================");
        
        // Prepare container environment (same as run.sh)
        INFO("Preparing container environment...");
        
        // Clean up old files
        std::string system_log = rootfs_path + "/data/system.log";
        unlink(system_log.c_str());
        
        // Remove and recreate socket directories
        std::string dev_socket = rootfs_path + "/dev/socket";
        std::string cmd = "rm -rf " + dev_socket;
        system(cmd.c_str());
        mkdir(dev_socket.c_str(), 0755);
        
        std::string dev_properties = rootfs_path + "/dev/__properties__";
        cmd = "rm -rf " + dev_properties;
        system(cmd.c_str());
        mkdir(dev_properties.c_str(), 0755);
        
        // Remove and recreate device files
        std::string dev_kmsg = rootfs_path + "/dev/kmsg";
        std::string dev_pmsg0 = rootfs_path + "/dev/pmsg0";
        unlink(dev_kmsg.c_str());
        unlink(dev_pmsg0.c_str());
        
        // Create empty device files
        int fd = open(dev_kmsg.c_str(), O_CREAT | O_WRONLY, 0644);
        if (fd >= 0) close(fd);
        fd = open(dev_pmsg0.c_str(), O_CREAT | O_WRONLY, 0644);
        if (fd >= 0) close(fd);
        
        // Create necessary data directories
        std::string data_media = rootfs_path + "/data/media/0";
        std::string data_system_ce = rootfs_path + "/data/system_ce/0";
        std::string data_misc_ce = rootfs_path + "/data/misc_ce/0";
        
        cmd = "mkdir -p " + data_media;
        system(cmd.c_str());
        cmd = "mkdir -p " + data_system_ce;
        system(cmd.c_str());
        cmd = "mkdir -p " + data_misc_ce;
        system(cmd.c_str());
        
        INFO("Container environment prepared");
        
        container_pid = fork();
        if (container_pid < 0) {
            ERROR("fork() failed: %s", strerror(errno));
            rt->stop();
            return 1;
        } else if (container_pid == 0) {
            // Child process
            sigset_t signals_to_unblock;
            sigfillset(&signals_to_unblock);
            sigprocmask(SIG_UNBLOCK, &signals_to_unblock, 0);
            
            // Determine working directory for proot
            // Priority: -d/--directory option > rootfs_path (default)
            std::string proot_working_dir = working_directory.empty() ? rootfs_path : working_directory;
            
            // Change to the working directory before running proot
            if (chdir(proot_working_dir.c_str()) != 0) {
                fprintf(stderr, "Failed to change to working directory %s: %s\n", 
                        proot_working_dir.c_str(), strerror(errno));
                exit(1);
            }
            fprintf(stderr, "Changed to working directory: %s\n", proot_working_dir.c_str());
            
            // Redirect proot output if requested
            if (!proot_log_dest.empty()) {
                int proot_output_fd = -1;
                if (proot_log_is_special) {
                    // Handle special destinations (?stdout, ?stderr, ?null)
                    if (proot_log_dest == "stdout") {
                        proot_output_fd = STDOUT_FILENO;
                    } else if (proot_log_dest == "stderr") {
                        proot_output_fd = STDERR_FILENO;
                    } else if (proot_log_dest == "/dev/null") {
                        proot_output_fd = open("/dev/null", O_WRONLY);
                        if (proot_output_fd < 0) {
                            fprintf(stderr, "Failed to open /dev/null for proot log: %s\n", strerror(errno));
                            exit(1);
                        }
                    }
                } else {
                    // Treat as file path
                    proot_output_fd = open(proot_log_dest.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
                    if (proot_output_fd < 0) {
                        fprintf(stderr, "Failed to open proot log file %s: %s\n", 
                                proot_log_dest.c_str(), strerror(errno));
                        exit(1);
                    }
                }
                
                // Redirect both stdout and stderr to the specified destination
                if (proot_output_fd != STDOUT_FILENO) {
                    dup2(proot_output_fd, STDOUT_FILENO);
                }
                if (proot_output_fd != STDERR_FILENO) {
                    dup2(proot_output_fd, STDERR_FILENO);
                }
                
                // Close the original fd if it's not stdout/stderr
                if (proot_output_fd != STDOUT_FILENO && proot_output_fd != STDERR_FILENO) {
                    close(proot_output_fd);
                }
            }
            
            // Set Android environment variables
            setenv("PATH", "/sbin:/system/bin:/system/sbin:/system/xbin:/system/vendor/bin", 1);
            setenv("ANDROID_ASSETS", "/assets", 1);
            setenv("ANDROID_DATA", "/data", 1);
            setenv("ANDROID_ROOT", "/system", 1);
            setenv("ANDROID_STORAGE", "/storage", 1);
            setenv("ASEC_MOUNTPOINT", "/mnt/asec", 1);
            setenv("EXTERNAL_STORAGE", "/sdcard", 1);
            setenv("PROOT_TMP_DIR", proot_tmp_dir.c_str(), 1);
            
            // Set PROOT_LOADER if available (for platforms with noexec restrictions)
            std::string proot_dir = get_parent_dir(proot_path);
            std::string loader_path = proot_dir + "/libproot-loader.so";
            if (access(loader_path.c_str(), F_OK) == 0) {
                setenv("PROOT_LOADER", loader_path.c_str(), 1);
                fprintf(stderr, "Using PROOT_LOADER: %s\n", loader_path.c_str());
            }
            
            // Build proot command with bind mounts
            // Compute relative paths from proot_working_dir to rootfs items
            // This ensures bind mounts work regardless of where proot is launched from
            
            // Helper function to safely check if child_path is a subdirectory of parent_path
            // Returns true if child_path is under parent_path (e.g., /a/b is under /a)
            // Parameters: is_subdirectory(parent, child) -> checks if child is under parent
            auto is_subdirectory = [](const std::string& parent_path, const std::string& child_path) -> bool {
                // Normalize parent path (remove trailing slash if present)
                std::string normalized_parent = parent_path;
                while (normalized_parent.length() > 1 && normalized_parent.back() == '/') {
                    normalized_parent.pop_back();
                }
                
                if (child_path.length() <= normalized_parent.length()) {
                    return false;
                }
                // Check if child starts with parent
                if (child_path.compare(0, normalized_parent.length(), normalized_parent) != 0) {
                    return false;
                }
                // Ensure it's a proper subdirectory (next char must be '/')
                return child_path[normalized_parent.length()] == '/';
            };
            
            // Helper function to safely join paths, avoiding double slashes
            auto join_paths = [](const std::string& base, const std::string& append) -> std::string {
                if (base.empty()) return append;
                if (append.empty()) return base;
                
                bool base_has_slash = base.back() == '/';
                bool append_has_slash = append.front() == '/';
                
                if (base_has_slash && append_has_slash) {
                    return base + append.substr(1);
                } else if (!base_has_slash && !append_has_slash) {
                    return base + "/" + append;
                } else {
                    return base + append;
                }
            };
            
            // Helper function to compute relative path from 'from' to 'to'
            // Both paths must be absolute
            auto compute_relative_path = [](const std::string& from, const std::string& to) -> std::string {
                // If they're the same, return "."
                if (from == to) {
                    return ".";
                }
                
                // Split paths into components
                auto split_path = [](const std::string& path) -> std::vector<std::string> {
                    std::vector<std::string> components;
                    std::string current;
                    for (char c : path) {
                        if (c == '/') {
                            if (!current.empty()) {
                                components.push_back(current);
                                current.clear();
                            }
                        } else {
                            current += c;
                        }
                    }
                    if (!current.empty()) {
                        components.push_back(current);
                    }
                    return components;
                };
                
                auto from_parts = split_path(from);
                auto to_parts = split_path(to);
                
                // Find common prefix
                size_t common = 0;
                while (common < from_parts.size() && common < to_parts.size() &&
                       from_parts[common] == to_parts[common]) {
                    common++;
                }
                
                // Build relative path
                std::string result;
                // Add ".." for each remaining component in from_parts
                for (size_t i = common; i < from_parts.size(); i++) {
                    if (!result.empty()) result += "/";
                    result += "..";
                }
                // Add remaining components from to_parts
                for (size_t i = common; i < to_parts.size(); i++) {
                    if (!result.empty()) result += "/";
                    result += to_parts[i];
                }
                
                return result.empty() ? "." : result;
            };
            
            // Get relative path from working directory to rootfs
            std::string rootfs_rel;
            if (proot_working_dir == rootfs_path) {
                // Working dir IS rootfs, use "."
                rootfs_rel = ".";
            } else if (proot_working_dir[0] == '/' && rootfs_path[0] == '/') {
                // Both are absolute, compute relative path
                rootfs_rel = compute_relative_path(proot_working_dir, rootfs_path);
            } else if (is_subdirectory(proot_working_dir, rootfs_path)) {
                // Check: is rootfs_path under proot_working_dir?
                // If yes, rootfs is a subdirectory of working dir, use simple relative path
                rootfs_rel = rootfs_path.substr(proot_working_dir.length() + 1);
            } else {
                // Cannot compute relative path - this is an error
                fprintf(stderr, "ERROR: Cannot compute relative path from %s to %s\n",
                        proot_working_dir.c_str(), rootfs_path.c_str());
                fprintf(stderr, "Please ensure rootfs path is accessible from working directory\n");
                exit(1);
            }
            
            // Store bind mount strings to prevent temporary string destruction
            // Use paths relative to working directory
            std::string bind_dev_kmsg = join_paths(rootfs_rel, "dev/kmsg") + ":/dev/kmsg";
            std::string bind_dev_pmsg0 = join_paths(rootfs_rel, "dev/pmsg0") + ":/dev/pmsg0";
            std::string bind_vendor = join_paths(rootfs_rel, "system/vendor") + ":/vendor";
            std::string bind_dev_properties = join_paths(rootfs_rel, "dev/__properties__") + ":/dev/__properties__";
            std::string bind_dev_socket = join_paths(rootfs_rel, "dev/socket") + ":/dev/socket";
            // qemu_pipe is in the base directory (parent of rootfs), not inside rootfs
            std::string base_path = get_parent_dir(rootfs_path);
            std::string qemu_pipe_rel;
            if (proot_working_dir == base_path) {
                qemu_pipe_rel = "qemu_pipe";
            } else if (proot_working_dir[0] == '/' && base_path[0] == '/') {
                // Both are absolute, compute relative path to base_path, then append qemu_pipe
                std::string base_rel = compute_relative_path(proot_working_dir, base_path);
                qemu_pipe_rel = join_paths(base_rel, "qemu_pipe");
            } else if (is_subdirectory(proot_working_dir, base_path)) {
                // Check: is base_path under proot_working_dir?
                // If yes, base_path is a subdirectory of working dir, use simple relative path
                qemu_pipe_rel = join_paths(base_path.substr(proot_working_dir.length() + 1), "qemu_pipe");
            } else {
                // Cannot compute relative path - this is an error
                fprintf(stderr, "ERROR: Cannot compute relative path for qemu_pipe from %s to %s\n",
                        proot_working_dir.c_str(), base_path.c_str());
                fprintf(stderr, "Please ensure base path is accessible from working directory\n");
                exit(1);
            }
            std::string bind_qemu_pipe = qemu_pipe_rel + ":/dev/qemu_pipe";
            std::string bind_dev_input = join_paths(rootfs_rel, "dev/input") + ":/dev/input";
            std::string bind_mnt_user = join_paths(rootfs_rel, "mnt/user/0") + ":/storage/self";
            
            // Map verbose level to proot's -v option (0=quiet, 1=normal, 2=verbose, 3=extra)
            const char* proot_verbose_level;
            if (verbose == VerboseLevel::QUIET) proot_verbose_level = "0";
            else if (verbose == VerboseLevel::NORMAL) proot_verbose_level = "1";
            else if (verbose == VerboseLevel::VERBOSE) proot_verbose_level = "2";
            else proot_verbose_level = "3";  // EXTRA
            
            // Check if we should use run.sh script or direct proot invocation
            if (!runsh_path.empty()) {
                // Use custom run.sh script
                // Script will be invoked with: run.sh <rootfs_parent> <proot_path>
                std::string rootfs_parent = get_parent_dir(rootfs_path);
                
                fprintf(stderr, "Starting container with run.sh script...\n");
                fprintf(stderr, "Script: %s\n", runsh_path.c_str());
                fprintf(stderr, "Arguments: %s %s\n", rootfs_parent.c_str(), proot_path.c_str());
                
                std::vector<const char*> script_args;
                script_args.push_back(runsh_path.c_str());
                script_args.push_back(rootfs_parent.c_str());
                script_args.push_back(proot_path.c_str());
                script_args.push_back(nullptr);
                
                execvp(runsh_path.c_str(), const_cast<char* const*>(script_args.data()));
                
                fprintf(stderr, "Failed to start container with run.sh: %s\n", strerror(errno));
                exit(1);
            }
            
            // Direct proot invocation (default)
            std::vector<const char*> proot_args;
            proot_args.push_back(proot_path.c_str());
            
            // If custom parameters are provided (-a/--customparameter), use them instead of defaults
            if (!custom_parameters.empty()) {
                // Split custom_parameters by spaces and add to proot_args
                std::istringstream iss(custom_parameters);
                std::vector<std::string> custom_tokens;
                std::string token;
                while (iss >> token) {
                    custom_tokens.push_back(token);
                }
                
                // Store tokens in a persistent location and add pointers to proot_args
                static std::vector<std::string> custom_params_storage;
                custom_params_storage = custom_tokens;
                for (const auto& param : custom_params_storage) {
                    proot_args.push_back(param.c_str());
                }
                
                fprintf(stderr, "Using custom proot parameters: %s\n", custom_parameters.c_str());
            } else {
                // Default proot parameters
                proot_args.push_back("--kill-on-exit");
                proot_args.push_back("-r");
                proot_args.push_back(rootfs_rel.c_str());  // Rootfs relative to working directory
                proot_args.push_back("-0");  // Fake root
                proot_args.push_back("-w");
                proot_args.push_back("/");   // Working directory inside container
                
                // Bind mounts - EXACTLY as in the working command, in the same order
                proot_args.push_back("-b");
                proot_args.push_back("/dev");
                proot_args.push_back("-b");
                proot_args.push_back("/proc");
                // Bind specific device files (using relative paths from rootfs)
                proot_args.push_back("-b");
                proot_args.push_back(bind_dev_kmsg.c_str());
                proot_args.push_back("-b");
                proot_args.push_back(bind_dev_pmsg0.c_str());
                proot_args.push_back("-b");
                proot_args.push_back(bind_vendor.c_str());
                proot_args.push_back("-b");
                proot_args.push_back(bind_dev_properties.c_str());
                proot_args.push_back("-b");
                proot_args.push_back(bind_dev_socket.c_str());
                proot_args.push_back("-b");
                proot_args.push_back("/dev/binder");
                proot_args.push_back("-b");
                proot_args.push_back("/dev/ashmem");
                proot_args.push_back("-b");
                proot_args.push_back(bind_qemu_pipe.c_str());
                proot_args.push_back("-b");
                proot_args.push_back(bind_dev_input.c_str());
                proot_args.push_back("-b");
                proot_args.push_back(bind_mnt_user.c_str());
                
                // Verbose level comes AFTER bind mounts (as in the working command)
                proot_args.push_back("-v");
                proot_args.push_back(proot_verbose_level);
                
                // Add -q option if qemu_command is specified (for cross-platform execution)
                if (!qemu_command.empty()) {
                    proot_args.push_back("-q");
                    proot_args.push_back(qemu_command.c_str());
                }
                
                // Init binary to execute (default: /init, customizable via -i)
                proot_args.push_back(init_path.c_str());
            }
            
            // Add extra arguments from -e/--end option
            for (const auto& extra_arg : extra_proot_args) {
                proot_args.push_back(extra_arg.c_str());
            }
            
            proot_args.push_back(nullptr);
            
            fprintf(stderr, "Starting container with proot...\n");
            fprintf(stderr, "Working directory: %s\n", proot_working_dir.c_str());
            fprintf(stderr, "Init: %s\n", init_path.c_str());
            fprintf(stderr, "Verbose level: %s\n", proot_verbose_level);
            
            // Display full proot command line
            fprintf(stderr, "Command: ");
            for (size_t i = 0; proot_args[i] != nullptr; i++) {
                fprintf(stderr, "%s ", proot_args[i]);
            }
            fprintf(stderr, "\n");
            
            execvp(proot_path.c_str(), const_cast<char* const*>(proot_args.data()));
            
            fprintf(stderr, "Failed to start container: %s\n", strerror(errno));
            exit(1);
        } else {
            // Parent process
            INFO("Container process started with PID %d", container_pid);
            INFO("Container is initializing... This may take a few seconds.");
            
            // Start logcat monitoring if requested
            if (!container_logcat_dest.empty()) {
                // Give container a moment to initialize before starting logcat
                // A simple sleep is sufficient here - logcat will retry connections
                // if the container isn't fully ready yet
                sleep(2);
                
                INFO("Starting container logcat monitoring...");
                logcat_pid = fork();
                if (logcat_pid < 0) {
                    ERROR("Failed to fork logcat process: %s", strerror(errno));
                } else if (logcat_pid == 0) {
                    // Child process for logcat
                    sigset_t signals_to_unblock;
                    sigfillset(&signals_to_unblock);
                    sigprocmask(SIG_UNBLOCK, &signals_to_unblock, 0);
                    
                    // Determine output file descriptor based on container_logcat_dest
                    int output_fd = -1;
                    if (container_logcat_is_special) {
                        // Handle special destinations (?stdout, ?stderr, ?null)
                        if (container_logcat_dest == "stdout") {
                            output_fd = STDOUT_FILENO;
                        } else if (container_logcat_dest == "stderr") {
                            output_fd = STDERR_FILENO;
                        } else if (container_logcat_dest == "/dev/null") {
                            output_fd = open("/dev/null", O_WRONLY);
                            if (output_fd < 0) {
                                fprintf(stderr, "Failed to open /dev/null: %s\n", strerror(errno));
                                exit(1);
                            }
                        }
                    } else {
                        // Treat as file path
                        output_fd = open(container_logcat_dest.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
                        if (output_fd < 0) {
                            fprintf(stderr, "Failed to open logcat output file %s: %s\n", 
                                    container_logcat_dest.c_str(), strerror(errno));
                            exit(1);
                        }
                    }
                    
                    // Redirect stdout and stderr to the output destination
                    if (output_fd != STDOUT_FILENO) {
                        dup2(output_fd, STDOUT_FILENO);
                    }
                    if (output_fd != STDERR_FILENO) {
                        dup2(output_fd, STDERR_FILENO);
                    }
                    // Close the original fd if it's not stdout/stderr (i.e., it's a file or /dev/null)
                    // This prevents fd leaks - stdout/stderr are special and should not be closed
                    if (output_fd != STDOUT_FILENO && output_fd != STDERR_FILENO) {
                        close(output_fd);
                    }
                    
                    // Set PROOT_TMP_DIR environment variable (same as container)
                    setenv("PROOT_TMP_DIR", proot_tmp_dir.c_str(), 1);
                    
                    // Change to base directory
                    if (chdir(base_path.c_str()) != 0) {
                        fprintf(stderr, "Failed to change to base directory: %s\n", strerror(errno));
                        exit(1);
                    }
                    
                    // Execute proot to run logcat inside the container
                    // The command will be: proot -r rootfs /system/bin/logcat
                    const char* logcat_args[] = {
                        proot_path.c_str(),
                        "-r", rootfs_path.c_str(),
                        "/system/bin/logcat",
                        nullptr
                    };
                    
                    execvp(proot_path.c_str(), const_cast<char* const*>(logcat_args));
                    
                    fprintf(stderr, "Failed to start logcat: %s\n", strerror(errno));
                    exit(1);
                } else {
                    // Parent process
                    INFO("Logcat monitoring started with PID %d, output: %s", 
                         logcat_pid, container_logcat_dest.c_str());
                }
            }
        }
    } else {
        WARNING("========================================");
        WARNING("Rootfs not found: %s", rootfs_path.c_str());
        WARNING("Running in server-only mode (no container).");
        WARNING("The rootfs directory must exist to start the container.");
        WARNING("========================================");
    }

    INFO("========================================");
    INFO("Server is running!");
    INFO("Press Ctrl+C to stop.");
    INFO("========================================");
    
    // If EGL failed, we need to send test frames to show the streaming works
    // The container's OpenGL commands won't work without EGL
    bool send_test_frames = !egl_initialized;
    if (send_test_frames) {
        INFO("EGL not available - sending test pattern frames to verify streaming");
        INFO("Note: Container graphics require EGL/swiftshader to work in headless mode");
    }
    
    // Test frame buffer for when EGL is not available
    std::vector<uint8_t> test_frame;
    int test_frame_counter = 0;
    if (send_test_frames) {
        test_frame.resize(display_width * display_height * 4);
    }

    // Main loop - wait for shutdown or child exit
    while (running) {
        if (container_pid > 0) {
            // Check if child process is still running
            int status;
            pid_t result = waitpid(container_pid, &status, WNOHANG);
            if (result == container_pid) {
                if (WIFEXITED(status)) {
                    INFO("Container exited with code %d", WEXITSTATUS(status));
                } else if (WIFSIGNALED(status)) {
                    INFO("Container killed by signal %d", WTERMSIG(status));
                }
                container_pid = -1;
                // Don't exit, keep server running
            } else if (result < 0 && errno != ECHILD) {
                ERROR("waitpid failed: %s", strerror(errno));
            }
        }
        
        if (logcat_pid > 0) {
            // Check if logcat process is still running
            int status;
            pid_t result = waitpid(logcat_pid, &status, WNOHANG);
            if (result == logcat_pid) {
                if (WIFEXITED(status)) {
                    INFO("Logcat monitoring exited with code %d", WEXITSTATUS(status));
                } else if (WIFSIGNALED(status)) {
                    INFO("Logcat monitoring killed by signal %d", WTERMSIG(status));
                }
                logcat_pid = -1;
            } else if (result < 0 && errno != ECHILD) {
                ERROR("waitpid for logcat failed: %s", strerror(errno));
            }
        }
        
        // Send test frames when EGL is not available and clients are connected
        if (send_test_frames && streaming_server->has_clients()) {
            test_frame_counter++;
            
            // Generate a simple animated test pattern
            for (uint32_t y = 0; y < display_height; y++) {
                for (uint32_t x = 0; x < display_width; x++) {
                    size_t offset = (y * display_width + x) * 4;
                    // Create a gradient with animation
                    test_frame[offset + 0] = static_cast<uint8_t>((x + test_frame_counter) % 256); // R
                    test_frame[offset + 1] = static_cast<uint8_t>((y + test_frame_counter) % 256); // G
                    test_frame[offset + 2] = static_cast<uint8_t>(((x + y) / 2 + test_frame_counter * 2) % 256); // B
                    test_frame[offset + 3] = 255; // A
                }
            }
            
            // Send the test frame
            streaming_server->send_frame(test_frame.data(), display_width, display_height,
                                         anbox::server::PIXEL_FORMAT_RGBA8888, 
                                         display_width * 4);
            
            // Log periodically
            if (test_frame_counter % 100 == 1) {
                INFO("Sent test frame %d to clients", test_frame_counter);
            }
            
            // Limit frame rate to ~30 FPS
            std::this_thread::sleep_for(std::chrono::milliseconds(33));
        } else {
            // Let the runtime process I/O events
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    // Cleanup
    INFO("Shutting down...");
    
    // Unregister layer composer
    unRegisterLayerComposer();
    streaming_composer_.reset();
    
    // Stop ADB forwarder
    if (adb_forwarder) {
        adb_forwarder->stop();
    }
    
    // Stop streaming server
    if (streaming_server) {
        streaming_server->stop();
    }
    
    // Stop runtime
    if (rt) {
        rt->stop();
    }
    
    // Kill logcat monitoring if still running
    if (logcat_pid > 0) {
        INFO("Stopping logcat monitoring (PID %d)...", logcat_pid);
        kill(logcat_pid, SIGTERM);
        int status;
        waitpid(logcat_pid, &status, 0);
    }
    
    // Kill container if still running
    if (container_pid > 0) {
        INFO("Stopping container (PID %d)...", container_pid);
        kill(container_pid, SIGTERM);
        int status;
        waitpid(container_pid, &status, 0);
    }

    INFO("Server stopped.");
    return 0;
    
    } catch (const boost::exception& e) {
        std::cerr << "========================================" << std::endl;
        std::cerr << "FATAL: Uncaught boost::exception in main()" << std::endl;
        std::cerr << "========================================" << std::endl;
        std::cerr << "Exception details:" << std::endl;
        std::cerr << boost::diagnostic_information(e) << std::endl;
        std::cerr << "========================================" << std::endl;
        std::cerr << "This is a bug - all exceptions should be caught earlier." << std::endl;
        std::cerr << "Please report this error with the full output above." << std::endl;
        std::cerr << "========================================" << std::endl;
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "========================================" << std::endl;
        std::cerr << "FATAL: Uncaught exception in main()" << std::endl;
        std::cerr << "========================================" << std::endl;
        std::cerr << "Exception: " << e.what() << std::endl;
        std::cerr << "========================================" << std::endl;
        std::cerr << "This is a bug - all exceptions should be caught earlier." << std::endl;
        std::cerr << "Please report this error with the full output above." << std::endl;
        std::cerr << "========================================" << std::endl;
        return 1;
    } catch (...) {
        std::cerr << "========================================" << std::endl;
        std::cerr << "FATAL: Unknown exception in main()" << std::endl;
        std::cerr << "========================================" << std::endl;
        std::cerr << "This is a bug - all exceptions should be caught earlier." << std::endl;
        std::cerr << "Please report this error." << std::endl;
        std::cerr << "========================================" << std::endl;
        return 1;
    }
}
