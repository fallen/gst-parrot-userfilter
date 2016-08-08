LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)

LOCAL_MODULE := gst-parrot-user-filter
LOCAL_CATEGORY_PATH := multimedia/gstreamer
LOCAL_DESCRIPTION := GStreamer Parrot user filter plugin
LOCAL_MODULE_FILENAME := libgstparrotuserfilter.so
LOCAL_DESTDIR := usr/lib/gstreamer-1.0

LOCAL_LIBRARIES := glib gstreamer libpomp gst-plugins-base

LOCAL_CFLAGS := -DGST_USE_UNSTABLE_API

LOCAL_C_INCLUDES := $(LOCAL_PATH)

LOCAL_SRC_FILES := userfilter.c

include $(BUILD_SHARED_LIBRARY)
