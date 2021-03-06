/*
 * Copyright 2008, The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "Sensors"

#include <sensors.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <math.h>
#include <poll.h>

#include <linux/input.h>

#include <cutils/log.h>
#include <cutils/atomic.h>

#include "akm8973.h"

/*****************************************************************************/

#define AKM_DEVICE_NAME             "/dev/akm8973_aot"
#define ACCEL_DEVICE_NAME           "/sys/class/i2c-adapter/i2c-0/0-0018/mode"

// sensor IDs must be a power of two and
// must match values in SensorManager.java
#define EVENT_TYPE_ACCEL_X          ABS_X
#define EVENT_TYPE_ACCEL_Y          ABS_Y
#define EVENT_TYPE_ACCEL_Z          ABS_Z
#define EVENT_TYPE_ACCEL_STATUS     ABS_WHEEL

#define EVENT_TYPE_YAW              ABS_RX
#define EVENT_TYPE_PITCH            ABS_RY
#define EVENT_TYPE_ROLL             ABS_RZ
#define EVENT_TYPE_ORIENT_STATUS    ABS_RUDDER

#define EVENT_TYPE_MAGV_X           ABS_HAT0X
#define EVENT_TYPE_MAGV_Y           ABS_HAT0Y
#define EVENT_TYPE_MAGV_Z           ABS_BRAKE

#define EVENT_TYPE_TEMPERATURE      ABS_THROTTLE
#define EVENT_TYPE_STEP_COUNT       ABS_GAS

#define EVENT_TYPE_ROTATION_IRQ     0x1b

// 720 LSG = 1G
//#define LSG                         (720.0f)
// for 4g   LSG =  512  , for  2g  LSG = 1024
#define LSG                         (512.0f)

// conversion of acceleration data to SI units (m/s^2)
#define CONVERT_A                   (GRAVITY_EARTH / LSG)
#define CONVERT_A_X                 (CONVERT_A)
#define CONVERT_A_Y                 (CONVERT_A)
#define CONVERT_A_Z                 (CONVERT_A)

// conversion of magnetic data to uT units
#define CONVERT_M                   (1.0f/16.0f)
#define CONVERT_M_X                 (CONVERT_M)
#define CONVERT_M_Y                 (CONVERT_M)
#define CONVERT_M_Z                 (CONVERT_M)

#define CONVERT_O                   (1.0f/64.0f)

#define SENSOR_STATE_MASK           (0x7FFF)


#define SENSORS_MASK               0x18f

#define SENSORS_ORIENTATION        0x01
#define SENSORS_ACCELERATION       0x02
#define SENSORS_TEMPERATURE        0x04
#define SENSORS_MAGNETIC_FIELD     0x08
#define SENSORS_ORIENTATION_RAW    0x80
#define SENSORS_ROTATION           0x100

#define SENSORS_ORIENTATION_HANDLE        0
#define SENSORS_ACCELERATION_HANDLE       1
#define SENSORS_TEMPERATURE_HANDLE        2
#define SENSORS_MAGNETIC_FIELD_HANDLE     3
#define SENSORS_ORIENTATION_RAW_HANDLE    7
#define SENSORS_ROTATION_HANDLE           8

#define SUPPORTED_SENSORS (SENSORS_ORIENTATION | \
              SENSORS_ACCELERATION | \
              SENSORS_MAGNETIC_FIELD | \
              SENSORS_ORIENTATION_RAW | \
		SENSORS_TEMPERATURE | \
			  SENSORS_ROTATION)

/*****************************************************************************/

static int sensors_control_device_close(struct hw_device_t *dev);
static int sensors_control_open_data_source(struct sensors_control_device_t *dev, int sensor);
static int sensors_control_activate(struct sensors_control_device_t *dev, 
            int handle, int enabled);
static int sensors_control_wake(struct sensors_control_device_t *dev);

static int sensors_data_device_close(struct hw_device_t *dev);
static int sensors_data_data_open(struct sensors_data_device_t *dev, int fd, int sensor);
static int sensors_data_data_close(struct sensors_data_device_t *dev);
static int sensors_data_poll(struct sensors_data_device_t *dev, 
            sensors_data_t* data);

static int sensors_get_sensors_list(struct sensors_module_t* module,
	struct sensor_t const**);

static int sensors_device_open(const struct hw_module_t* module, const char* name,
        struct hw_device_t** device);

/*****************************************************************************/

static int sAccelFD = -1; // accelerometer input (currently not avail for regular applications)
static int sAccelModeFD = -1; // accelerometer mode file (will be used only by system process)
static int sAkmFD = -1;
static uint32_t sActiveSensors = 0;
static char *sAccelSpeed = "s";
//static char *enable_irq = "2 m 4g"; // turn accelerometer on
static char *disable_irq = "0 s 4g";
//                               C1C2C3C4 C T D C T D 
//static char *enable_irq =  "raw0127FF19d0860d0a890d0a"; // turn accelerometer on
//static char *disable_irq = "raw0007FF0000000000000000";

