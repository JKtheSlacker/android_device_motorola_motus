#
# Copyright (C) 2008 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
$(call inherit-product, $(SRC_TARGET_DIR)/product/languages_full.mk)

# The gps config appropriate for this device
$(call inherit-product, device/common/gps/gps_us_supl.mk)

PACKAGES.lights.motus.OVERRIDES := lights.msm7k libaudio gralloc.msm7k

DEVICE_PACKAGE_OVERLAYS := device/motorola/motus/overlay

PRODUCT_PACKAGES += \
    VoiceDialer \
    lights.motus

PRODUCT_LOCALES += mdpi

PRODUCT_COPY_FILES += \
    frameworks/base/data/etc/handheld_core_hardware.xml:system/etc/permissions/handheld_core_hardware.xml \
    frameworks/base/data/etc/android.hardware.camera.autofocus.xml:system/etc/permissions/android.hardware.camera.autofocus.xml \
    frameworks/base/data/etc/android.hardware.camera.flash-autofocus.xml:system/etc/permissions/android.hardware.camera.flash-autofocus.xml \
    frameworks/base/data/etc/android.hardware.sensor.compass.xml:system/etc/permissions/android.hardware.sensor.compass.xml \
    frameworks/base/data/etc/android.hardware.sensor.accelerometer.xml:system/etc/permissions/android.hardware.sensor.accelerometer.xml \
    frameworks/base/data/etc/android.hardware.telephony.gsm.xml:system/etc/permissions/android.hardware.telephony.gsm.xml \
    frameworks/base/data/etc/android.hardware.location.gps.xml:system/etc/permissions/android.hardware.location.gps.xml \
    frameworks/base/data/etc/android.hardware.wifi.xml:system/etc/permissions/android.hardware.wifi.xml \
    frameworks/base/data/etc/android.hardware.sensor.proximity.xml:system/etc/permissions/android.hardware.sensor.proximity.xml \
    frameworks/base/data/etc/android.hardware.sensor.light.xml:system/etc/permissions/android.hardware.sensor.light.xml \
    frameworks/base/data/etc/android.hardware.touchscreen.multitouch.xml:system/etc/permissions/android.hardware.touchscreen.multitouch.xml

PRODUCT_PROPERTY_OVERRIDES += \
    keyguard.no_require_sim=true \
    ro.com.android.dateformat=MM-dd-yyyy \
    ro.media.dec.jpeg.memcap=10000000 \
    rild.libpath=/system/lib/libril-qc-1.so \
    wifi.interface=eth0

# Time between scans in seconds. Keep it high to minimize battery drain.
# This only affects the case in which there are remembered access points,
# but none are in range.
PRODUCT_PROPERTY_OVERRIDES += \
    wifi.supplicant_scan_interval=15

# density in DPI of the LCD of this board. This is used to scale the UI
# appropriately. If this property is not defined, the default value is 160 dpi. 
PRODUCT_PROPERTY_OVERRIDES += \
    ro.sf.lcd_density=160

# Default network type
# 0 => WCDMA Preferred.
PRODUCT_PROPERTY_OVERRIDES += \
    ro.telephony.default_network=0

# The OpenGL ES API level that is natively supported by this device.
# This is a 16.16 fixed point number
PRODUCT_PROPERTY_OVERRIDES += \
    ro.opengles.version=65536

# The kernel
ifeq ($(TARGET_PREBUILT_KERNEL),)
LOCAL_KERNEL := device/motorola/motus/kernel
else
LOCAL_KERNEL := $(TARGET_PREBUILT_KERNEL)
endif

PRODUCT_COPY_FILES += \
    $(LOCAL_KERNEL):kernel

PRODUCT_COPY_FILES += \
    device/motorola/motus/vold.fstab:/system/etc/vold.fstab \
    device/motorola/motus/prebuilt/bin/init.bt.sh:/system/bin/init.bt.sh \
    device/motorola/motus/prebuilt/etc/media_profiles.xml:/system/etc/media_profiles.xml \
    device/motorola/motus/prebuilt/etc/wifi/wpa_supplicant.conf:system/etc/wifi/wpa_supplicant.conf \
    device/motorola/motus/prebuilt/etc/wifi/backoff.conf:system/etc/wifi/backoff.conf \
    device/motorola/motus/prebuilt/etc/dhcpcd/dhcpcd.conf:system/etc/dhcpcd/dhcpcd.conf \
    device/motorola/motus/prebuilt/lib/modules/dhd.ko:system/lib/modules/dhd.ko \
    device/motorola/motus/prebuilt/lib/modules/ramzswap.ko:system/lib/modules/ramzswap.ko \
    device/motorola/motus/prebuilt/usr/keylayout/motus-kpd.kl:/system/usr/keylayout/motus-kpd.kl \
    device/motorola/motus/prebuilt/usr/keylayout/adp5588_motus.kl:/system/usr/keylayout/adp5588_motus.kl \
    device/motorola/motus/prebuilt/usr/keylayout/adp5588_motus_P1.kl:/system/usr/keylayout/adp5588_motus_P1.kl \
    device/motorola/motus/prebuilt/usr/keylayout/adp5588_motus_P2.kl:/system/usr/keylayout/adp5588_motus_P2.kl \
    device/motorola/motus/prebuilt/usr/keylayout/adp5588_motus_P3.kl:/system/usr/keylayout/adp5588_motus_P3.kl \
    device/motorola/motus/prebuilt/usr/keychars/motus-kpd.kcm:system/usr/keychars/motus-kpd.kcm \
    device/motorola/motus/prebuilt/usr/keychars/adp5588_motus_P1.kcm.bin:system/usr/keychars/adp5588_motus_P1.kcm.bin \
    device/motorola/motus/prebuilt/usr/keychars/adp5588_motus_P2.kcm.bin:system/usr/keychars/adp5588_motus_P2.kcm.bin \
    device/motorola/motus/prebuilt/usr/keychars/adp5588_motus_P3.kcm.bin:system/usr/keychars/adp5588_motus_P3.kcm.bin


## (2) Also get non-open-source aspects if available
$(call inherit-product-if-exists, vendor/motorola/motus/motus-vendor.mk)

$(call inherit-product, $(SRC_TARGET_DIR)/product/generic.mk)

# Discard inherited values and use our own instead.
PRODUCT_NAME := motus
PRODUCT_DEVICE := motus
PRODUCT_MODEL := Full Android on Motus
