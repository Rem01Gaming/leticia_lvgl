LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_MODULE := update-binary

LOCAL_SRC_FILES := $(shell find $(LOCAL_PATH)/src -name "*.cpp")
LOCAL_SRC_FILES := $(LOCAL_SRC_FILES:$(LOCAL_PATH)/%=%)

LOCAL_C_INCLUDES := $(LOCAL_PATH)/src

LOCAL_CPPFLAGS := -std=c++23 -O3 -fno-exceptions -fno-rtti
LOCAL_CPPFLAGS := -Wpedantic -Wall -Wextra -Werror -Wformat -Wuninitialized

LOCAL_LDLIBS := -lz -lm

LOCAL_LDFLAGS += -static

LOCAL_STATIC_LIBRARIES := lvgl ModernAlsa avformat avcodec swscale swresample avutil

include $(BUILD_EXECUTABLE)

include $(LOCAL_PATH)/external/Android.mk
