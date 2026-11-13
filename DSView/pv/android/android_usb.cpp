/*
 * Android USB OTG 接入桥。
 *
 * 背景：libusb 在 Android 上没有 /dev/bus/usb 的访问权限，也不能自己枚举
 * 设备（selinux + 权限模型）。标准做法是：
 *
 *   1) Java 侧用 UsbManager 找到设备 → requestPermission → openDevice()
 *   2) 把 UsbDeviceConnection 的 fd 交给 native
 *   3) native 用 libusb_wrap_sys_device(ctx, fd, &handle) 把它包成 libusb 设备
 *
 * 本文件负责 2)、3)，并对上层（libsigrok4DSL 的 scan 路径）暴露一个
 * "只有一个设备" 的假 devlist，让原有的枚举/打开逻辑原封不动地复用。
 */
#include "../../../libsigrok4DSL/android_usb_bridge.h"

#ifdef __ANDROID__

#include <QCoreApplication>
#include <QJniObject>
#include <QJniEnvironment>
#include <QtCore/qcoreapplication_platform.h>

#include <libusb.h>

#include <stdio.h>
#include <stdlib.h>

static libusb_context *g_ctx = nullptr;
static libusb_device_handle *g_handle = nullptr;
static libusb_device *g_device = nullptr;
static int g_fd = -1;

static const char *kHelperClass = "org/dreamsourcelab/dsview/UsbHelper";

static jobject androidActivity()
{
    // Qt 6.5 的 QNativeInterface::QAndroidApplication::context() 直接返回 jobject
    return QNativeInterface::QAndroidApplication::context();
}

extern "C" {

int srdv_android_usb_open(unsigned vid, unsigned pid)
{
    if (g_device != nullptr)
        return 0;

    jobject activity = androidActivity();
    if (activity == nullptr) {
        fprintf(stderr, "android_usb: no Qt activity, cannot talk to UsbManager\n");
        return -1;
    }

    jint fd = QJniObject::callStaticMethod<jint>(
            kHelperClass, "openDevice",
            "(Landroid/app/Activity;II)I",
            activity, (jint)vid, (jint)pid);

    if (fd < 0) {
        fprintf(stderr, "android_usb: openDevice failed (vid=%04x pid=%04x)\n", vid, pid);
        return -1;
    }

    if (g_ctx == nullptr) {
        if (libusb_init(&g_ctx) != 0) {
            fprintf(stderr, "android_usb: libusb_init failed\n");
            return -1;
        }
    }

    int ret = libusb_wrap_sys_device(g_ctx, (intptr_t)fd, &g_handle);
    if (ret != 0 || g_handle == nullptr) {
        fprintf(stderr, "android_usb: libusb_wrap_sys_device failed: %s\n",
                libusb_error_name(ret));
        g_handle = nullptr;
        return -1;
    }

    g_device = libusb_get_device(g_handle);
    g_fd = (int)fd;
    fprintf(stderr, "android_usb: wrapped fd=%d as libusb device %p\n", g_fd, (void *)g_device);
    return 0;
}

int srdv_android_usb_opened(void)
{
    return g_device != nullptr ? 1 : 0;
}

libusb_device *srdv_android_usb_device(void)
{
    return g_device;
}

/**
 * 给 libsigrok 的 scan 路径用的"设备列表"。
 * 调用方按 libusb_get_device_list 的语义使用：遍历到 NULL 结束。
 */
libusb_device **srdv_android_usb_scan_list(void)
{
    if (g_device == nullptr)
        return nullptr;

    libusb_device **list = (libusb_device **)calloc(2, sizeof(libusb_device *));
    if (list == nullptr)
        return nullptr;
    list[0] = g_device;
    list[1] = nullptr;
    return list;
}

void srdv_android_usb_free_scan_list(libusb_device **list)
{
    free(list);
}

void srdv_android_usb_close(void)
{
    if (g_handle != nullptr) {
        libusb_close(g_handle);
        g_handle = nullptr;
    }
    g_device = nullptr;
    g_fd = -1;
    QJniObject::callStaticMethod<void>(kHelperClass, "closeDevice", "()V");
}

} // extern "C"

#endif // __ANDROID__
