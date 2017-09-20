/*
 * ANDROID QEMUD pipe.
 *
 * The entry points for driver_qemudpipe
 *
 * This file is Copyright (c) 2017 by You-Sheng Yang
 * BSD terms apply: see the file COPYING in the distribution root for details.
 */

#ifndef _DRIVER_QEMUDPIPE_H_
#define _DRIVER_QEMUDPIPE_H_

#if defined(QEMUDPIPE_ENABLE)

int qemudpipe_open(struct gps_device_t *session);

#endif /* of defined(QEMUDPIPE_ENABLE) */

#endif /* of ifndef _DRIVER_QEMUDPIPE_H_ */

