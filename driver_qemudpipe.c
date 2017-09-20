/*
 * ANDROID QEMUD pipe.
 *
 * This file is Copyright (c) 2017 by You-Sheng Yang
 * BSD terms apply: see the file COPYING in the distribution root for details.
 */

#include "gpsd.h"

#if defined(QEMUDPIPE_ENABLE)
#include <hardware/qemud.h>
#include "driver_qemudpipe.h"

int qemudpipe_open(struct gps_device_t *session)
{
    const char *name = session->gpsdata.dev.path + strlen("qemud://");
    int fd = qemud_channel_open(name);
    if (fd < 0) {
        gpsd_log(&session->context->errout, LOG_ERROR,
                 "qemudpipe open: no named channel '%s' detected\n", name);
        return -1;
    }

    gpsd_switch_driver(session, "qemudpipe");
    session->gpsdata.gps_fd = fd;
    return session->gpsdata.gps_fd;
}

/* *INDENT-OFF* */
const struct gps_type_t driver_qemudpipe = {
    .type_name      = "qemudpipe",	/* full name of type */
    .packet_type    = NMEA_PACKET,	/* associated lexer packet type */
    .flags          = DRIVER_STICKY,	/* remember this */
    .trigger        = NULL,		/* it's the default */
    .channels       = 0,		/* not used */
    .probe_detect   = NULL,		/* no probe */
    .get_packet     = generic_get,	/* how to get a packet */
    .parse_packet   = generic_parse_input,	/* how to interpret a packet */
    .rtcm_writer    = NULL,		/* write RTCM data straight */
    .init_query     = NULL,		/* non-perturbing initial query */
    .event_hook     = NULL,		/* lifetime event handler */
#ifdef RECONFIGURE_ENABLE
    .speed_switcher = NULL,		/* no speed switcher */
    .mode_switcher  = NULL,		/* no mode switcher */
    .rate_switcher  = NULL,		/* no sample-rate switcher */
    .min_cycle      = 1,		/* not relevant, no rate switch */
#endif /* RECONFIGURE_ENABLE */
#ifdef CONTROLSEND_ENABLE
    .control_send   = NULL,		/* how to send control strings */
#endif /* CONTROLSEND_ENABLE */
#ifdef TIMEHINT_ENABLE
    .time_offset     = NULL,		/* no method for NTP fudge factor */
#endif /* TIMEHINT_ENABLE */
};
/* *INDENT-ON* */

/* end */

#endif /* of  defined(QEMUDPIPE_ENABLE) */
