#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <math.h>
#include <time.h>

#include <gps.h>

#define  LOG_TAG  "gps_gpsd"
#include <cutils/log.h>
#include <cutils/sockets.h>
#include <hardware/gps.h>

#define  GPS_DEBUG  0

#if GPS_DEBUG
#  define  D(...)   ALOGD(__VA_ARGS__)
#  define  V(...)   ALOGV(__VA_ARGS__)
#else
#  define  D(...)   ((void)0)
#  define  V(...)   ((void)0)
#endif

#if !defined(MIN)
#  define MIN(x,y) ((x) < (y) ? (x) : (y))
#endif

enum {
    FD_CONTROL = 0,
    FD_WORKER = 1,
};

/* commands sent to the gps thread */
enum {
    CMD_QUIT,
    CMD_START,
    CMD_STOP,
    CMD_CHANGE_INTERVAL,
    _CMD_COUNT
};

typedef struct {
    int initialized:1;
    int watch_enabled:1;
    struct gps_data_t gps_data;
    int control_fds[2];
    pthread_t worker;
    int epoll_fd;
    int timer_fd;
    GpsCallbacks callbacks;
    struct timespec report_interval;
    char *version;
    GpsStatus status;
    int fix_mode;
    int last_reported_fix_mode;
    GpsLocation location;
} GpsState;

static GpsState _gps_state[1];

static void epoll_add(int epoll_fd, int fd, int events)
{
    struct epoll_event ev;

    ev.events = events;
    ev.data.fd = fd;
    if (TEMP_FAILURE_RETRY(epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev)) < 0)
        ALOGE("epoll_add() unexpected error: %s", strerror(errno));
}

static void epoll_del(int epoll_fd, int fd)
{
    struct epoll_event ev;

    if (TEMP_FAILURE_RETRY(epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, &ev)) < 0)
        ALOGE("epoll_del() unexpected error: %s", strerror(errno));
}

static int write_control_command(GpsState *s, char cmd)
{
    return TEMP_FAILURE_RETRY(write(s->control_fds[FD_CONTROL], &cmd,
                                    sizeof(cmd)));
}

static void setup_timerfd(int timer_fd, const struct timespec *preferred)
{
    if (timer_fd < 0)
        return;

    struct itimerspec its;

    memset(&its, 0, sizeof(its));
    timerfd_gettime(timer_fd, &its);
    if ((preferred->tv_sec == its.it_interval.tv_sec)
            && (preferred->tv_nsec == its.it_interval.tv_nsec))
        return;

    its.it_value = *preferred;
    its.it_interval = *preferred;
    if (timerfd_settime(timer_fd, 0, &its, NULL) < 0)
        ALOGE("timerfd_settime() unexpected error: %s", strerror(errno));
}

static void disable_timerfd(int timer_fd)
{
    if (timer_fd < 0)
        return;

    struct itimerspec its;

    memset(&its, 0, sizeof(its));
    timerfd_settime(timer_fd, 0, &its, NULL);
}

static void report_status(GpsState *s, GpsStatusValue status)
{
    if (s->status.status == status)
        return;

    s->status.status = status;

    if (!s->callbacks.status_cb)
        return;

    D("status_cb({.status: %d})", s->status.status);
    s->callbacks.status_cb(&s->status);
}

static void report_location(GpsState *s, int timer_triggered)
{
    if (!timer_triggered) {
        // Periodical report triggered by gpsd event.

        if (!(s->location.flags & GPS_LOCATION_HAS_LAT_LONG)
                && (s->last_reported_fix_mode < MODE_2D)) {
            V("skipped continuous NO_FIX location reports");
            return;
        }

        if (s->report_interval.tv_sec || s->report_interval.tv_nsec) {
            V("location to be reported in next timer event");
            return;
        }
    }

    s->last_reported_fix_mode = s->fix_mode;

    if (!s->callbacks.location_cb)
        return;

    D("location_cb({.lat: %.6f, .lon: %.6f, .flags: %d, "
                   ".speed: %.3f, .bearing: %.1f, "
                   ".accuracy: %d, .timestamp: %"PRId64"}), mode: %d",
            s->location.latitude, s->location.longitude,
            s->location.flags, s->location.speed,
            s->location.bearing, (int) floor(s->location.accuracy + 0.5),
            s->location.timestamp, s->fix_mode);
    s->callbacks.location_cb(&s->location);
}