#define ACCEL_SETTINGS  "/system/etc/accel_settings.conf"
#define ACCEL_DEBOUNCE  "/system/etc/accel_debounce.conf"
char settings[64];
int debounce = 450;


/*****************************************************************************/

/*
 * We use a Least Mean Squares filter to smooth out the output of the yaw
 * sensor.
 *
 * The goal is to estimate the output of the sensor based on previous acquired
 * samples.
 *
 * We approximate the input by a line with the equation:
 *      Z(t) = a * t + b
 *
 * We use the Least Mean Squares method to calculate a and b so that the
 * distance between the line and the measured COUNT inputs Z(t) is minimal.
 *
 * In practice we only need to compute b, which is the value we're looking for
 * (it's the estimated Z at t=0). However, to improve the latency a little bit,
 * we're going to discard a certain number of samples that are too far from
 * the estimated line and compute b again with the new (trimmed down) samples.
 *
 * notes:
 * 'a' is the slope of the line, and physicaly represent how fast the input
 * is changing. In our case, how fast the yaw is changing, that is, how fast the
 * user is spinning the device (in degre / nanosecond). This value should be
 * zero when the device is not moving.
 *
 * The minimum distance between the line and the samples (which we are not
 * explicitely computing here), is an indication of how bad the samples are
 * and gives an idea of the "quality" of the estimation (well, really of the
 * sensor values).
 *
 */

/* sensor rate in me */
#define SENSORS_RATE_MS     20
/* timeout (constant value) in ms */
#define SENSORS_TIMEOUT_MS  1000
/* # of samples to look at in the past for filtering */
#define COUNT               24
/* prediction ratio */
#define PREDICTION_RATIO    (1.0f/3.0f)
/* prediction time in seconds (>=0) */
#define PREDICTION_TIME     ((SENSORS_RATE_MS*COUNT/1000.0f)*PREDICTION_RATIO)

static float mV[COUNT*2];
static float mT[COUNT*2];
static int mIndex;

static inline
float normalize(float x)
{
    x *= (1.0f / 360.0f);
    if (fabsf(x) >= 0.5f)
        x = x - ceilf(x + 0.5f) + 1.0f;
    if (x < 0)
        x += 1.0f;
    x *= 360.0f;
    return x;
}

static void LMSInit(void)
{
    memset(mV, 0, sizeof(mV));
    memset(mT, 0, sizeof(mT));
    mIndex = COUNT;
}

static float LMSFilter(int64_t time, int v)
{
    const float ns = 1.0f / 1000000000.0f;
    const float t = time*ns;
    float v1 = mV[mIndex];
    int i;
/* IF new value is 360 degrie different from last one adjust stored value or compass pointer  will rotate in opposite direction */ 

     if ( (v-v1) > 360 ) {
         for (i=0 ; i<COUNT*2 ; i++) {  
            mV[i] += 360;  }
        } else if ( (v1-v) > 360)  {
          for (i=0 ; i<COUNT*2 ; i++) {  
            mV[i] -= 360;  }
        }
    v1 = mV[mIndex];

    if ((v-v1) > 180) {
        v -= 360;
    } else if ((v1-v) > 180) {
        v += 360;
    }
    /* Manage the circular buffer, we write the data twice spaced by COUNT
     * values, so that we don't have to memcpy() the array when it's full */
    mIndex++;
    if (mIndex >= COUNT*2)
        mIndex = COUNT;
    mV[mIndex] = v;
    mT[mIndex] = t;
    mV[mIndex-COUNT] = v;
    mT[mIndex-COUNT] = t;

    float A, B, C, D, E;
    float a, b;


    A = B = C = D = E = 0;
    for (i=0 ; i<COUNT-1 ; i++) {
        const int j = mIndex - 1 - i;
        const float Z = mV[j];
        const float T = 0.5f*(mT[j] + mT[j+1]) - t;
        float dT = mT[j] - mT[j+1];
        dT *= dT;
        A += Z*dT;
        B += T*(T*dT);
        C +=   (T*dT);
        D += Z*(T*dT);
        E += dT;
    }
    b = (A*B + C*D) / (E*B + C*C);
    a = (E*b - A) / C;
    float f = b + PREDICTION_TIME*a;

    //LOGD("A=%f, B=%f, C=%f, D=%f, E=%f", A,B,C,D,E);
    //LOGD("%lld  %d  %f  %f", time, v, f, a);

    f = normalize(f);
    return f;
}

/*****************************************************************************/

