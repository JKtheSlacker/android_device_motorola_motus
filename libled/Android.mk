ifeq ($(TARGET_BOOTLOADER_BOARD_NAME),motus)

LOCAL_PATH:= $(call my-dir)

include $(CLEAR_VARS)

LOCAL_MODULE := leds.motus

LOCAL_MODULE_PATH := $(TARGET_OUT_SHARED_LIBRARIES)/hw

LOCAL_MODULE_TAGS := eng

LOCAL_SRC_FILES := led_motus.c
LOCAL_SRC_FILES += led_stub.c
LOCAL_CFLAGS    += -DCONFIG_LED_MOTUS
LOCAL_SHARED_LIBRARIES := liblog
LOCAL_PRELINK_MODULE := false

include $(BUILD_SHARED_LIBRARY)

endif