static void reconnect_gpsd(GpsState *s)
{
    int connected = 1;
    if (gps_open(NULL, NULL, &s->gps_data))
        connected = 0;
    else if (gps_stream(&s->gps_data, WATCH_ENABLE, NULL)) {
        connected = 0;
        gps_close(&s->gps_data);
    }

    if (connected) {
        epoll_add(s->epoll_fd, s->gps_data.gps_fd,
                EPOLLIN | EPOLLERR | EPOLLHUP);

        s->status.status = GPS_STATUS_NONE;
        s->fix_mode = MODE_NOT_SEEN;
        s->last_reported_fix_mode = MODE_NOT_SEEN;
        s->location.flags = 0;

        if (gps_send(&s->gps_data, "?DEVICES;\n")) {
            ALOGE("Failed to query devices list. Assume off.");
            report_status(s, GPS_STATUS_ENGINE_OFF);
        }

        setup_timerfd(s->timer_fd, &s->report_interval);
        return;
    }

    ALOGE("failed to connect gpsd server");

    // Setup reconnect timer
    struct itimerspec its;

    its.it_interval.tv_sec = its.it_value.tv_sec = 3;
    its.it_interval.tv_nsec = its.it_value.tv_nsec = 0;
    if (timerfd_settime(s->timer_fd, 0, &its, NULL) < 0)
        ALOGE("failed to setup reconnect timer. Stopped forever.");
}

static void handle_control_start(GpsState *s)
{
    if (s->watch_enabled)
        return;

    s->watch_enabled = 1;
    reconnect_gpsd(s);
}

static void handle_control_stop(GpsState *s)
{
    if (!s->watch_enabled)
        return;

    s->watch_enabled = 0;

    if (s->gps_data.gps_fd >= 0) {
        epoll_del(s->epoll_fd, s->gps_data.gps_fd);

        gps_stream(&s->gps_data, WATCH_DISABLE, NULL);
        gps_close(&s->gps_data);
    }

    disable_timerfd(s->timer_fd);
}

static int handle_control(GpsState *s)
{
    char cmd = _CMD_COUNT;
    int ret;

    if (TEMP_FAILURE_RETRY(read(s->control_fds[FD_WORKER], &cmd,
            sizeof(cmd))) < 0) {
        ALOGE("read control fd unexpected error: %s", strerror(errno));
        return -1;
    }

    V("gps thread control command: %d", cmd);

    switch (cmd) {
        case CMD_QUIT:
            return -1;

        case CMD_START:
            handle_control_start(s);
            break;

        case CMD_STOP:
            handle_control_stop(s);
            break;

        case CMD_CHANGE_INTERVAL:
            if (s->status.status == GPS_STATUS_SESSION_BEGIN)
                setup_timerfd(s->timer_fd, &s->report_interval);

            break;

        default:
            ALOGE("unknown control command: %d", cmd);
            break;
    }

    return 0;
}

static void handle_gpsd_version(GpsState *s, struct gps_data_t *gps_data)
{
    int needed;

#define VERSION_PRINTF_ARGS \
        "gpsd release %s rev %s", \
        gps_data->version.release, gps_data->version.rev
    needed = snprintf(NULL, 0, VERSION_PRINTF_ARGS);
    if (!s->version || ((int)strlen(s->version) < needed)) {
        if (s->version)
            free(s->version);
        s->version = (char*) calloc(needed + 1, 1);
    }
    snprintf(s->version, needed + 1, VERSION_PRINTF_ARGS);
#undef VERSION_PRINTF_ARGS

    ALOGI("%s", s->version);
}