static int open_input(char* _name)
{
    /* scan all input drivers and look for _name */
    int fd = -1;
    const char *dirname = "/dev/input";
    char devname[PATH_MAX];
    char *filename;
    DIR *dir;
    struct dirent *de;
    dir = opendir(dirname);
    if(dir == NULL)
        return -1;
    strcpy(devname, dirname);
    filename = devname + strlen(devname);
    *filename++ = '/';
    while((de = readdir(dir))) {
        if(de->d_name[0] == '.' &&
           (de->d_name[1] == '\0' ||
            (de->d_name[1] == '.' && de->d_name[2] == '\0')))
            continue;
        strcpy(filename, de->d_name);
        fd = open(devname, O_RDONLY);
        if (fd>=0) {
            char name[80];
            if (ioctl(fd, EVIOCGNAME(sizeof(name) - 1), &name) < 1) {
                name[0] = '\0';
            }
            if (!strcmp(name, _name)) {
                LOGD("using %s (name=%s), fd = %d", devname, name, fd);
                break;
            }
            close(fd);
            fd = -1;
        }
    }
    closedir(dir);

    if (fd < 0) {
        LOGE("Couldn't find or open '%s' driver (%s)", _name, strerror(errno));
    }
    return fd;
}

static int open_akm()
{
    if (sAkmFD <= 0) {
        sAkmFD = open(AKM_DEVICE_NAME, O_RDONLY);
        LOGD("%s, fd=%d", __PRETTY_FUNCTION__, sAkmFD);
        LOGE_IF(sAkmFD<0, "Couldn't open %s (%s)",
                AKM_DEVICE_NAME, strerror(errno));
        if (sAkmFD >= 0) {
            sActiveSensors = 0;
        }
    }
    return sAkmFD;
}

static void close_akm()
{
    if (sAkmFD > 0) {
        LOGD("%s, fd=%d", __PRETTY_FUNCTION__, sAkmFD);
        close(sAkmFD);
        sAkmFD = -1;
    }
}

static int open_accel() {
    if (sAccelModeFD <= 0) {
        sAccelModeFD = open(ACCEL_DEVICE_NAME, O_RDWR);
        LOGD("%s, fd=%d", __PRETTY_FUNCTION__, sAccelModeFD);
        LOGE_IF(sAccelModeFD < 0, "Couldn't open %s (%s)", ACCEL_DEVICE_NAME, strerror(errno));
    }
    return sAccelModeFD;
}

static void close_accel() {
    if (sAccelModeFD > 0) {
        LOGD("%s, fd=%d", __PRETTY_FUNCTION__, sAccelModeFD);
        close(sAccelModeFD);
        sAccelModeFD = -1;
    }
}

static char* get_new_settings(uint32_t sensors ,char *speed )
 {
 int mode ;

   if (sensors & SENSORS_ACCELERATION ) mode =  (sensors & SENSORS_ROTATION ) ? 3:1;
   else mode =  (sensors & SENSORS_ROTATION ) ? 2:4;
     
    sprintf(settings,"%d %s 4g",mode,speed);
  return settings;
/*
    int fd = open(ACCEL_DEBOUNCE, O_RDONLY);
    if (fd > 0) {
        int bytes = read(fd, settings, 63);
        if (bytes > 0) {
            settings[bytes] = 0;
            debounce = atoi(settings);
        }
        close(fd);
    }
    fd = open(ACCEL_SETTINGS, O_RDONLY);
    if (fd < 0) {
        return enable_irq;
    }
    int bytes = read(fd, settings, 63);
    if (bytes <= 0) {
        close(fd);
        return enable_irq;
    }
    if (bytes > 63) { // strange
        bytes = 63;
    }
    settings[bytes] = 0;
    close(fd);
    return settings;
*/
}

