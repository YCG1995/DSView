/*
 * Android USB OTG 接入桥的 C 接口声明。
 *
 * 由 DSView/pv/android/android_usb.cpp 实现（只在 __ANDROID__ 下编译）。
 * libsigrok4DSL 的 scan/open 逻辑是纯 C，通过这里拿"只有一个设备"的
 * libusb 设备列表，避免去改 libusb 本体。
 */
#ifndef DSVIEW_ANDROID_USB_H
#define DSVIEW_ANDROID_USB_H

#ifdef __ANDROID__

#ifdef __cplusplus
extern "C" {
#endif

struct libusb_device;

/* 打开设备（内部会调 Java 的 UsbManager.openDevice 并 libusb_wrap_sys_device）。
 * 返回 0 成功，<0 失败。 */
int srdv_android_usb_open(unsigned vid, unsigned pid);

/* 已经成功打开过设备？ */
int srdv_android_usb_opened(void);

/* 被 wrap 出来的 libusb_device，没有则返回 NULL。 */
struct libusb_device *srdv_android_usb_device(void);

/* 返回 {dev, NULL} 的两元素数组，供 scan 循环使用；用完调 free。 */
struct libusb_device **srdv_android_usb_scan_list(void);
void srdv_android_usb_free_scan_list(struct libusb_device **list);

/* 关闭设备并释放 Java 侧的连接。 */
void srdv_android_usb_close(void);

#ifdef __cplusplus
}
#endif

#endif /* __ANDROID__ */

#endif /* DSVIEW_ANDROID_USB_H */