static void handle_gpsd_devicelist(GpsState *s, struct gps_data_t *gps_data)
{
    int activated = 0;

    for (int i = 0; i < gps_data->devices.ndevices; ++i) {
        double timestamp = gps_data->devices.list[i].activated;
        if (!isnan(timestamp) && (floor(timestamp) > 0)) {
            activated = 1;
            break;
        }
    }

    if (!activated)
        report_status(s, GPS_STATUS_ENGINE_OFF);
    else if ((s->status.status == GPS_STATUS_ENGINE_OFF)
            || (s->status.status == GPS_STATUS_NONE))
        report_status(s, GPS_STATUS_ENGINE_ON);
}

static void handle_gpsd_satellite_gnss(GpsState *s,
        struct gps_data_t *gps_data)
{
    GnssSvStatus statuses;
    int used = 0;

    statuses.size = sizeof(statuses);
    statuses.num_svs =
            MIN(GNSS_MAX_SVS, gps_data->satellites_visible);
    for (int i = 0; i < statuses.num_svs; ++i) {
        GnssSvInfo *info = &statuses.gnss_sv_list[i];
        const struct satellite_t *satellite = &gps_data->skyview[i];

        info->size = sizeof(info);

        info->svid = satellite->PRN;
        if (GPS_PRN(satellite->PRN))
            info->constellation = GNSS_CONSTELLATION_GPS;
        else if (GBAS_PRN(satellite->PRN)) {
            info->constellation = GNSS_CONSTELLATION_GLONASS;
            info->svid -= GLONASS_PRN_OFFSET;
        } else if (SBAS_PRN(satellite->PRN))
            info->constellation = GNSS_CONSTELLATION_SBAS;
#define QZSS_PRN(n) (((n) >= 193) && ((n) <= 200))
        else if (QZSS_PRN(satellite->PRN))
            info->constellation = GNSS_CONSTELLATION_QZSS;
#define BEIDOU_PRN(n) (((n) >= 201) && ((n) <= 235))
        else if (BEIDOU_PRN(satellite->PRN)) {
            info->constellation = GNSS_CONSTELLATION_BEIDOU;
            info->svid -= 200;
        } else
            info->constellation = GNSS_CONSTELLATION_UNKNOWN;

        info->c_n0_dbhz = satellite->ss;
        info->elevation = satellite->elevation;
        info->azimuth = satellite->azimuth;
        info->flags = GNSS_SV_FLAGS_NONE;
        if (satellite->used) {
            info->flags |= GNSS_SV_FLAGS_USED_IN_FIX;
            used++;
        }
    }

    D("gnss_sv_status_cb({.num_svs: %d, ...}), used=%d",
            statuses.num_svs, used);
    s->callbacks.gnss_sv_status_cb(&statuses);
}

static void handle_gpsd_satellite_legacy(GpsState *s,
        struct gps_data_t *gps_data)
{
    GpsSvStatus statuses;

    statuses.size = sizeof(statuses);
    statuses.num_svs =
            MIN(GPS_MAX_SVS, gps_data->satellites_visible);
    statuses.ephemeris_mask = 0;
    statuses.almanac_mask = 0;
    statuses.used_in_fix_mask = 0;
    for (int i = 0; i < statuses.num_svs; ++i) {
        GpsSvInfo *info = &statuses.sv_list[i];
        const struct satellite_t *satellite = &gps_data->skyview[i];

        info->size = sizeof(info);
        info->prn = satellite->PRN;
        info->snr = satellite->ss;
        info->elevation = satellite->elevation;
        info->azimuth = satellite->azimuth;
        if (satellite->used)
            statuses.used_in_fix_mask |= (0x01U << i);
    }

    D("sv_status_cb({.num_svs: %d, ...})", statuses.num_svs);
    s->callbacks.sv_status_cb(&statuses);
}

static void handle_gpsd_satellite(GpsState *s, struct gps_data_t *gps_data)
{
    report_status(s, GPS_STATUS_SESSION_BEGIN);

    if (s->callbacks.gnss_sv_status_cb)
        handle_gpsd_satellite_gnss(s, gps_data);
    else if (s->callbacks.sv_status_cb)
        handle_gpsd_satellite_legacy(s, gps_data);
}