static void enable_disable(int fd, uint32_t sensors, uint32_t mask)
{

    short flags;
    LOGD("enable disable fd %x sensor %x, mask %x", fd, sensors, mask);
    char* accel_cmd = NULL;
    if (sActiveSensors > 0 && sensors == 0) { // all turn off
        accel_cmd = disable_irq;
    }

    if(sensors & SENSORS_ACCELERATION == 0) {
        sAccelSpeed = "s";
    }

    if ( sensors > 0) { // something turned on
        accel_cmd = get_new_settings(sensors,sAccelSpeed);
    }
    if (accel_cmd != NULL) { // got to be 1st as it could re-init driver
	    int accelFD = open_accel();
		if (accelFD > 0) {
			LOGD("write '%s' to sensor", accel_cmd);
		    int res = write(accelFD, accel_cmd, strlen(accel_cmd));
			LOGD("%d written (err - '%s-%s')", res, strerror(errno), strerror(res));
		    close_accel();
		}
    }
   if (fd < 0) return;

    if (sensors & SENSORS_ORIENTATION_RAW) {
        sensors |= SENSORS_ORIENTATION;
        mask |= SENSORS_ORIENTATION;
    } else if (mask & SENSORS_ORIENTATION_RAW) {
        mask |= SENSORS_ORIENTATION;
    }
    
    if (mask & SENSORS_ORIENTATION) {
        flags = (sensors & SENSORS_ORIENTATION) ? 1 : 0;
        if (ioctl(fd, ECS_IOCTL_APP_SET_MFLAG, &flags) < 0) {
            LOGE("ECS_IOCTL_APP_SET_MFLAG error (%s)", strerror(errno));
        }
    }
    if (mask & SENSORS_ACCELERATION) {
        flags = (sensors & SENSORS_ACCELERATION) ? 1 : 0;
        if (ioctl(fd, ECS_IOCTL_APP_SET_AFLAG, &flags) < 0) {
            LOGE("ECS_IOCTL_APP_SET_AFLAG error (%s)", strerror(errno));
        }
    }
    if (mask & SENSORS_TEMPERATURE) {
        flags = (sensors & SENSORS_TEMPERATURE) ? 1 : 0;
        if (ioctl(fd, ECS_IOCTL_APP_SET_TFLAG, &flags) < 0) {
            LOGE("ECS_IOCTL_APP_SET_TFLAG error (%s)", strerror(errno));
        }
    }
#ifdef ECS_IOCTL_APP_SET_MVFLAG
    if (mask & SENSORS_MAGNETIC_FIELD) {
        flags = (sensors & SENSORS_MAGNETIC_FIELD) ? 1 : 0;
        if (ioctl(fd, ECS_IOCTL_APP_SET_MVFLAG, &flags) < 0) {
            LOGE("ECS_IOCTL_APP_SET_MVFLAG error (%s)", strerror(errno));
        }
    }
#endif
}

static uint32_t read_sensors_state(int fd)
{
    if (fd < 0) return 0;
    short flags;
    uint32_t sensors = 0;
    // read the actual value of all sensors
    if (!ioctl(fd, ECS_IOCTL_APP_GET_MFLAG, &flags)) {
        if (flags)  sensors |= SENSORS_ORIENTATION;
        else        sensors &= ~SENSORS_ORIENTATION;
    }
    if (!ioctl(fd, ECS_IOCTL_APP_GET_AFLAG, &flags)) {
        if (flags)  sensors |= SENSORS_ACCELERATION;
        else        sensors &= ~SENSORS_ACCELERATION;
    }
    /* e42601@ MOT: we are not supporting it, but flag kept getting raised!
     * Disable until we really support SENSORS_TEMPERATURE
     * cvk011c :  AMK9873 has temperature sensor. we shoud support it  
     */
	if (!ioctl(fd, ECS_IOCTL_APP_GET_TFLAG, &flags)) {
        if (flags)  sensors |= SENSORS_TEMPERATURE;
        else        sensors &= ~SENSORS_TEMPERATURE;
    }
#ifdef ECS_IOCTL_APP_SET_MVFLAG
    if (!ioctl(fd, ECS_IOCTL_APP_GET_MVFLAG, &flags)) {
        if (flags)  sensors |= SENSORS_MAGNETIC_FIELD;
        else        sensors &= ~SENSORS_MAGNETIC_FIELD;
    }
#endif
    return sensors;
}

/*****************************************************************************/
static int trigger_rotation_event()
{
    char* accel_cmd ="9 s 4g";
    int accelFD = open_accel();
	if (accelFD > 0) {
	 int res = write(accelFD, accel_cmd, strlen(accel_cmd));
	 close_accel();
	}
    return 0;
}




static int sensors_control_open_data_source(struct sensors_control_device_t *dev, int sensor) {
int fd;
    fd= open_input(sensor == SENSORS_ROTATION ? "accelerometer" : "compass");
    if ( sensor == SENSORS_ROTATION ) trigger_rotation_event();
return (fd);
}


static int sensors_change_in_progress = 0;
static int sensors_control_activate(struct sensors_control_device_t *dev, 
            int handle, int enabled)
{
int fd =-1;
    sensors_change_in_progress = 5;
    uint32_t active = sActiveSensors;
    uint32_t sensor = 1 << (handle - SENSORS_HANDLE_BASE);
    uint32_t new_sensors = enabled ? (active | sensor) : (active & ~sensor);
    uint32_t changed = active ^ new_sensors;
    LOGE("sensors=%08x, changed %08x", sActiveSensors, changed);
    if (changed) {
    /* filter ROTATION SENSOR  */
       if( changed &= ~SENSORS_ROTATION )
        {
         fd = open_akm();
        if (fd < 0) return -1;
         }

        if (!active && new_sensors) {
            // force all sensors to be updated
            changed = SUPPORTED_SENSORS;
        }

        enable_disable(fd, new_sensors, changed);

        if( (active &(~SENSORS_ROTATION)) && (!(new_sensors & ~SENSORS_ROTATION)) ){
            // close the driver
            close_akm();
        }

        sActiveSensors = active = new_sensors;
        LOGE("sensors=%08x, real=%08x", sActiveSensors, read_sensors_state(fd));
    }
    sensors_change_in_progress = 0;
    if(new_sensors & SENSORS_ROTATION)  trigger_rotation_event();
    return 0;
}

