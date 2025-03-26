LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_SHARED_LIBRARIES := \
    audio.primary.$(TARGET_BOARD_PLATFORM) \
    libaudioroute \
    libaudioutils \
    libcutils \
    liblog \
    libtinyalsa \
    libtinycompress

LOCAL_C_INCLUDES += \
    $(call include-path-for,audio-route) \
    $(call project-path-for,qcom-audio)/hal \
    $(call project-path-for,qcom-audio)/hal/$(AUDIO_PLATFORM) \
    $(call project-path-for,qcom-audio)/hal/audio_extn

# For Cirrus Logic CS35L41 speaker amp
LOCAL_CFLAGS += -DDEBUG_SHOW_VALUES

LOCAL_SRC_FILES := audio_amplifier.c
LOCAL_MODULE := audio_amplifier.$(TARGET_BOARD_PLATFORM)
LOCAL_HEADER_LIBRARIES := \
    generated_kernel_headers \
    libhardware_headers

LOCAL_MODULE_RELATIVE_PATH := hw
LOCAL_VENDOR_MODULE := true

include $(BUILD_SHARED_LIBRARY)