static void handle_gpsd_status(GpsState *s, struct gps_data_t *gps_data)
{
    report_status(s, GPS_STATUS_SESSION_BEGIN);

    s->fix_mode = gps_data->fix.mode;
    if (s->fix_mode < MODE_2D) {
        D("No fix yet. Ignored.");
        return;
    }

    s->location.flags = 0;
    if ((gps_data->set & LATLON_SET) && (gps_data->set & TIME_SET)) {
        s->location.latitude = gps_data->fix.latitude;
        s->location.longitude = gps_data->fix.longitude;
        s->location.timestamp = floor(gps_data->fix.time * 1000);
        s->location.flags |= GPS_LOCATION_HAS_LAT_LONG;
    }
    if (gps_data->set & ALTITUDE_SET) {
        s->location.altitude = gps_data->fix.altitude;
        s->location.flags |= GPS_LOCATION_HAS_ALTITUDE;
    }
    if (gps_data->set & SPEED_SET) {
        s->location.speed = gps_data->fix.speed;
        s->location.flags |= GPS_LOCATION_HAS_SPEED;
    }
    if (gps_data->set & TRACK_SET) {
        s->location.bearing = gps_data->fix.track;
        s->location.flags |= GPS_LOCATION_HAS_BEARING;
    }
    if (gps_data->set & (HERR_SET | VERR_SET)) {
        double err = 0.0;
        if (gps_data->set & HERR_SET)
            err = (gps_data->fix.epx > gps_data->fix.epy) ?
                    gps_data->fix.epx : gps_data->fix.epy;
        if ((gps_data->set & VERR_SET) && (gps_data->fix.epv > err))
            err = gps_data->fix.epv;

        s->location.accuracy = err;
        s->location.flags |= GPS_LOCATION_HAS_ACCURACY;
    }

    report_location(s, 0);
}

static int handle_gpsd(GpsState *s)
{
    struct gps_data_t *gps_data = &s->gps_data;

    if (gps_read(gps_data) == -1) {
        ALOGE("error while reading from gps daemon socket: %s:",
                strerror(errno));
        return -1;
    }

    if (gps_data->set & VERSION_SET)
        handle_gpsd_version(s, gps_data);

    if (gps_data->set & DEVICELIST_SET)
        handle_gpsd_devicelist(s, gps_data);

    if (gps_data->set & SATELLITE_SET)
        handle_gpsd_satellite(s, gps_data);

    if (gps_data->set & STATUS_SET)
        handle_gpsd_status(s, gps_data);

    return 0;
}

static void handle_timer(GpsState *s)
{
    // discard content for next run.
    uint64_t count;
    read(s->timer_fd, &count, sizeof(count));

    if (s->gps_data.gps_fd >= 0)
        report_location(s, 1);
    else if (s->watch_enabled)
        reconnect_gpsd(s);
}

static int worker_loop(GpsState *s)
{
    struct epoll_event events[3];
    int ne, nevents;
    int ret = 0;

    nevents = epoll_wait(s->epoll_fd, events,
            (sizeof(events) / sizeof(events[0])), -1);
    if (nevents < 0) {
        if (errno != EINTR)
            ALOGE("epoll_wait() unexpected error: %s", strerror(errno));
        return ret;
    }

    if (s->callbacks.acquire_wakelock_cb)
        s->callbacks.acquire_wakelock_cb();

    for (ne = 0; ne < nevents; ne++) {
        struct epoll_event *event = &events[ne];
        int fd = events[ne].data.fd;

        if (fd == s->control_fds[FD_WORKER]) {
            V("events %d for worker control fd", event->events);
            if (handle_control(s) < 0) {
                ret = -1;
                break;
            }
        } else if (fd == s->gps_data.gps_fd) {
            V("events %d for gpsd socket", event->events);
            if ((event->events & (EPOLLERR | EPOLLHUP))
                    || (handle_gpsd(s) < 0)) {
                ALOGE("gpsd socket error. reconnecting ...");

                epoll_del(s->epoll_fd, s->gps_data.gps_fd);
                gps_close(&s->gps_data);

                reconnect_gpsd(s);
            }
        } else if (fd == s->timer_fd) {
            V("events %d for timer fd", event->events);
            handle_timer(s);
        } else
            ALOGE("epoll_wait() returned unkown fd %d ?", fd);
    } /* nevents loop */

    if (s->callbacks.release_wakelock_cb)
        s->callbacks.release_wakelock_cb();

    return ret;
}

