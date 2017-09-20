LOCAL_PATH := $(dir $(call my-dir))

.PHONY: all-gpsd-modules

gpsd_with_ntpshm := $(strip $(shell grep '^\#define NTPSHM_ENABLE' $(LOCAL_PATH)/gpsd_config.h 2>/dev/null))
gpsd_with_ncurses := $(strip $(shell grep '^\#define NCURSES_ENABLE' $(LOCAL_PATH)/gpsd_config.h 2>/dev/null))

########################################
# libgps

include $(CLEAR_VARS)

LOCAL_MODULE := libgps
all-gpsd-modules: $(LOCAL_MODULE)

LOCAL_MODULE_CLASS := SHARED_LIBRARIES
LOCAL_SRC_FILES := \
    ais_json.c \
    bits.c \
    gpsutils.c \
    gpsdclient.c \
    hex.c \
    json.c \
    libgps_core.c \
    libgps_dbus.c \
    libgps_json.c \
    libgps_shm.c \
    libgps_sock.c \
    netlib.c \
    os_compat.c \
    rtcm2_json.c \
    rtcm3_json.c \
    shared_json.c \
    $(empty)

libgps_gen_intermediates := $(call local-generated-sources-dir)

# ais_json.c includes generated ais_json.i
$(LOCAL_PATH)/ais_json.c: $(libgps_gen_intermediates)/ais_json.i
$(libgps_gen_intermediates)/ais_json.i: PRIVATE_PATH := $(LOCAL_PATH)
$(libgps_gen_intermediates)/ais_json.i: PRIVATE_CUSTOM_TOOL = $< --ais --target=parser >$@
$(libgps_gen_intermediates)/ais_json.i: $(LOCAL_PATH)/jsongen.py
	$(transform-generated-source)

LOCAL_C_INCLUDES := \
    $(libgps_gen_intermediates) \
    $(empty)

LOCAL_GENERATED_SOURCES := \
    $(libgps_gen_intermediates)/gps_maskdump.c \
    $(libgps_gen_intermediates)/packet_names.h \
    $(libgps_gen_intermediates)/revision.h \
    $(empty)

$(libgps_gen_intermediates)/gps_maskdump.c: PRIVATE_PATH := $(LOCAL_PATH)
$(libgps_gen_intermediates)/gps_maskdump.c: PRIVATE_CUSTOM_TOOL = $(PRIVATE_PATH)/maskaudit.py -c $(PRIVATE_PATH) >$@
$(libgps_gen_intermediates)/gps_maskdump.c: $(addprefix $(LOCAL_PATH)/,maskaudit.py gps.h gpsd.h)
	$(transform-generated-source)

$(libgps_gen_intermediates)/packet_names.h: PRIVATE_CUSTOM_TOOL = sed -e '/^ *\([A-Z][A-Z0-9_]*\),/s//   \"\\1\",/' $< >$@
$(libgps_gen_intermediates)/packet_names.h: $(LOCAL_PATH)/packet_states.h
	$(transform-generated-source)

$(libgps_gen_intermediates)/revision.h: PRIVATE_PATH := $(LOCAL_PATH)
$(libgps_gen_intermediates)/revision.h: PRIVATE_CUSTOM_TOOL = echo "\#define REVISION $$(cat $(PRIVATE_PATH)/SConstruct | grep "^gpsd_version =" | awk '{print $$3}')" >$@
$(libgps_gen_intermediates)/revision.h:
	$(transform-generated-source)

LOCAL_COPY_HEADERS := gps.h

include $(BUILD_SHARED_LIBRARY)

########################################
# libgpsmm

include $(CLEAR_VARS)

LOCAL_MODULE := libgpsmm
all-gpsd-modules: $(LOCAL_MODULE)

LOCAL_SRC_FILES := \
    libgpsmm.cpp \
    $(empty)
LOCAL_SHARED_LIBRARIES := \
    libgps \
    $(empty)

LOCAL_COPY_HEADERS := libgpsmm.h

include $(BUILD_SHARED_LIBRARY)

########################################
# libgpsd

include $(CLEAR_VARS)

LOCAL_MODULE := libgpsd
all-gpsd-modules: $(LOCAL_MODULE)

