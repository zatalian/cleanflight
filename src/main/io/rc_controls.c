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
#include <string.h>

#include <math.h>

#include "common/axis.h"
#include "common/maths.h"

#include "config/config.h"
#include "config/runtime_config.h"

#include "flight/flight.h"

#include "drivers/accgyro.h"

#include "sensors/sensors.h"
#include "sensors/gyro.h"
#include "sensors/acceleration.h"

#include "io/gps.h"
#include "mw.h"

#include "rx/rx.h"
#include "io/rc_controls.h"

int16_t rcCommand[4];           // interval [1000;2000] for THROTTLE and [-500;+500] for ROLL/PITCH/YAW

// each entry in the array is a bitmask, 3 bits per aux channel (only aux 1 to 4), aux1 is first, each bit corresponds to an rc channel reading
// bit 1 - stick LOW
// bit 2 - stick MIDDLE
// bit 3 - stick HIGH
// an option is enabled when ANY channel has an appropriate reading corresponding to the bit.
// an option is disabled when NO channel has an appropriate reading corresponding to the bit.
// example: 110000000001 - option is only enabled when AUX1 is LOW or AUX4 is MEDIUM or HIGH.
uint8_t rcOptions[CHECKBOX_ITEM_COUNT];

bool areSticksInApModePosition(uint16_t ap_mode)
{
    return abs(rcCommand[ROLL]) < ap_mode && abs(rcCommand[PITCH]) < ap_mode;
}

throttleStatus_e calculateThrottleStatus(rxConfig_t *rxConfig, uint16_t deadband3d_throttle)
{
    if (feature(FEATURE_3D) && (rcData[THROTTLE] > (rxConfig->midrc - deadband3d_throttle) && rcData[THROTTLE] < (rxConfig->midrc + deadband3d_throttle)))
        return THROTTLE_LOW;
    else if (!feature(FEATURE_3D) && (rcData[THROTTLE] < rxConfig->mincheck))
        return THROTTLE_LOW;

    return THROTTLE_HIGH;
}

