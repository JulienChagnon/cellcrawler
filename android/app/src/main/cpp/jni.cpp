// JNI entry points for com.fpvcar.rover.NativeBridge.
#include <jni.h>

#include <algorithm>
#include <memory>
#include <mutex>
#include <string>

#include "common.h"
#include "fpv/passphrase.h"
#include "engine.h"
#include "usb_serial.h"
#include "video.h"

using namespace rover;

namespace {
std::mutex gMu;
UsbSerial gUsb;  // lives independently of the engine: USB can come and go while running
std::unique_ptr<Engine> gEngine;

std::string str(JNIEnv* env, jstring s) {
    if (!s) return {};
    const char* c = env->GetStringUTFChars(s, nullptr);
    std::string r(c);
    env->ReleaseStringUTFChars(s, c);
    return r;
}
}  // namespace

extern "C" {

JNIEXPORT jstring JNICALL Java_com_fpvcar_rover_NativeBridge_start(JNIEnv* env, jclass, jstring host, jint port, jstring key,
                                                                    jint width, jint height, jint fps, jint kbps,
                                                                    jstring cameraId, jlong network) {
    std::lock_guard<std::mutex> lk(gMu);
    gEngine.reset();
    EngineConfig cfg;
    cfg.host = str(env, host);
    cfg.port = uint16_t(port);
    std::string k = str(env, key);
    if (const char* err = fpv::secretToKey(k.c_str(), cfg.key)) return env->NewStringUTF(err);
    if (cfg.host.empty()) return env->NewStringUTF("PC address is empty");
    cfg.video.width = width;
    cfg.video.height = height;
    cfg.video.fps = fps;
    cfg.video.maxKbps = kbps;
    cfg.video.minKbps = std::min(500, int(kbps));
    cfg.video.cameraId = str(env, cameraId);
    gEngine = std::make_unique<Engine>(cfg, gUsb);
    gEngine->setNetwork(net_handle_t(network));
    gEngine->start();
    return nullptr;
}

JNIEXPORT void JNICALL Java_com_fpvcar_rover_NativeBridge_stop(JNIEnv*, jclass) {
    std::lock_guard<std::mutex> lk(gMu);
    gEngine.reset();
}

JNIEXPORT jboolean JNICALL Java_com_fpvcar_rover_NativeBridge_usbOpen(JNIEnv*, jclass, jint fd, jint vid, jint pid,
                                                                      jint ctrlIface, jint epIn, jint epOut) {
    return gUsb.open(fd, vid, pid, ctrlIface, epIn, epOut) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL Java_com_fpvcar_rover_NativeBridge_usbClose(JNIEnv*, jclass) { gUsb.close(); }

JNIEXPORT jboolean JNICALL Java_com_fpvcar_rover_NativeBridge_usbIsOpen(JNIEnv*, jclass) {
    return gUsb.isOpen() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL Java_com_fpvcar_rover_NativeBridge_setNetwork(JNIEnv*, jclass, jlong handle) {
    std::lock_guard<std::mutex> lk(gMu);
    if (gEngine) gEngine->setNetwork(net_handle_t(handle));
}

JNIEXPORT void JNICALL Java_com_fpvcar_rover_NativeBridge_setPhoneStats(JNIEnv*, jclass, jint battPct, jint tempDeciC,
                                                                         jboolean charging, jint rsrpDbm, jint netType) {
    std::lock_guard<std::mutex> lk(gMu);
    if (!gEngine) return;
    PhoneStats s;
    s.battPct = uint8_t(battPct);
    s.tempDeciC = int16_t(tempDeciC);
    s.charging = charging;
    s.rsrpDbm = int16_t(rsrpDbm);
    s.netType = uint8_t(netType);
    gEngine->setPhoneStats(s);
}

JNIEXPORT jstring JNICALL Java_com_fpvcar_rover_NativeBridge_status(JNIEnv* env, jclass) {
    std::lock_guard<std::mutex> lk(gMu);
    std::string s = gEngine ? gEngine->status() : std::string("Stopped\nUSB:    ") + (gUsb.isOpen() ? gUsb.chip() : "not connected") + "\n";
    return env->NewStringUTF(s.c_str());
}

JNIEXPORT jstring JNICALL Java_com_fpvcar_rover_NativeBridge_cameraList(JNIEnv* env, jclass) {
    return env->NewStringUTF(VideoPipeline::listCameras().c_str());
}

}  // extern "C"