LOCAL_SRC_FILES := \
    bsd_base64.c \
    crc24q.c \
    gpsd_json.c \
    geoid.c \
    isgps.c \
    libgpsd_core.c \
    matrix.c \
    net_dgpsip.c \
    net_gnss_dispatch.c \
    net_ntrip.c \
    ppsthread.c \
    packet.c \
    pseudonmea.c \
    pseudoais.c \
    serial.c \
    subframe.c \
    timebase.c \
    timespec_str.c \
    drivers.c \
    driver_ais.c \
    driver_evermore.c \
    driver_garmin.c \
    driver_garmin_txt.c \
    driver_geostar.c \
    driver_italk.c \
    driver_navcom.c \
    driver_nmea0183.c \
    driver_nmea2000.c \
    driver_oncore.c \
    driver_qemudpipe.c \
    driver_rtcm2.c \
    driver_rtcm3.c \
    driver_sirf.c \
    driver_skytraq.c \
    driver_superstar2.c \
    driver_tsip.c \
    driver_ubx.c \
    driver_zodiac.c \
    $(empty)

ifneq ($(gpsd_with_ntpshm),)
LOCAL_SRC_FILES += \
    ntpshmread.c \
    ntpshmwrite.c
endif

LOCAL_C_INCLUDES := \
    $(libgps_gen_intermediates) \
    $(empty)
LOCAL_SHARED_LIBRARIES := \
    libcutils \
    libgps \
    $(empty)

include $(BUILD_SHARED_LIBRARY)

########################################
# gpsmon utility

include $(CLEAR_VARS)

LOCAL_MODULE := gpsmon
ifneq ($(gpsd_with_ncurses),)
all-gpsd-modules: $(LOCAL_MODULE)
endif

LOCAL_SRC_FILES := \
    gpsmon.c \
    monitor_italk.c \
    monitor_nmea0183.c \
    monitor_oncore.c \
    monitor_sirf.c \
    monitor_superstar2.c \
    monitor_tnt.c \
    monitor_ubx.c \
    monitor_garmin.c \
    $(empty)
LOCAL_SHARED_LIBRARIES := \
    libgpsd \
    $(empty)

include $(BUILD_EXECUTABLE)

########################################
# gpsd utility
#
# On Android-N or above, LOCAL_INIT_RC should install gpsd service
# file to /system/etc/init, and you should define ${ro.gpsd.sources},
# too:
#
#   ADDITIONAL_BUILD_PROPERTIES += ro.gpsd.sources=/dev/ttyS0
#
# ${ro.gpsd.sources} should follow gpsd's data source format, e.g.
# "/dev/ttyS0" or "qemud://gps" on emulators. For older platforms, a
# similar section must be added to init.rc. On Android-M:
#
#   on post-fs-data
#     mkdir /data/gps 0770 gps system
#
#   service gpsd /system/bin/gpsd -F /dev/socket/gpsd -N /dev/ttyS0
#     class late_start
#     user gps
#     group gps inet
#
# Note that serial device has to be labelled as "gps_device" under
# selinux. Include selinux policy rules in BoardConfig.mk as well:
#
#   BOARD_SEPOLICY_DIRS += $(LOCAL_PATH)/android/sepolicy

include $(CLEAR_VARS)

LOCAL_MODULE := gpsd
all-gpsd-modules: $(LOCAL_MODULE)

LOCAL_SRC_FILES := \
    dbusexport.c \
    gpsd.c \
    shmexport.c \
    timehint.c \
    $(empty)
LOCAL_SHARED_LIBRARIES := \
    libgps \
    libgpsd \
    $(empty)
LOCAL_C_INCLUDES := \
    $(libgps_gen_intermediates) \
    $(empty)
LOCAL_INIT_RC := android/gpsd.rc

include $(BUILD_EXECUTABLE)

########################################
# gpsdecode utility

include $(CLEAR_VARS)

LOCAL_MODULE := gpsdecode
all-gpsd-modules: $(LOCAL_MODULE)

LOCAL_SRC_FILES := \
    gpsdecode.c \
    $(empty)
LOCAL_SHARED_LIBRARIES := \
    libgps \
    libgpsd \
    $(empty)

include $(BUILD_EXECUTABLE)

########################################
# gpsctl utility

include $(CLEAR_VARS)

LOCAL_MODULE := gpsctl
all-gpsd-modules: $(LOCAL_MODULE)

LOCAL_SRC_FILES := \
    gpsctl.c \
    $(empty)
LOCAL_C_INCLUDES := \
    $(libgps_gen_intermediates) \
    $(empty)
LOCAL_SHARED_LIBRARIES := \
    libgps \
    libgpsd \
    $(empty)