static int sensors_control_delay(struct sensors_control_device_t *dev, int32_t ms)
{
    short flags;
    char* accel_cmd = NULL;
    //char *speed ="s";
    LOGE("sensor set delay %d  sensors %x", ms,sActiveSensors);
    if(  ms < 20 )   sAccelSpeed = "f";
    else  if ( ms < 60 ) sAccelSpeed = "m";
    else  sAccelSpeed ="s";

    if (sActiveSensors & SENSORS_ACCELERATION  ) { // something turned on
        accel_cmd = get_new_settings(sActiveSensors,sAccelSpeed);
    }
    if (accel_cmd != NULL) { // got to be 1st as it could re-init driver
	    int accelFD = open_accel();
		if (accelFD > 0) {
			LOGD("write '%s' to sensor", accel_cmd);
		    int res = write(accelFD, accel_cmd, strlen(accel_cmd));
			LOGD("%d written (err - '%s-%s')", res, strerror(errno), strerror(res));
		    close_accel();
		}
    }
    return 0;

/*
#ifdef ECS_IOCTL_APP_SET_DELAY
    if (sAkmFD <= 0) {
        return -1;
    }
    short delay = ms;
    if (!ioctl(sAkmFD, ECS_IOCTL_APP_SET_DELAY, &delay)) {
        return -errno;
    }
    return 0;
#else
    return -1;
#endif
*/
}


/*****************************************************************************/

struct hw_module_methods_t sensors_module_methods = {
    open: sensors_device_open
};

const struct sensors_module_t HAL_MODULE_INFO_SYM = {
    common: {
        tag: HARDWARE_MODULE_TAG,
        version_major: 1,
        version_minor: 0,
        id: SENSORS_HARDWARE_MODULE_ID,
        name : "AK8973 Compass module",
        author : "Asahi Kasei Corp.",
        methods: &sensors_module_methods,
    },
    get_sensors_list: sensors_get_sensors_list,
};

struct sensor_t sensors_descs[] = {
    {
      name : "AK8973 Compass",
      vendor : "Asahi Kasei Corp.",
      version : 1,
      handle : SENSORS_ORIENTATION_HANDLE,
      type : SENSOR_TYPE_ORIENTATION,
      maxRange : 1.0,
      resolution : 1,
      power : 20,
    },
    {
      name : "AK8973 Compass Raw",
      vendor : "Asahi Kasei Corp.",
      version : 1,
      handle : SENSORS_ORIENTATION_RAW_HANDLE,
      type : SENSOR_TYPE_ORIENTATION,
      maxRange : 1.0,
      resolution : 1,
      power : 20,
    },

    {
      name : "LIS331dlh",
      vendor : "stmicro",
      version : 1,
      handle : SENSORS_ACCELERATION_HANDLE,
      type : SENSOR_TYPE_ACCELEROMETER,
      maxRange : 1.0,
      resolution : 1,
      power : 20,
    },
    {
      name : "AK8973 Temperature",
      vendor : "Asahi Kasei Corp.",
      version : 1,
      handle : SENSORS_TEMPERATURE_HANDLE,
      type : SENSOR_TYPE_TEMPERATURE,
      maxRange : 1.0,
      resolution : 1,
      power : 20,
    },

    {
      name : "AK8973 Magnetic Field",
      vendor : "Asahi Kasei Corp.",
      version : 1,
      handle : SENSORS_MAGNETIC_FIELD_HANDLE,
      type : SENSOR_TYPE_MAGNETIC_FIELD,
      maxRange : 1.0,
      resolution : 1,
      power : 20,
    },
    {
      name : "LIS331dlh Rotation",
      vendor : "stmicro",
      version : 1,
      handle : SENSORS_ROTATION_HANDLE,
      type : SENSOR_TYPE_ROTATION,
      maxRange : 1.0,
      resolution : 1,
      power : 20,
    },
    0,
};

/*****************************************************************************/
struct sensors_control_context_t {
    struct sensors_control_device_t device;
};

struct sensors_data_context_t {
    struct sensors_data_device_t device;
};

static int sensors_get_sensors_list(struct sensors_module_t* module,
		struct sensor_t const** plist){
    *plist = sensors_descs;
    LOGD("sensors_get_sensors_list\n");
    return 6; // No need to return number of sensor list
}

