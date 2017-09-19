#include <stdlib.h>

#define  LOG_TAG  "gps_gpsd"
#include <cutils/log.h>
#include <hardware/gps.h>


static int device_close(struct hw_device_t *device __unused)
{
    return 0;
}

static const GpsInterface* device_get_gps_interface(
        struct gps_device_t *dev __unused)
{
    return NULL;
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
