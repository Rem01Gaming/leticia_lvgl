LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_MODULE := update-binary

LOCAL_SRC_FILES := $(wildcard $(LOCAL_PATH)/src/*.cpp)
LOCAL_SRC_FILES := $(LOCAL_SRC_FILES:$(LOCAL_PATH)/%=%) $(LVGL_SRC_FILES)

LOCAL_C_INCLUDES := $(LOCAL_PATH)/src

LOCAL_CPPFLAGS := -std=c++23 -O2
LOCAL_CPPFLAGS := -Wpedantic -Wall -Wextra -Werror -Wformat -Wuninitialized

LOCAL_LDLIBS := -lm

LOCAL_LDFLAGS += -static

LOCAL_STATIC_LIBRARIES := lvgl

include $(BUILD_EXECUTABLE)

include $(LOCAL_PATH)/external/Android.mk