include $(BUILD_EXECUTABLE)

########################################
# gpsdctl utility

include $(CLEAR_VARS)

LOCAL_MODULE := gpsdctl
all-gpsd-modules: $(LOCAL_MODULE)

LOCAL_SRC_FILES := \
    gpsdctl.c \
    $(empty)
LOCAL_SHARED_LIBRARIES := \
    libgps \
    $(empty)

include $(BUILD_EXECUTABLE)

########################################
# gpspipe utility

include $(CLEAR_VARS)

LOCAL_MODULE := gpspipe
all-gpsd-modules: $(LOCAL_MODULE)

LOCAL_SRC_FILES := \
    gpspipe.c \
    $(empty)
LOCAL_C_INCLUDES := \
    $(libgps_gen_intermediates) \
    $(empty)
LOCAL_SHARED_LIBRARIES := \
    libgps \
    $(empty)

include $(BUILD_EXECUTABLE)

########################################
# gps2udp utility

include $(CLEAR_VARS)

LOCAL_MODULE := gps2udp
all-gpsd-modules: $(LOCAL_MODULE)

LOCAL_SRC_FILES := \
    gps2udp.c \
    $(empty)
LOCAL_C_INCLUDES := \
    $(libgps_gen_intermediates) \
    $(empty)
LOCAL_SHARED_LIBRARIES := \
    libgps \
    $(empty)

include $(BUILD_EXECUTABLE)

########################################
# gpxlogger utility

include $(CLEAR_VARS)

LOCAL_MODULE := gpxlogger
all-gpsd-modules: $(LOCAL_MODULE)

LOCAL_SRC_FILES := \
    gpxlogger.c \
    $(empty)
LOCAL_C_INCLUDES := \
    $(libgps_gen_intermediates) \
    $(empty)
LOCAL_SHARED_LIBRARIES := \
    libgps \
    $(empty)

include $(BUILD_EXECUTABLE)

########################################
# lcdgps utility

include $(CLEAR_VARS)

LOCAL_MODULE := lcdgps
all-gpsd-modules: $(LOCAL_MODULE)

LOCAL_SRC_FILES := \
    lcdgps.c \
    $(empty)
LOCAL_C_INCLUDES := \
    $(libgps_gen_intermediates) \
    $(empty)
LOCAL_SHARED_LIBRARIES := \
    libgps \
    $(empty)

include $(BUILD_EXECUTABLE)

########################################
# cgps utility

include $(CLEAR_VARS)

LOCAL_MODULE := cgps
ifneq ($(gpsd_with_ncurses),)
all-gpsd-modules: $(LOCAL_MODULE)
endif

LOCAL_SRC_FILES := \
    cgps.c \
    $(empty)
LOCAL_C_INCLUDES := \
    $(libgps_gen_intermediates) \
    $(empty)
LOCAL_SHARED_LIBRARIES := \
    libgps \
    $(empty)

include $(BUILD_EXECUTABLE)

########################################
# gps.gpsd HAL module
#
# To use this module:
#
#   1.1) define BOARD_USES_CATB_GPSD_GPS to true, and/or
#   1.2) ADDITIONAL_BUILD_PROPERTIES += ro.hardware.gps=<variant>
#   2) add gps.<variant> to PRODUCT_PACKAGES
#
# Note that `gps.${ro.hardware}` have higher priority than
# $(TARGET_BOOTLOADER_BOARD_NAME) and $(TARGET_BOARD_PLATFORM), which
# might even be empty in emulators. So if your platform have already
# `gps.${ro.hardware}`, or `gps.gpsd` is chosen here, you must define
# ${ro.hardware.gps} to override it.

include $(CLEAR_VARS)

LOCAL_MODULE := gps.$(word 1,$(if $(filter true,$(BOARD_USES_CATB_GPSD_GPS)),$(TARGET_BOOTLOADER_BOARD_NAME) $(TARGET_BOARD_PLATFORM) default) gpsd)
all-gpsd-modules: $(LOCAL_MODULE)

LOCAL_MODULE_RELATIVE_PATH := hw

LOCAL_SRC_FILES := \
    android/hal_module.c \
    $(empty)

LOCAL_SHARED_LIBRARIES := \
    liblog \
    libhardware \
    libgps \
    $(empty)

LOCAL_REQUIRED_MODULES := gpsd

include $(BUILD_SHARED_LIBRARY)