static void worker_thread(void *thread_data)
{
    GpsState *s = (GpsState*)thread_data;
    int quit = 0;
    uint32_t capabilities = 0;

    ALOGI("gps thread running");

    // register file descriptors for polling
    s->epoll_fd = epoll_create(3);
    epoll_add(s->epoll_fd, s->control_fds[FD_WORKER], EPOLLIN);

    s->timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (s->timer_fd < 0)
        ALOGE("timerfd_create() unexpected error: %s", strerror(errno));
    else {
        epoll_add(s->epoll_fd, s->timer_fd, EPOLLIN);

        capabilities |= GPS_CAPABILITY_SCHEDULING;
    }

    if (s->callbacks.set_capabilities_cb)
        s->callbacks.set_capabilities_cb(capabilities);

    // now loop
    while (worker_loop(s) == 0);

    s->watch_enabled = 0;

    if (s->version) {
        free(s->version);
        s->version = NULL;
    }

    if (s->gps_data.gps_fd >= 0)
        gps_close(&s->gps_data);

    if (s->timer_fd >= 0) {
        close(s->timer_fd);
        s->timer_fd = -1;
    }

    close(s->epoll_fd);
    s->epoll_fd = -1;

    ALOGI("gps thread quit");
}

static int gps_iface_init(GpsCallbacks *callbacks)
{
    GpsState *s = _gps_state;

    if (s->initialized)
        return 0;

    memset(s, 0, sizeof(*s));

    // System GpsCallbacks might be smaller/larger than what we have.
    memcpy(&s->callbacks, callbacks,
            MIN(callbacks->size, sizeof(s->callbacks)));

    // Initialize fields shouldn't change between sessions.
    s->status.size = sizeof(s->status);
    s->location.size = sizeof(s->location);

    if (socketpair(AF_LOCAL, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0,
                   s->control_fds) < 0) {
        ALOGE("failed to create control sockets");
        return -1;
    }

    s->worker = callbacks->create_thread_cb("gps_worker_thread",
            worker_thread, s);
    if (!s->worker) {
        ALOGE("could not create gps thread: %s", strerror(errno));

        close(s->control_fds[FD_CONTROL]);
        s->control_fds[FD_CONTROL] = -1;
        close(s->control_fds[FD_WORKER]);
        s->control_fds[FD_WORKER] = -1;
        return -1;
    }

    s->initialized = 1;
    V("gps state initialized");

    return 0;
}

static void gps_iface_cleanup()
{
    GpsState *s = _gps_state;

    if (!s->initialized)
        return;

    // tell the thread to quit, and wait for it
    void *dummy;
    write_control_command(s, CMD_QUIT);
    pthread_join(s->worker, &dummy);

    // close the control socket pair
    close(s->control_fds[FD_CONTROL]);
    s->control_fds[FD_CONTROL] = -1;
    close(s->control_fds[FD_WORKER]);
    s->control_fds[FD_WORKER] = -1;

    s->initialized = 0;
}


static int gps_iface_start()
{
    GpsState *s = _gps_state;

    if (!s->initialized) {
        ALOGE("%s: called with uninitialized state !!", __FUNCTION__);
        return -1;
    }

    return write_control_command(s, CMD_START) > 0 ? 0 : -1;
}


static int gps_iface_stop()
{
    GpsState *s = _gps_state;

    if (!s->initialized) {
        ALOGE("%s: called with uninitialized state !!", __FUNCTION__);
        return -1;
    }

    return write_control_command(s, CMD_STOP) > 0 ? 0 : -1;
}


