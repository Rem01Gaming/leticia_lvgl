LOCAL_PATH := $(call my-dir)
ROOT_PATH := $(call my-dir)/../..

include $(CLEAR_VARS)
LOCAL_MODULE := lvgl

rwildcard = $(wildcard $1$2) $(foreach d,$(wildcard $1*),$(call rwildcard,$d/,$2))

LOCAL_SRC_FILES := $(call rwildcard,$(LOCAL_PATH)/src/,*.c)
LOCAL_SRC_FILES := $(LOCAL_SRC_FILES:$(LOCAL_PATH)/%=%)

LOCAL_STATIC_LIBRARIES := avformat avcodec swscale

LOCAL_C_INCLUDES := $(ROOT_PATH)/include

LOCAL_EXPORT_C_INCLUDES := $(LOCAL_PATH)

LOCAL_CFLAGS += -O3 -Wall -Wextra -std=gnu11

include $(BUILD_STATIC_LIBRARY)