void processRcStickPositions(rxConfig_t *rxConfig, throttleStatus_e throttleStatus, uint32_t *activate, bool retarded_arm, bool disarm_kill_switch)
{
    static uint8_t rcDelayCommand;      // this indicates the number of time (multiple of RC measurement at 50Hz) the sticks must be maintained to run or switch off motors
    static uint8_t rcSticks;            // this hold sticks position for command combos
    uint8_t stTmp = 0;
    int i;

    // ------------------ STICKS COMMAND HANDLER --------------------
    // checking sticks positions
    for (i = 0; i < 4; i++) {
        stTmp >>= 2;
        if (rcData[i] > rxConfig->mincheck)
            stTmp |= 0x80;  // check for MIN
        if (rcData[i] < rxConfig->maxcheck)
            stTmp |= 0x40;  // check for MAX
    }
    if (stTmp == rcSticks) {
        if (rcDelayCommand < 250)
            rcDelayCommand++;
    } else
        rcDelayCommand = 0;
    rcSticks = stTmp;

    // perform actions
    if (activate[BOXARM] > 0) {

        if (rcOptions[BOXARM]) {
            // Arming via ARM BOX
            if (throttleStatus == THROTTLE_LOW) {
                if (ARMING_FLAG(OK_TO_ARM)) {
                    mwArm();
                }
            }
        } else {
            // Disarming via ARM BOX
            if (ARMING_FLAG(ARMED)) {
                if (disarm_kill_switch) {
                    mwDisarm();
                } else if (throttleStatus == THROTTLE_LOW) {
                    mwDisarm();
                }
            }
        }
    }

    if (rcDelayCommand != 20) {
        return;
    }

    if (ARMING_FLAG(ARMED)) {
        // actions during armed

        // Disarm on throttle down + yaw
        if (activate[BOXARM] == 0 && (rcSticks == THR_LO + YAW_LO + PIT_CE + ROL_CE))
            mwDisarm();

        // Disarm on roll (only when retarded_arm is enabled)
        if (retarded_arm && activate[BOXARM] == 0 && (rcSticks == THR_LO + YAW_CE + PIT_CE + ROL_LO))
            mwDisarm();

        return;
    }

    // actions during not armed
    i = 0;

    if (rcSticks == THR_LO + YAW_LO + PIT_LO + ROL_CE) {
        // GYRO calibration
        gyroSetCalibrationCycles(CALIBRATING_GYRO_CYCLES);

#ifdef GPS
        if (feature(FEATURE_GPS)) {
            GPS_reset_home_position();
        }
#endif

#ifdef BARO
        if (sensors(SENSOR_BARO))
            baroSetCalibrationCycles(10); // calibrate baro to new ground level (10 * 25 ms = ~250 ms non blocking)
#endif

        if (!sensors(SENSOR_MAG))
            heading = 0; // reset heading to zero after gyro calibration

        return;
    }

    if (feature(FEATURE_INFLIGHT_ACC_CAL) && (rcSticks == THR_LO + YAW_LO + PIT_HI + ROL_HI)) {
        // Inflight ACC Calibration
        handleInflightCalibrationStickPosition();
        return;
    }

    // Multiple configuration profiles
    if (rcSticks == THR_LO + YAW_LO + PIT_CE + ROL_LO)          // ROLL left  -> Profile 1
        i = 1;
    else if (rcSticks == THR_LO + YAW_LO + PIT_HI + ROL_CE)     // PITCH up   -> Profile 2
        i = 2;
    else if (rcSticks == THR_LO + YAW_LO + PIT_CE + ROL_HI)     // ROLL right -> Profile 3
        i = 3;
    if (i) {
        changeProfile(i - 1);
        return;
    }

    if (activate[BOXARM] == 0 && (rcSticks == THR_LO + YAW_HI + PIT_CE + ROL_CE)) {
        // Arm via YAW
        mwArm();
        return;
    }

    if (retarded_arm && activate[BOXARM] == 0 && (rcSticks == THR_LO + YAW_CE + PIT_CE + ROL_HI)) {
        // Arm via ROLL
        mwArm();
        return;
    }

    if (rcSticks == THR_HI + YAW_LO + PIT_LO + ROL_CE) {
        // Calibrating Acc
        accSetCalibrationCycles(CALIBRATING_ACC_CYCLES);
        return;
    }


    if (rcSticks == THR_HI + YAW_HI + PIT_LO + ROL_CE) {
        // Calibrating Mag
        ENABLE_STATE(CALIBRATE_MAG);
        return;
    }


    // Accelerometer Trim

    rollAndPitchTrims_t accelerometerTrimsDelta;
    memset(&accelerometerTrimsDelta, 0, sizeof(accelerometerTrimsDelta));

    bool shouldApplyRollAndPitchTrimDelta = false;
    if (rcSticks == THR_HI + YAW_CE + PIT_HI + ROL_CE) {
        accelerometerTrimsDelta.values.pitch = 2;
        shouldApplyRollAndPitchTrimDelta = true;
    } else if (rcSticks == THR_HI + YAW_CE + PIT_LO + ROL_CE) {
        accelerometerTrimsDelta.values.pitch = -2;
        shouldApplyRollAndPitchTrimDelta = true;
    } else if (rcSticks == THR_HI + YAW_CE + PIT_CE + ROL_HI) {
        accelerometerTrimsDelta.values.roll = 2;
        shouldApplyRollAndPitchTrimDelta = true;
    } else if (rcSticks == THR_HI + YAW_CE + PIT_CE + ROL_LO) {
        accelerometerTrimsDelta.values.roll = -2;
        shouldApplyRollAndPitchTrimDelta = true;
    }
    if (shouldApplyRollAndPitchTrimDelta) {
        applyAndSaveAccelerometerTrimsDelta(&accelerometerTrimsDelta);
        rcDelayCommand = 0; // allow autorepetition
        return;
    }
}

#define MAX_AUX_STATE_CHANNELS 8

void updateRcOptions(uint32_t *activate)
{
    // auxState is a bitmask, 3 bits per channel.
    // lower 16 bits contain aux 4 to 1 (msb to lsb)
    // upper 16 bits contain aux 8 to 5 (msb to lsb)
    //
    // the three bits are as follows:
    // bit 1 is SET when the stick is less than 1300
    // bit 2 is SET when the stick is between 1300 and 1700
    // bit 3 is SET when the stick is above 1700

    // if the value is 1300 or 1700 NONE of the three bits are set.

    int i;
    uint32_t auxState = 0;
    uint8_t shift = 0;
    int8_t chunkOffset = 0;

    for (i = 0; i < rxRuntimeConfig.auxChannelCount && i < MAX_AUX_STATE_CHANNELS; i++) {
        if (i > 0 && i % 4 == 0) {
            chunkOffset -= 4;
            shift += 16;
        }

        uint8_t bitIndex = 3 * (i + chunkOffset);

        uint32_t temp =
                (rcData[AUX1 + i] < 1300) << bitIndex |
                (1300 < rcData[AUX1 + i] && rcData[AUX1 + i] < 1700) << (bitIndex + 1) |
                (rcData[AUX1 + i] > 1700) << (bitIndex + 2);

        auxState |= temp << shift;
    }

    for (i = 0; i < CHECKBOX_ITEM_COUNT; i++)
        rcOptions[i] = (auxState & activate[i]) > 0;
}