static int sensors_device_open(const struct hw_module_t* module, const char* name,
        struct hw_device_t** device)
{
    int status = -EINVAL;

    LOGD("sensors_device_open: module %x, name %s\n", module, name ? name : "NULL");

    if (!strcmp(name, SENSORS_HARDWARE_CONTROL)) {
        struct sensors_control_context_t *dev;
        dev = (struct sensors_control_context_t*)malloc(sizeof(*dev));

        /* initialize our state here */
        memset(dev, 0, sizeof(*dev));

        /* initialize the procs */
        dev->device.common.tag = HARDWARE_DEVICE_TAG;
        dev->device.common.version = 0;
        dev->device.common.module = module;
        dev->device.common.close = sensors_control_device_close;
        dev->device.open_data_source = sensors_control_open_data_source;
        dev->device.activate = sensors_control_activate;
        dev->device.set_delay = sensors_control_delay;
        dev->device.wake = sensors_control_wake;

        *device = &dev->device.common;
        status = 0;
    }
    if (!strcmp(name, SENSORS_HARDWARE_DATA)) {
        struct sensors_data_context_t *dev;
        dev = (struct sensors_data_context_t*)malloc(sizeof(*dev));

        /* initialize our state here */
        memset(dev, 0, sizeof(*dev));

        /* initialize the procs */
        dev->device.common.tag = HARDWARE_DEVICE_TAG;
        dev->device.common.version = 0;
        dev->device.common.module = module;
        dev->device.common.close = sensors_data_device_close;
        
        dev->device.data_open = sensors_data_data_open;
        dev->device.data_close = sensors_data_data_close;
        dev->device.poll = sensors_data_poll;

        *device = &dev->device.common;
        status = 0;
    }
	
    return status;
}

static int sensors_control_device_close(struct hw_device_t *dev)
{
    struct sensors_control_context_t* ctx = (struct sensors_control_t*)dev;
    free(ctx);
    return 0;
}

static int sensors_data_device_close(struct hw_device_t *dev)
{
    struct sensors_data_context_t* ctx = (struct sensors_data_t*)dev;
    free(ctx);
    return 0;
}

static int sensors_control_wake(struct sensors_control_device_t *dev)
{
    return 0;
}

/*****************************************************************************/

#define MAX_NUM_SENSORS 9

static int sInputFD = -1;
static const int ID_O  = 0;
static const int ID_A  = 1;
static const int ID_T  = 2;
static const int ID_M  = 3;
static const int ID_OR = 7; // orientation raw
static const int ID_R  = 8; // rotation
static sensors_data_t sSensors[MAX_NUM_SENSORS];
static uint32_t sPendingSensors;

static int sensors_data_data_open(struct sensors_data_device_t *dev, int fd, int sensor)
{
    int i;

    if (sensor == SENSORS_ROTATION) {
        sAccelFD = dup(fd);
        LOGD("special care of rotation %d <- %d", sAccelFD, fd);
        return 0;
    }

    LMSInit();
    memset(&sSensors, 0, sizeof(sSensors));
    for (i=0 ; i<MAX_NUM_SENSORS ; i++) {
        // by default all sensors have high accuracy
        // (we do this because we don't get an update if the value doesn't
        // change).
        sSensors[i].vector.status = SENSOR_STATUS_ACCURACY_HIGH;
    }
    sPendingSensors = 0;
    sInputFD = dup(fd);
    LOGD("sensors_data_open: fd = %d (dupped from %d)", sInputFD, fd);
    return 0;
}

static int sensors_data_data_close(struct sensors_data_device_t *dev)
{
    close(sInputFD);
    close(sAccelFD);
    LOGD("sensors_data_close: fd = %d", sInputFD);
    sInputFD = -1;
    sAccelFD = -1;
    return 0;
}

static int pick_sensor(sensors_data_t* values)
{
    uint32_t mask = SENSORS_MASK;
    while(mask) {
        uint32_t i = 31 - __builtin_clz(mask);
        mask &= ~(1<<i);
        if (sPendingSensors & (1<<i)) {
            sPendingSensors &= ~(1<<i);
            *values = sSensors[i];
            values->sensor = (1<<i);
            LOGD_IF(0, "%d [%f, %f, %f]", (1<<i),
                    values->vector.x,
                    values->vector.y,
                    values->vector.z);
            return SENSORS_HANDLE_BASE + i;
        }
    }
    LOGE("No sensor to return!!! sPendingSensors=%08x", sPendingSensors);
    // we may end-up in a busy loop, slow things down, just in case.
    usleep(100000);
    return 0;
}

int orientation = -1;
int counter = 0;
int poll_timeout = SENSORS_TIMEOUT_MS;