static int gps_iface_inject_time(GpsUtcTime time __unused,
        int64_t timeReference __unused, int uncertainty __unused)
{
    V("%s", __FUNCTION__);
    return 0;
}

static int gps_iface_inject_location(double latitude __unused,
        double longitude __unused, float accuracy __unused)
{
    V("%s", __FUNCTION__);
    return 0;
}

static void gps_iface_delete_aiding_data(GpsAidingData flags __unused)
{
    V("%s", __FUNCTION__);
}

static int gps_iface_set_position_mode(GpsPositionMode mode __unused,
        GpsPositionRecurrence recurrence, uint32_t min_interval,
        uint32_t preferred_accuracy __unused,
        uint32_t preferred_time __unused)
{
    GpsState *s = _gps_state;

    if (!s->initialized) {
        ALOGE("%s: called with uninitialized state !!", __FUNCTION__);
        return -1;
    }

    if (recurrence != GPS_POSITION_RECURRENCE_PERIODIC) {
        ALOGE("%s: recurrence %d not supported", __FUNCTION__, recurrence);
        return -1;
    }

    D("%s: mode=%d, recurrence=%d, min_interval=%d, "
            "preferred_accuracy=%d, preferred_time=%d",
            __FUNCTION__, mode, recurrence, min_interval,
            preferred_accuracy, preferred_time);
    s->report_interval.tv_sec = min_interval / 1000;
    s->report_interval.tv_nsec = (min_interval % 1000) * 1000000;
    return write_control_command(s, CMD_CHANGE_INTERVAL) > 0 ? 0 : -1;
}

/* GPS_DEBUG_INTERFACE */
static size_t gps_debug_iface_get_internal_state(char*, size_t);

static const void* gps_iface_get_extension(const char *name)
{
    static const GpsDebugInterface gps_debug_iface = {
        sizeof(GpsDebugInterface),
        gps_debug_iface_get_internal_state,
    };

    D("%s: %s", __FUNCTION__, name);
    if (0 == strcmp(name, GPS_DEBUG_INTERFACE))
        return &gps_debug_iface;

    return NULL;
}

static size_t gps_debug_iface_get_internal_state(char *buffer, size_t bufferSize)
{
    GpsState *s = _gps_state;

    if (!s->version)
        return 0;

    return strlcpy(buffer, s->version, bufferSize);
}

static int device_close(struct hw_device_t *device __unused)
{
    V("%s: %p", __FUNCTION__, device);
    return 0;
}

static const GpsInterface* device_get_gps_interface(
        struct gps_device_t *dev __unused)
{
    static const GpsInterface iface = {
        sizeof(GpsInterface),
        gps_iface_init,
        gps_iface_start,
        gps_iface_stop,
        gps_iface_cleanup,
        gps_iface_inject_time,
        gps_iface_inject_location,
        gps_iface_delete_aiding_data,
        gps_iface_set_position_mode,
        gps_iface_get_extension,
    };

    return &iface;
}

static int module_open(const struct hw_module_t *module,
        char const *name, struct hw_device_t **device)
{
    if (strcmp(name, GPS_HARDWARE_MODULE_ID) != 0)
        return -1;

    struct gps_device_t *dev = calloc(1, sizeof(struct gps_device_t));

    dev->common.tag = HARDWARE_DEVICE_TAG;
    dev->common.version = 0;
    dev->common.module = (struct hw_module_t*)module;
    dev->common.close = device_close;
    dev->get_gps_interface = device_get_gps_interface;

    *device = (struct hw_device_t*)dev;
    return 0;
}

static struct hw_module_methods_t gps_module_methods = {
    .open = module_open
};

struct hw_module_t HAL_MODULE_INFO_SYM = {
    .tag = HARDWARE_MODULE_TAG,
    .module_api_version = HARDWARE_MODULE_API_VERSION(0, 1),
    .hal_api_version = HARDWARE_HAL_API_VERSION,
    .id = GPS_HARDWARE_MODULE_ID,
    .name = "Catb.org gpsd GPS Module",
    .author = "You-Sheng Yang",
    .methods = &gps_module_methods,
};
