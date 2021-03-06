/*
 * This file is part of Cleanflight.
 *
 * Cleanflight is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Cleanflight is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Cleanflight.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "build_config.h"

#include "platform.h"

#include "common/maths.h"
#include "common/axis.h"

#include "drivers/system.h"
#include "drivers/accgyro.h"
#include "drivers/serial.h"
#include "drivers/bus_i2c.h"
#include "drivers/gpio.h"
#include "drivers/timer.h"
#include "drivers/pwm_rx.h"

#include "flight/flight.h"
#include "flight/mixer.h"
#include "flight/failsafe.h"
#include "flight/navigation.h"
#include "rx/rx.h"
#include "rx/msp.h"
#include "io/escservo.h"
#include "io/rc_controls.h"
#include "io/gps.h"
#include "io/gimbal.h"
#include "io/serial.h"
#include "telemetry/telemetry.h"
#include "sensors/boardalignment.h"
#include "sensors/sensors.h"
#include "sensors/battery.h"
#include "sensors/acceleration.h"
#include "sensors/barometer.h"
#include "sensors/compass.h"
#include "sensors/gyro.h"

#include "config/runtime_config.h"
#include "config/config.h"
#include "config/config_profile.h"
#include "config/config_master.h"

#include "version.h"

#include "serial_msp.h"

static serialPort_t *mspSerialPort;

extern uint16_t cycleTime; // FIXME dependency on mw.c
extern uint16_t rssi; // FIXME dependency on mw.c
extern int16_t debug[4]; // FIXME dependency on mw.c

// Multiwii Serial Protocol 0
#define MSP_VERSION              0
#define CAP_PLATFORM_32BIT          ((uint32_t)1 << 31)
#define CAP_DYNBALANCE              ((uint32_t)1 << 2)
#define CAP_FLAPS                   ((uint32_t)1 << 3)
#define CAP_CHANNEL_FORWARDING      ((uint32_t)1 << 4)
#define CAP_ACTIVATE_AUX1_TO_AUX8   ((uint32_t)1 << 5)

#define MSP_IDENT                100    //out message         multitype + multiwii version + protocol version + capability variable
#define MSP_STATUS               101    //out message         cycletime & errors_count & sensor present & box activation & current setting number
#define MSP_RAW_IMU              102    //out message         9 DOF
#define MSP_SERVO                103    //out message         8 servos
#define MSP_MOTOR                104    //out message         8 motors
#define MSP_RC                   105    //out message         8 rc chan and more
#define MSP_RAW_GPS              106    //out message         fix, numsat, lat, lon, alt, speed, ground course
#define MSP_COMP_GPS             107    //out message         distance home, direction home
#define MSP_ATTITUDE             108    //out message         2 angles 1 heading
#define MSP_ALTITUDE             109    //out message         altitude, variometer
#define MSP_ANALOG               110    //out message         vbat, powermetersum, rssi if available on RX
#define MSP_RC_TUNING            111    //out message         rc rate, rc expo, rollpitch rate, yaw rate, dyn throttle PID
#define MSP_PID                  112    //out message         P I D coeff (9 are used currently)
#define MSP_BOX                  113    //out message         BOX setup (number is dependant of your setup)
#define MSP_MISC                 114    //out message         powermeter trig
#define MSP_MOTOR_PINS           115    //out message         which pins are in use for motors & servos, for GUI
#define MSP_BOXNAMES             116    //out message         the aux switch names
#define MSP_PIDNAMES             117    //out message         the PID names
#define MSP_WP                   118    //out message         get a WP, WP# is in the payload, returns (WP#, lat, lon, alt, flags) WP#0-home, WP#16-poshold
#define MSP_BOXIDS               119    //out message         get the permanent IDs associated to BOXes
#define MSP_SERVO_CONF           120    //out message         Servo settings
#define MSP_NAV_STATUS           121    //out message         Returns navigation status
#define MSP_NAV_CONFIG           122    //out message         Returns navigation parameters
#define MSP_CHANNEL_FORWARDING   123    //out message         Returns channel forwarding settings

#define MSP_SET_RAW_RC           200    //in message          8 rc chan
#define MSP_SET_RAW_GPS          201    //in message          fix, numsat, lat, lon, alt, speed
#define MSP_SET_PID              202    //in message          P I D coeff (9 are used currently)
#define MSP_SET_BOX              203    //in message          BOX setup (number is dependant of your setup)
#define MSP_SET_RC_TUNING        204    //in message          rc rate, rc expo, rollpitch rate, yaw rate, dyn throttle PID
#define MSP_ACC_CALIBRATION      205    //in message          no param
#define MSP_MAG_CALIBRATION      206    //in message          no param
#define MSP_SET_MISC             207    //in message          powermeter trig + 8 free for future use
#define MSP_RESET_CONF           208    //in message          no param
#define MSP_SET_WP               209    //in message          sets a given WP (WP#,lat, lon, alt, flags)
#define MSP_SELECT_SETTING       210    //in message          Select Setting Number (0-2)
#define MSP_SET_HEAD             211    //in message          define a new heading hold direction
#define MSP_SET_SERVO_CONF       212    //in message          Servo settings
#define MSP_SET_CHANNEL_FORWARDING 213  //in message          Channel forwarding settings
#define MSP_SET_MOTOR            214    //in message          PropBalance function
#define MSP_SET_NAV_CONFIG       215    //in message          Sets nav config parameters - write to the eeprom

// #define MSP_BIND                 240    //in message          no param

#define MSP_EEPROM_WRITE         250    //in message          no param

#define MSP_DEBUGMSG             253    //out message         debug string buffer
#define MSP_DEBUG                254    //out message         debug1,debug2,debug3,debug4

// Additional commands that are not compatible with MultiWii
#define MSP_UID                  160    //out message         Unique device ID
#define MSP_ACC_TRIM             240    //out message         get acc angle trim values
#define MSP_SET_ACC_TRIM         239    //in message          set acc angle trim values
#define MSP_GPSSVINFO            164    //out message         get Signal Strength (only U-Blox)

#define INBUF_SIZE 64

#define ACTIVATE_MASK 0xFFF // see

typedef struct box_e {
    const uint8_t boxId;         // see boxId_e
    const char *boxName;            // GUI-readable box name
    const uint8_t permanentId;      //
} box_t;

static const box_t const boxes[] = {
    { BOXARM, "ARM;", 0 },
    { BOXANGLE, "ANGLE;", 1 },
    { BOXHORIZON, "HORIZON;", 2 },
    { BOXBARO, "BARO;", 3 },
    //{ BOXVARIO, "VARIO;", 4 },
    { BOXMAG, "MAG;", 5 },
    { BOXHEADFREE, "HEADFREE;", 6 },
    { BOXHEADADJ, "HEADADJ;", 7 },
    { BOXCAMSTAB, "CAMSTAB;", 8 },
    { BOXCAMTRIG, "CAMTRIG;", 9 },
    { BOXGPSHOME, "GPS HOME;", 10 },
    { BOXGPSHOLD, "GPS HOLD;", 11 },
    { BOXPASSTHRU, "PASSTHRU;", 12 },
    { BOXBEEPERON, "BEEPER;", 13 },
    { BOXLEDMAX, "LEDMAX;", 14 },
    { BOXLEDLOW, "LEDLOW;", 15 },
    { BOXLLIGHTS, "LLIGHTS;", 16 },
    { BOXCALIB, "CALIB;", 17 },
    { BOXGOV, "GOVERNOR;", 18 },
    { BOXOSD, "OSD SW;", 19 },
    { BOXTELEMETRY, "TELEMETRY;", 20 },
    { BOXAUTOTUNE, "AUTOTUNE;", 21 },
    { CHECKBOX_ITEM_COUNT, NULL, 0xFF }
};

// this is calculated at startup based on enabled features.
static uint8_t activeBoxIds[CHECKBOX_ITEM_COUNT];
// this is the number of filled indexes in above array
static uint8_t activeBoxIdCount = 0;
// from mixer.c
extern int16_t motor_disarmed[MAX_SUPPORTED_MOTORS];

static const char pidnames[] =
    "ROLL;"
    "PITCH;"
    "YAW;"
    "ALT;"
    "Pos;"
    "PosR;"
    "NavR;"
    "LEVEL;"
    "MAG;"
    "VEL;";

typedef enum {
    IDLE,
    HEADER_START,
    HEADER_M,
    HEADER_ARROW,
    HEADER_SIZE,
    HEADER_CMD,
} mspState_e;

typedef enum {
    UNUSED_PORT = 0,
    FOR_GENERAL_MSP,
    FOR_TELEMETRY
} mspPortUsage_e;

typedef struct mspPort_s {
    serialPort_t *port;
    uint8_t offset;
    uint8_t dataSize;
    uint8_t checksum;
    uint8_t indRX;
    uint8_t inBuf[INBUF_SIZE];
    mspState_e c_state;
    uint8_t cmdMSP;
    mspPortUsage_e mspPortUsage;
} mspPort_t;

static mspPort_t mspPorts[MAX_MSP_PORT_COUNT];

static mspPort_t *currentPort;

void serialize32(uint32_t a)
{
    static uint8_t t;
    t = a;
    serialWrite(mspSerialPort, t);
    currentPort->checksum ^= t;
    t = a >> 8;
    serialWrite(mspSerialPort, t);
    currentPort->checksum ^= t;
    t = a >> 16;
    serialWrite(mspSerialPort, t);
    currentPort->checksum ^= t;
    t = a >> 24;
    serialWrite(mspSerialPort, t);
    currentPort->checksum ^= t;
}

void serialize16(int16_t a)
{
    static uint8_t t;
    t = a;
    serialWrite(mspSerialPort, t);
    currentPort->checksum ^= t;
    t = a >> 8 & 0xff;
    serialWrite(mspSerialPort, t);
    currentPort->checksum ^= t;
}

void serialize8(uint8_t a)
{
    serialWrite(mspSerialPort, a);
    currentPort->checksum ^= a;
}

uint8_t read8(void)
{
    return currentPort->inBuf[currentPort->indRX++] & 0xff;
}

uint16_t read16(void)
{
    uint16_t t = read8();
    t += (uint16_t)read8() << 8;
    return t;
}

uint32_t read32(void)
{
    uint32_t t = read16();
    t += (uint32_t)read16() << 16;
    return t;
}

void headSerialResponse(uint8_t err, uint8_t s)
{
    serialize8('$');
    serialize8('M');
    serialize8(err ? '!' : '>');
    currentPort->checksum = 0;               // start calculating a new checksum
    serialize8(s);
    serialize8(currentPort->cmdMSP);
}

void headSerialReply(uint8_t s)
{
    headSerialResponse(0, s);
}

void headSerialError(uint8_t s)
{
    headSerialResponse(1, s);
}

void tailSerialReply(void)
{
    serialize8(currentPort->checksum);
}

void s_struct(uint8_t *cb, uint8_t siz)
{
    headSerialReply(siz);
    while (siz--)
        serialize8(*cb++);
}

void serializeNames(const char *s)
{
    const char *c;
    for (c = s; *c; c++)
        serialize8(*c);
}

const box_t *findBoxById(uint8_t boxId)
{
    uint8_t boxIndex;
    const box_t *candidate;
    for (boxIndex = 0; boxIndex < sizeof(boxes) / sizeof(box_t); boxIndex++) {
        candidate = &boxes[boxIndex];
        if (candidate->boxId == boxId) {
            return candidate;
        }
    }
    return NULL;
}

void serializeBoxNamesReply(void)
{
    int i, id, j, flag = 1, count = 0, len;
    const box_t *box;

reset:
    // in first run of the loop, we grab total size of junk to be sent
    // then come back and actually send it
    for (i = 0; i < activeBoxIdCount; i++) {
        id = activeBoxIds[i];

        box = findBoxById(id);
        if (!box) {
            continue;
        }

        len = strlen(box->boxName);
        if (flag) {
            count += len;
        } else {
            for (j = 0; j < len; j++)
                serialize8(box->boxName[j]);
        }
    }

    if (flag) {
        headSerialReply(count);
        flag = 0;
        goto reset;
    }
}

static void resetMspPort(mspPort_t *mspPortToReset, serialPort_t *serialPort, mspPortUsage_e usage)
{
    memset(mspPortToReset, 0, sizeof(mspPort_t));

    mspPortToReset->port = serialPort;
    mspPortToReset->mspPortUsage = usage;
}

// This rate is chosen since softserial supports it.
#define MSP_FALLBACK_BAUDRATE 19200

static void openAllMSPSerialPorts(serialConfig_t *serialConfig)
{
    serialPort_t *port;

    uint8_t portIndex = 0;
    do {

        uint32_t baudRate = serialConfig->msp_baudrate;

        bool triedFallbackRate = false;
        do {

            port = openSerialPort(FUNCTION_MSP, NULL, baudRate, MODE_RXTX, SERIAL_NOT_INVERTED);
            if (!port) {
                if (triedFallbackRate) {
                    break;
                }

                baudRate = MSP_FALLBACK_BAUDRATE;
                triedFallbackRate = true;
            }
        } while (!port);

        if (port && portIndex < MAX_MSP_PORT_COUNT) {
            mspPort_t *newMspPort = &mspPorts[portIndex++];

            resetMspPort(newMspPort, port, FOR_GENERAL_MSP);
        }

    } while (port);

    // XXX this function might help with adding support for MSP on more than one port, if not delete it.
    const serialPortFunctionList_t *serialPortFunctionList = getSerialPortFunctionList();
    UNUSED(serialPortFunctionList);
}

void mspInit(serialConfig_t *serialConfig)
{
    // calculate used boxes based on features and fill availableBoxes[] array
    memset(activeBoxIds, 0xFF, sizeof(activeBoxIds));

    activeBoxIdCount = 0;
    activeBoxIds[activeBoxIdCount++] = BOXARM;

    if (sensors(SENSOR_ACC)) {
        activeBoxIds[activeBoxIdCount++] = BOXANGLE;
        activeBoxIds[activeBoxIdCount++] = BOXHORIZON;
    }

    if (sensors(SENSOR_BARO)) {
        activeBoxIds[activeBoxIdCount++] = BOXBARO;
    }

    if (sensors(SENSOR_ACC) || sensors(SENSOR_MAG)) {
        activeBoxIds[activeBoxIdCount++] = BOXMAG;
        activeBoxIds[activeBoxIdCount++] = BOXHEADFREE;
        activeBoxIds[activeBoxIdCount++] = BOXHEADADJ;
    }

    if (feature(FEATURE_SERVO_TILT))
        activeBoxIds[activeBoxIdCount++] = BOXCAMSTAB;

#ifdef GPS
    if (feature(FEATURE_GPS)) {
        activeBoxIds[activeBoxIdCount++] = BOXGPSHOME;
        activeBoxIds[activeBoxIdCount++] = BOXGPSHOLD;
    }
#endif

    if (masterConfig.mixerConfiguration == MULTITYPE_FLYING_WING || masterConfig.mixerConfiguration == MULTITYPE_AIRPLANE)
        activeBoxIds[activeBoxIdCount++] = BOXPASSTHRU;

    activeBoxIds[activeBoxIdCount++] = BOXBEEPERON;

    if (feature(FEATURE_INFLIGHT_ACC_CAL))
        activeBoxIds[activeBoxIdCount++] = BOXCALIB;

    activeBoxIds[activeBoxIdCount++] = BOXOSD;

    if (feature(FEATURE_TELEMETRY && masterConfig.telemetryConfig.telemetry_switch))
        activeBoxIds[activeBoxIdCount++] = BOXTELEMETRY;

#ifdef AUTOTUNE
    activeBoxIds[activeBoxIdCount++] = BOXAUTOTUNE;
#endif

    memset(mspPorts, 0x00, sizeof(mspPorts));

    openAllMSPSerialPorts(serialConfig);
}

#define IS_ENABLED(mask) (mask == 0 ? 0 : 1)

static bool processOutCommand(uint8_t cmdMSP)
{
    uint32_t i, tmp, junk;


#ifdef GPS
    uint8_t wp_no;
    int32_t lat = 0, lon = 0;
#endif

    switch (cmdMSP) {
    case MSP_IDENT:
        headSerialReply(7);
        serialize8(MW_VERSION);
        serialize8(masterConfig.mixerConfiguration); // type of multicopter
        serialize8(MSP_VERSION);            // MultiWii Serial Protocol Version
        serialize32(CAP_PLATFORM_32BIT | CAP_DYNBALANCE | (masterConfig.airplaneConfig.flaps_speed ? CAP_FLAPS : 0) | CAP_CHANNEL_FORWARDING | CAP_ACTIVATE_AUX1_TO_AUX8); // "capability"
        break;
    case MSP_STATUS:
        headSerialReply(11);
        serialize16(cycleTime);
        serialize16(i2cGetErrorCounter());
        serialize16(sensors(SENSOR_ACC) | sensors(SENSOR_BARO) << 1 | sensors(SENSOR_MAG) << 2 | sensors(SENSOR_GPS) << 3 | sensors(SENSOR_SONAR) << 4);
        // OK, so you waste all the fucking time to have BOXNAMES and BOXINDEXES etc, and then you go ahead and serialize enabled shit simply by stuffing all
        // the bits in order, instead of setting the enabled bits based on BOXINDEX. WHERE IS THE FUCKING LOGIC IN THIS, FUCKWADS.
        // Serialize the boxes in the order we delivered them, until multiwii retards fix their shit
        junk = 0;
        tmp = IS_ENABLED(FLIGHT_MODE(ANGLE_MODE)) << BOXANGLE |
            IS_ENABLED(FLIGHT_MODE(HORIZON_MODE)) << BOXHORIZON |
            IS_ENABLED(FLIGHT_MODE(BARO_MODE)) << BOXBARO |
            IS_ENABLED(FLIGHT_MODE(MAG_MODE)) << BOXMAG |
            IS_ENABLED(FLIGHT_MODE(HEADFREE_MODE)) << BOXHEADFREE |
            rcOptions[BOXHEADADJ] << BOXHEADADJ |
            rcOptions[BOXCAMSTAB] << BOXCAMSTAB |
            rcOptions[BOXCAMTRIG] << BOXCAMTRIG |
            IS_ENABLED(FLIGHT_MODE(GPS_HOME_MODE)) << BOXGPSHOME |
            IS_ENABLED(FLIGHT_MODE(GPS_HOLD_MODE)) << BOXGPSHOLD |
            IS_ENABLED(FLIGHT_MODE(PASSTHRU_MODE)) << BOXPASSTHRU |
            rcOptions[BOXBEEPERON] << BOXBEEPERON |
            rcOptions[BOXLEDMAX] << BOXLEDMAX |
            rcOptions[BOXLLIGHTS] << BOXLLIGHTS |
            rcOptions[BOXCALIB] << BOXCALIB |
            rcOptions[BOXGOV] << BOXGOV |
            rcOptions[BOXOSD] << BOXOSD |
            rcOptions[BOXTELEMETRY] << BOXTELEMETRY |
            rcOptions[BOXAUTOTUNE] << BOXAUTOTUNE |
            IS_ENABLED(ARMING_FLAG(ARMED)) << BOXARM;
        for (i = 0; i < activeBoxIdCount; i++) {
            int flag = (tmp & (1 << activeBoxIds[i]));
            if (flag)
                junk |= 1 << i;
        }
        serialize32(junk);
        serialize8(masterConfig.current_profile_index);
        break;
    case MSP_RAW_IMU:
        headSerialReply(18);
        // Retarded hack until multiwiidorks start using real units for sensor data
        if (acc_1G > 1024) {
            for (i = 0; i < 3; i++)
                serialize16(accSmooth[i] / 8);
        } else {
            for (i = 0; i < 3; i++)
                serialize16(accSmooth[i]);
        }
        for (i = 0; i < 3; i++)
            serialize16(gyroData[i]);
        for (i = 0; i < 3; i++)
            serialize16(magADC[i]);
        break;
    case MSP_SERVO:
        s_struct((uint8_t *)&servo, 16);
        break;
    case MSP_SERVO_CONF:
        headSerialReply(56);
        for (i = 0; i < MAX_SUPPORTED_SERVOS; i++) {
            serialize16(currentProfile->servoConf[i].min);
            serialize16(currentProfile->servoConf[i].max);
            serialize16(currentProfile->servoConf[i].middle);
            serialize8(currentProfile->servoConf[i].rate);
        }
        break;
    case MSP_CHANNEL_FORWARDING:
        headSerialReply(8);
        for (i = 0; i < MAX_SUPPORTED_SERVOS; i++) {
            serialize8(currentProfile->servoConf[i].forwardFromChannel);
        }
        break;
    case MSP_MOTOR:
        s_struct((uint8_t *)motor, 16);
        break;
    case MSP_RC:
        headSerialReply(2 * rxRuntimeConfig.channelCount);
        for (i = 0; i < rxRuntimeConfig.channelCount; i++)
            serialize16(rcData[i]);
        break;
    case MSP_ATTITUDE:
        headSerialReply(6);
        for (i = 0; i < 2; i++)
            serialize16(inclination.raw[i]);
        serialize16(heading);
        break;
    case MSP_ALTITUDE:
        headSerialReply(6);
        serialize32(EstAlt);
        serialize16(vario);
        break;
    case MSP_ANALOG:
        headSerialReply(7);
        serialize8((uint8_t)constrain(vbat, 0, 255));
        serialize16((uint16_t)constrain(mAhDrawn, 0, 0xFFFF)); // milliamphours drawn from battery
        serialize16(rssi);
        if(masterConfig.batteryConfig.multiwiiCurrentMeterOutput) {
            serialize16((uint16_t)constrain((abs(amperage) * 10), 0, 0xFFFF)); // send amperage in 0.001 A steps
        } else
            serialize16((uint16_t)constrain(abs(amperage), 0, 0xFFFF)); // send amperage in 0.01 A steps
        break;
    case MSP_RC_TUNING:
        headSerialReply(7);
        serialize8(currentProfile->controlRateConfig.rcRate8);
        serialize8(currentProfile->controlRateConfig.rcExpo8);
        serialize8(currentProfile->controlRateConfig.rollPitchRate);
        serialize8(currentProfile->controlRateConfig.yawRate);
        serialize8(currentProfile->dynThrPID);
        serialize8(currentProfile->controlRateConfig.thrMid8);
        serialize8(currentProfile->controlRateConfig.thrExpo8);
        break;
    case MSP_PID:
        headSerialReply(3 * PID_ITEM_COUNT);
        if (currentProfile->pidController == 2) { // convert float stuff into uint8_t to keep backwards compatability with all 8-bit shit with new pid
            for (i = 0; i < 3; i++) {
                serialize8(constrain(lrintf(currentProfile->pidProfile.P_f[i] * 10.0f), 0, 250));
                serialize8(constrain(lrintf(currentProfile->pidProfile.I_f[i] * 100.0f), 0, 250));
                serialize8(constrain(lrintf(currentProfile->pidProfile.D_f[i] * 1000.0f), 0, 100));
            }
            for (i = 3; i < PID_ITEM_COUNT; i++) {
                if (i == PIDLEVEL) {
                    serialize8(constrain(lrintf(currentProfile->pidProfile.A_level * 10.0f), 0, 250));
                    serialize8(constrain(lrintf(currentProfile->pidProfile.H_level * 10.0f), 0, 250));
                    serialize8(0);
                } else {
                    serialize8(currentProfile->pidProfile.P8[i]);
                    serialize8(currentProfile->pidProfile.I8[i]);
                    serialize8(currentProfile->pidProfile.D8[i]);
                }
            }
        } else {
            for (i = 0; i < PID_ITEM_COUNT; i++) {
                serialize8(currentProfile->pidProfile.P8[i]);
                serialize8(currentProfile->pidProfile.I8[i]);
                serialize8(currentProfile->pidProfile.D8[i]);
            }
        }
        break;
    case MSP_PIDNAMES:
        headSerialReply(sizeof(pidnames) - 1);
        serializeNames(pidnames);
        break;
    case MSP_BOX:
        headSerialReply(4 * activeBoxIdCount);
        for (i = 0; i < activeBoxIdCount; i++)
            serialize16(currentProfile->activate[activeBoxIds[i]] & ACTIVATE_MASK);
        for (i = 0; i < activeBoxIdCount; i++)
            serialize16((currentProfile->activate[activeBoxIds[i]] >> 16) & ACTIVATE_MASK);
        break;
    case MSP_BOXNAMES:
        // headSerialReply(sizeof(boxnames) - 1);
        serializeBoxNamesReply();
        break;
    case MSP_BOXIDS:
        headSerialReply(activeBoxIdCount);
        for (i = 0; i < activeBoxIdCount; i++) {
            const box_t *box = findBoxById(activeBoxIds[i]);
            if (!box) {
                continue;
            }
            serialize8(box->permanentId);
        }
        break;
    case MSP_MISC:
        headSerialReply(2 * 6 + 4 + 2 + 4);
        serialize16(0); // intPowerTrigger1 (aka useless trash)
        serialize16(masterConfig.escAndServoConfig.minthrottle);
        serialize16(masterConfig.escAndServoConfig.maxthrottle);
        serialize16(masterConfig.escAndServoConfig.mincommand);
        serialize16(currentProfile->failsafeConfig.failsafe_throttle);
        serialize16(0); // plog useless shit
        serialize32(0); // plog useless shit
        serialize16(currentProfile->mag_declination / 10); // TODO check this shit
        serialize8(masterConfig.batteryConfig.vbatscale);
        serialize8(masterConfig.batteryConfig.vbatmincellvoltage);
        serialize8(masterConfig.batteryConfig.vbatmaxcellvoltage);
        serialize8(0);
        break;
    case MSP_MOTOR_PINS:
        headSerialReply(8);
        for (i = 0; i < 8; i++)
            serialize8(i + 1);
        break;
#ifdef GPS
    case MSP_RAW_GPS:
        headSerialReply(16);
        serialize8(STATE(GPS_FIX));
        serialize8(GPS_numSat);
        serialize32(GPS_coord[LAT]);
        serialize32(GPS_coord[LON]);
        serialize16(GPS_altitude);
        serialize16(GPS_speed);
        serialize16(GPS_ground_course);
        break;
    case MSP_COMP_GPS:
        headSerialReply(5);
        serialize16(GPS_distanceToHome);
        serialize16(GPS_directionToHome);
        serialize8(GPS_update & 1);
        break;
    case MSP_WP:
        wp_no = read8();    // get the wp number
        headSerialReply(18);
        if (wp_no == 0) {
            lat = GPS_home[LAT];
            lon = GPS_home[LON];
        } else if (wp_no == 16) {
            lat = GPS_hold[LAT];
            lon = GPS_hold[LON];
        }
        serialize8(wp_no);
        serialize32(lat);
        serialize32(lon);
        serialize32(AltHold);           // altitude (cm) will come here -- temporary implementation to test feature with apps
        serialize16(0);                 // heading  will come here (deg)
        serialize16(0);                 // time to stay (ms) will come here
        serialize8(0);                  // nav flag will come here
        break;
    case MSP_GPSSVINFO:
        headSerialReply(1 + (GPS_numCh * 4));
        serialize8(GPS_numCh);
           for (i = 0; i < GPS_numCh; i++){
               serialize8(GPS_svinfo_chn[i]);
               serialize8(GPS_svinfo_svid[i]);
               serialize8(GPS_svinfo_quality[i]);
               serialize8(GPS_svinfo_cno[i]);
            }
        break;
#endif
    case MSP_DEBUG:
        headSerialReply(8);
        // make use of this crap, output some useful QA statistics
        //debug[3] = ((hse_value / 1000000) * 1000) + (SystemCoreClock / 1000000);         // XX0YY [crystal clock : core clock]
        for (i = 0; i < 4; i++)
            serialize16(debug[i]);      // 4 variables are here for general monitoring purpose
        break;

    // Additional commands that are not compatible with MultiWii
    case MSP_ACC_TRIM:
        headSerialReply(4);
        serialize16(currentProfile->accelerometerTrims.values.pitch);
        serialize16(currentProfile->accelerometerTrims.values.roll);
        break;
    case MSP_UID:
        headSerialReply(12);
        serialize32(U_ID_0);
        serialize32(U_ID_1);
        serialize32(U_ID_2);
        break;
    default:
        return false;
    }
    return true;
}

static bool processInCommand(void)
{
    uint32_t i;
#ifdef GPS
    uint8_t wp_no;
    int32_t lat = 0, lon = 0, alt = 0;
#endif

    switch (currentPort->cmdMSP) {
    case MSP_SELECT_SETTING:
        if (!ARMING_FLAG(ARMED)) {
            masterConfig.current_profile_index = read8();
            if (masterConfig.current_profile_index > 2) {
                masterConfig.current_profile_index = 0;
            }
            writeEEPROM();
            readEEPROM();
        }
        break;
    case MSP_SET_HEAD:
        magHold = read16();
        break;
    case MSP_SET_RAW_RC:
        // FIXME need support for more than 8 channels
        for (i = 0; i < 8; i++)
            rcData[i] = read16();
        rxMspFrameRecieve();
        break;
    case MSP_SET_ACC_TRIM:
        currentProfile->accelerometerTrims.values.pitch = read16();
        currentProfile->accelerometerTrims.values.roll  = read16();
        break;
    case MSP_SET_PID:
        if (currentProfile->pidController == 2) {
            for (i = 0; i < 3; i++) {
                currentProfile->pidProfile.P_f[i] = (float)read8() / 10.0f;
                currentProfile->pidProfile.I_f[i] = (float)read8() / 100.0f;
                currentProfile->pidProfile.D_f[i] = (float)read8() / 1000.0f;
            }
            for (i = 3; i < PID_ITEM_COUNT; i++) {
                if (i == PIDLEVEL) {
                    currentProfile->pidProfile.A_level = (float)read8() / 10.0f;
                    currentProfile->pidProfile.H_level = (float)read8() / 10.0f;
                    read8();
                } else {
                    currentProfile->pidProfile.P8[i] = read8();
                    currentProfile->pidProfile.I8[i] = read8();
                    currentProfile->pidProfile.D8[i] = read8();
                }
            }
        } else {
            for (i = 0; i < PID_ITEM_COUNT; i++) {
                currentProfile->pidProfile.P8[i] = read8();
                currentProfile->pidProfile.I8[i] = read8();
                currentProfile->pidProfile.D8[i] = read8();
            }
        }
        break;
    case MSP_SET_BOX:
        for (i = 0; i < activeBoxIdCount; i++)
            currentProfile->activate[activeBoxIds[i]] = read16() & ACTIVATE_MASK;
        for (i = 0; i < activeBoxIdCount; i++)
            currentProfile->activate[activeBoxIds[i]] |= (read16() & ACTIVATE_MASK) << 16;
        break;
    case MSP_SET_RC_TUNING:
        currentProfile->controlRateConfig.rcRate8 = read8();
        currentProfile->controlRateConfig.rcExpo8 = read8();
        currentProfile->controlRateConfig.rollPitchRate = read8();
        currentProfile->controlRateConfig.yawRate = read8();
        currentProfile->dynThrPID = read8();
        currentProfile->controlRateConfig.thrMid8 = read8();
        currentProfile->controlRateConfig.thrExpo8 = read8();
        break;
    case MSP_SET_MISC:
        read16(); // powerfailmeter
        masterConfig.escAndServoConfig.minthrottle = read16();
        masterConfig.escAndServoConfig.maxthrottle = read16();
        masterConfig.escAndServoConfig.mincommand = read16();
        currentProfile->failsafeConfig.failsafe_throttle = read16();
        read16();
        read32();
        currentProfile->mag_declination = read16() * 10;
        masterConfig.batteryConfig.vbatscale = read8();           // actual vbatscale as intended
        masterConfig.batteryConfig.vbatmincellvoltage = read8();  // vbatlevel_warn1 in MWC2.3 GUI
        masterConfig.batteryConfig.vbatmaxcellvoltage = read8();  // vbatlevel_warn2 in MWC2.3 GUI
        read8();                            // vbatlevel_crit (unused)
        break;
    case MSP_SET_MOTOR:
        for (i = 0; i < 8; i++) // FIXME should this use MAX_MOTORS or MAX_SUPPORTED_MOTORS instead of 8
            motor_disarmed[i] = read16();
        break;
    case MSP_SET_SERVO_CONF:
        for (i = 0; i < MAX_SUPPORTED_SERVOS; i++) {
            currentProfile->servoConf[i].min = read16();
            currentProfile->servoConf[i].max = read16();
            // provide temporary support for old clients that try and send a channel index instead of a servo middle
            uint16_t potentialServoMiddleOrChannelToForward = read16();
            if (potentialServoMiddleOrChannelToForward < MAX_SUPPORTED_SERVOS) {
                currentProfile->servoConf[i].forwardFromChannel = potentialServoMiddleOrChannelToForward;
            }
            if (potentialServoMiddleOrChannelToForward >= PWM_RANGE_MIN && potentialServoMiddleOrChannelToForward <= PWM_RANGE_MAX) {
                currentProfile->servoConf[i].middle = potentialServoMiddleOrChannelToForward;
            }
            currentProfile->servoConf[i].rate = read8();
        }
        break;
    case MSP_SET_CHANNEL_FORWARDING:
        for (i = 0; i < MAX_SUPPORTED_SERVOS; i++) {
            currentProfile->servoConf[i].forwardFromChannel = read8();
        }
        break;
    case MSP_RESET_CONF:
        if (!ARMING_FLAG(ARMED)) {
            resetEEPROM();
            readEEPROM();
        }
        break;
    case MSP_ACC_CALIBRATION:
        if (!ARMING_FLAG(ARMED))
            accSetCalibrationCycles(CALIBRATING_ACC_CYCLES);
        break;
    case MSP_MAG_CALIBRATION:
        if (!ARMING_FLAG(ARMED))
            ENABLE_STATE(CALIBRATE_MAG);
        break;
    case MSP_EEPROM_WRITE:
        if (ARMING_FLAG(ARMED)) {
            headSerialError(0);
            return true;
        }
        writeEEPROM();
        readEEPROM();
        break;
#ifdef GPS
    case MSP_SET_RAW_GPS:
        if (read8()) {
            ENABLE_STATE(GPS_FIX);
        } else {
            DISABLE_STATE(GPS_FIX);
        }
        GPS_numSat = read8();
        GPS_coord[LAT] = read32();
        GPS_coord[LON] = read32();
        GPS_altitude = read16();
        GPS_speed = read16();
        GPS_update |= 2;        // New data signalisation to GPS functions // FIXME Magic Numbers
        break;
    case MSP_SET_WP:
        wp_no = read8();    //get the wp number
        lat = read32();
        lon = read32();
        alt = read32();     // to set altitude (cm)
        read16();           // future: to set heading (deg)
        read16();           // future: to set time to stay (ms)
        read8();            // future: to set nav flag
        if (wp_no == 0) {
            GPS_home[LAT] = lat;
            GPS_home[LON] = lon;
            DISABLE_FLIGHT_MODE(GPS_HOME_MODE);        // with this flag, GPS_set_next_wp will be called in the next loop -- OK with SERIAL GPS / OK with I2C GPS
            ENABLE_STATE(GPS_FIX_HOME);
            if (alt != 0)
                AltHold = alt;          // temporary implementation to test feature with apps
        } else if (wp_no == 16) {       // OK with SERIAL GPS  --  NOK for I2C GPS / needs more code dev in order to inject GPS coord inside I2C GPS
            GPS_hold[LAT] = lat;
            GPS_hold[LON] = lon;
            if (alt != 0)
                AltHold = alt;          // temporary implementation to test feature with apps
            nav_mode = NAV_MODE_WP;
            GPS_set_next_wp(&GPS_hold[LAT], &GPS_hold[LON]);
        }
        break;
#endif
    default:
        // we do not know how to handle the (valid) message, indicate error MSP $M!
        return false;
    }
    headSerialReply(0);
    return true;
}

static void mspProcessPort(void)
{
    uint8_t c;

    while (serialTotalBytesWaiting(mspSerialPort)) {
        c = serialRead(mspSerialPort);

        if (currentPort->c_state == IDLE) {
            currentPort->c_state = (c == '$') ? HEADER_START : IDLE;
            if (currentPort->c_state == IDLE && !ARMING_FLAG(ARMED))
                evaluateOtherData(c); // if not armed evaluate all other incoming serial data
        } else if (currentPort->c_state == HEADER_START) {
            currentPort->c_state = (c == 'M') ? HEADER_M : IDLE;
        } else if (currentPort->c_state == HEADER_M) {
            currentPort->c_state = (c == '<') ? HEADER_ARROW : IDLE;
        } else if (currentPort->c_state == HEADER_ARROW) {
            if (c > INBUF_SIZE) {       // now we are expecting the payload size
                currentPort->c_state = IDLE;
                continue;
            }
            currentPort->dataSize = c;
            currentPort->offset = 0;
            currentPort->checksum = 0;
            currentPort->indRX = 0;
            currentPort->checksum ^= c;
            currentPort->c_state = HEADER_SIZE;      // the command is to follow
        } else if (currentPort->c_state == HEADER_SIZE) {
            currentPort->cmdMSP = c;
            currentPort->checksum ^= c;
            currentPort->c_state = HEADER_CMD;
        } else if (currentPort->c_state == HEADER_CMD && currentPort->offset < currentPort->dataSize) {
            currentPort->checksum ^= c;
            currentPort->inBuf[currentPort->offset++] = c;
        } else if (currentPort->c_state == HEADER_CMD && currentPort->offset >= currentPort->dataSize) {
            if (currentPort->checksum == c) {        // compare calculated and transferred checksum
                // we got a valid packet, evaluate it
                if (!(processOutCommand(currentPort->cmdMSP) || processInCommand())) {
                    headSerialError(0);
                }
                tailSerialReply();
            }
            currentPort->c_state = IDLE;
        }
    }
}

void setCurrentPort(mspPort_t *port)
{
    currentPort = port;
    mspSerialPort = currentPort->port;
}

void mspProcess(void)
{
    uint8_t portIndex;
    mspPort_t *candidatePort;

    for (portIndex = 0; portIndex < MAX_MSP_PORT_COUNT; portIndex++) {
        candidatePort = &mspPorts[portIndex];
        if (candidatePort->mspPortUsage != FOR_GENERAL_MSP) {
            continue;
        }

        setCurrentPort(candidatePort);
        mspProcessPort();
    }
}

static const uint8_t mspTelemetryCommandSequence[] = {
    MSP_BOXNAMES,   // repeat boxnames, in case the first transmission was lost or never received.
    MSP_STATUS,
    MSP_IDENT,
    MSP_RAW_IMU,
    MSP_ALTITUDE,
    MSP_RAW_GPS,
    MSP_RC,
    MSP_MOTOR_PINS,
    MSP_ATTITUDE,
    MSP_SERVO
};

#define MSP_TELEMETRY_COMMAND_SEQUENCE_ENTRY_COUNT (sizeof(mspTelemetryCommandSequence) / sizeof(mspTelemetryCommandSequence[0]))

static mspPort_t *mspTelemetryPort = NULL;

void mspSetTelemetryPort(serialPort_t *serialPort)
{
    uint8_t portIndex;
    mspPort_t *candidatePort = NULL;
    mspPort_t *matchedPort = NULL;

    // find existing telemetry port
    for (portIndex = 0; portIndex < MAX_MSP_PORT_COUNT && !matchedPort; portIndex++) {
        candidatePort = &mspPorts[portIndex];
        if (candidatePort->mspPortUsage == FOR_TELEMETRY) {
            matchedPort = candidatePort;
        }
    }

    if (!matchedPort) {
        // find unused port
        for (portIndex = 0; portIndex < MAX_MSP_PORT_COUNT && !matchedPort; portIndex++) {
            candidatePort = &mspPorts[portIndex];
            if (candidatePort->mspPortUsage == UNUSED_PORT) {
                matchedPort = candidatePort;
            }
        }
    }
    mspTelemetryPort = matchedPort;
    if (!mspTelemetryPort) {
        return;
    }

    resetMspPort(mspTelemetryPort, serialPort, FOR_TELEMETRY);
}

void sendMspTelemetry(void)
{
    static uint32_t sequenceIndex = 0;

    if (!mspTelemetryPort) {
        return;
    }

    setCurrentPort(mspTelemetryPort);

    processOutCommand(mspTelemetryCommandSequence[sequenceIndex]);
    tailSerialReply();

    sequenceIndex++;
    if (sequenceIndex >= MSP_TELEMETRY_COMMAND_SEQUENCE_ENTRY_COUNT) {
        sequenceIndex = 0;
    }
}
