LOCAL_PATH := $(call my-dir)
ROOT_PATH := $(call my-dir)/../..

include $(CLEAR_VARS)
LOCAL_MODULE := lvgl

rwildcard = $(wildcard $1$2) $(foreach d,$(wildcard $1*),$(call rwildcard,$d/,$2))

# ThorVG (src/libs/thorvg) is C++, so both extensions are needed here;
# the rest of LVGL proper stays plain C.
LOCAL_SRC_FILES := $(call rwildcard,$(LOCAL_PATH)/src/,*.c) $(call rwildcard,$(LOCAL_PATH)/src/,*.cpp)
LOCAL_SRC_FILES := $(LOCAL_SRC_FILES:$(LOCAL_PATH)/%=%)

LOCAL_STATIC_LIBRARIES := avformat avcodec swscale freetype

LOCAL_C_INCLUDES := $(ROOT_PATH)/include

LOCAL_EXPORT_C_INCLUDES := $(LOCAL_PATH)

LOCAL_CFLAGS += -O3 -Wall -Wextra -std=gnu11

# ThorVG relies on C++ exceptions and RTTI internally. This module's flags
# are independent of the app executable's (-fno-exceptions -fno-rtti in
# jni/Android.mk), so enabling them here does not affect the rest of the app.
LOCAL_CPPFLAGS += -O3 -std=c++17 -fexceptions -frtti

include $(BUILD_STATIC_LIBRARY)