static int sensors_data_poll(struct sensors_data_device_t *dev, sensors_data_t* data)
{
    struct input_event event;
    int nread;
    int64_t t;

    //LOGE("POLL: %x:%x:%x", sActiveSensors, sOpenSensors, SUPPORTED_SENSORS);

    int fd = sInputFD;
    if (fd <= 0) {
        LOGE("ouch: input file is not opened!");
        return -1;
    }
    // there are pending sensors, returns them now...
    if (sPendingSensors) {
        return pick_sensor(data);
    }

    uint32_t new_sensors = 0;
    struct pollfd fds[2];
    int fdsToPoll = 1;
    fds[0].fd = fd;
    fds[0].events = POLLIN;
    fds[0].revents = 0;
    fds[1].events = POLLIN;
    fds[1].revents = 0;
    if (sAccelFD > 0) { // can't get it on non system process!
        fds[1].fd = sAccelFD;
        fdsToPoll = 2;
    }

    // wait until we get a complete event for an enabled sensor
    // TRICKY: "sAccelFD < 0" is temporary fix to enable raw data for non-system process
    while ((sActiveSensors & SUPPORTED_SENSORS) || (sAccelModeFD < 0) || sensors_change_in_progress) {
        nread = 0;
        int err = -1;

        err = poll(fds, fdsToPoll, poll_timeout);
/*  Move debounce timer to HW .  set default to 500 ms 
        if ( orientation != -1) { // Have Pending  Rotation sensor , check debounce 
            struct timespec time;
            time.tv_sec = time.tv_nsec = 0;
            clock_gettime(CLOCK_MONOTONIC, &time);
            // generate an output value 
            t = time.tv_sec*1000LL+time.tv_nsec/1000000LL;

            if( (t - sSensors[ID_R].time/1000000LL) > debounce  )
             {
            *data = sSensors[ID_R];
            data->sensor = (1<<ID_R);
            poll_timeout = SENSORS_TIMEOUT_MS;
            orientation = -1;
            LOGE("orientation changed! t %lld , time %lld  sec %lld deb %d",
                                      t,sSensors[ID_R].time/1000000LL,(t - sSensors[ID_R].time/1000000LL),debounce);
            return SENSORS_ROTATION_HANDLE;
            }
          }
*/
        /* TODO: Do we also break out of this loop if err is -1? */

        if ((sActiveSensors & SENSORS_ORIENTATION) && (err == 0)) {
            /* We do some special processing if the orientation sensor is
             * activated. In particular the yaw value is filtered with a
             * LMS filter. Since the kernel only sends an event when the
             * value changes, we need to wake up at regular intervals to
             * generate an output value (the output value may not be
             * constant when the input value is constant)
             */
                struct timespec time;
                time.tv_sec = time.tv_nsec = 0;
                clock_gettime(CLOCK_MONOTONIC, &time);

                /* generate an output value */
                t = time.tv_sec*1000000000LL+time.tv_nsec;
                new_sensors |= SENSORS_ORIENTATION;
                sSensors[ID_O].orientation.azimuth =
                        LMSFilter(t, sSensors[ID_OR].orientation.azimuth);

                /* generate a fake sensors event */
                event.type = EV_SYN;
                event.time.tv_sec = time.tv_sec;
                event.time.tv_usec = time.tv_nsec/1000;
                nread = sizeof(event);
        }
        if (nread == 0) {
            /* read the next event */
            int fdToRead = -1;
            if (fdsToPoll == 2) { // system process
                if (fds[1].revents > 0) {
                    fdToRead = fds[1].fd;
                } else if (fds[0].revents > 0) {
                    fdToRead = fds[0].fd;
                } else {
                    // tricky: don't want to read in such case! could lock on only of 2 possible inputs!
                    fdToRead = -1;
                }
            } else { // will happen in non system process
                fdToRead = fds[0].fd;
            }
            if (fdToRead > 0) {
                nread = read(fdToRead, &event, sizeof(event));
            }
        }

        if (nread == sizeof(event)) {
            uint32_t v;
            if (event.type == EV_ABS) {
                //LOGD("type: %d code: %d value: %-5d time: %ds",
                //        event.type, event.code, event.value,
                //      (int)event.time.tv_sec);
                switch (event.code) {

                    case EVENT_TYPE_ACCEL_X:
                        new_sensors |= SENSORS_ACCELERATION;
                        sSensors[ID_A].acceleration.x = event.value * CONVERT_A_X;
                        break;
                    case EVENT_TYPE_ACCEL_Y:
                        new_sensors |= SENSORS_ACCELERATION;
                        sSensors[ID_A].acceleration.y = event.value * CONVERT_A_Y;
                        break;
                    case EVENT_TYPE_ACCEL_Z:
                        new_sensors |= SENSORS_ACCELERATION;
                        sSensors[ID_A].acceleration.z = event.value * CONVERT_A_Z;
                        break;

                    case EVENT_TYPE_MAGV_X:
                        new_sensors |= SENSORS_MAGNETIC_FIELD;
                        sSensors[ID_M].magnetic.x = -event.value * CONVERT_M;
                        break;
                    case EVENT_TYPE_MAGV_Y:
                        new_sensors |= SENSORS_MAGNETIC_FIELD;
                        sSensors[ID_M].magnetic.y = -event.value * CONVERT_M;
                        break;
                    case EVENT_TYPE_MAGV_Z:
                        new_sensors |= SENSORS_MAGNETIC_FIELD;
                        sSensors[ID_M].magnetic.z = event.value * CONVERT_M;
                        break;

                    case EVENT_TYPE_YAW:
                        new_sensors |= SENSORS_ORIENTATION | SENSORS_ORIENTATION_RAW;
// Do not need LMS  filter here.  The same filter is in SensorManager.java 
//                        t = event.time.tv_sec*1000000000LL 
//                                event.time.tv_usec*1000;                  
//                       sSensors[ID_O].orientation.azimuth = 
//                            (sActiveSensors & SENSORS_ORIENTATION) ?
//                                   LMSFilter(t, event.value) : event.value;

                        sSensors[ID_O].orientation.azimuth = event.value;
                        sSensors[ID_OR].orientation.azimuth = event.value;
                        break;
                    case EVENT_TYPE_PITCH:
                        new_sensors |= SENSORS_ORIENTATION | SENSORS_ORIENTATION_RAW;
                        sSensors[ID_O].orientation.pitch = event.value;
                        sSensors[ID_OR].orientation.pitch = event.value;
                        break;
                    case EVENT_TYPE_ROLL:
                        new_sensors |= SENSORS_ORIENTATION | SENSORS_ORIENTATION_RAW;
                        sSensors[ID_O].orientation.roll = event.value;
                        sSensors[ID_OR].orientation.roll = event.value;
                        break;

                    case EVENT_TYPE_TEMPERATURE:
                        new_sensors |= SENSORS_TEMPERATURE;
                        sSensors[ID_T].temperature = event.value;
                        break;

                    case EVENT_TYPE_STEP_COUNT:
                        // step count (only reported in MODE_FFD)
                        // we do nothing with it for now.
                        break;
                    case EVENT_TYPE_ACCEL_STATUS:
                        // accuracy of the calibration (never returned!)
                        //LOGD("G-Sensor status %d", event.value);
                        break;
                    case EVENT_TYPE_ORIENT_STATUS:
                        // accuracy of the calibration
                        v = (uint32_t)(event.value & SENSOR_STATE_MASK);
                        LOGD_IF(sSensors[ID_O].orientation.status != (uint8_t)v,
                                "M-Sensor status %d", v);
                        sSensors[ID_O].orientation.status = (uint8_t)v;
                        sSensors[ID_OR].orientation.status = (uint8_t)v;
                        break;
                    case EVENT_TYPE_ROTATION_IRQ: {
                        // no need to wait sync here
                        int rotation = -1;
                        switch (event.value) {
                            case 0x100: rotation = 1; break; // 90, consts from java's Surface
                            case 0x200: rotation = 3; break; // 270
                            case 0x400: rotation = 0; break; // 0
                            case 0x800: rotation = 2; break; // 180
                        }
                        if (rotation >= 0) {
                            int64_t t = event.time.tv_sec*1000000000LL + event.time.tv_usec*1000;
                            sSensors[ID_R].rotation = rotation;
                            sSensors[ID_R].time = t;

                             *data = sSensors[ID_R];
                             data->sensor = (1<<ID_R);
                             LOGD("orientation changed to  %d",rotation);
                             return SENSORS_ROTATION_HANDLE;

                        }
                        }
                        break;
                }
            } else if (event.type == EV_SYN) {
                if (new_sensors) {
                    sPendingSensors = new_sensors;
                    int64_t t = event.time.tv_sec*1000000000LL +
                            event.time.tv_usec*1000;
                    while (new_sensors) {
                        uint32_t i = 31 - __builtin_clz(new_sensors);
                        new_sensors &= ~(1<<i);
                        sSensors[i].time = t;
                    }
                    return pick_sensor(data);
                }
            }
        }
        if (sensors_change_in_progress > 0) {
            LOGE("sensor state didn't change yet");
            sensors_change_in_progress--;
            usleep(20000); // only 20 ms
        }
    }
    LOGE("exit loop: conditions are %d, %d", (sActiveSensors & SUPPORTED_SENSORS), (sAccelModeFD < 0));
    return -1;
}

