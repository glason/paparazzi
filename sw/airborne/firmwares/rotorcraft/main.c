/*
 * $Id$
 *
 * Copyright (C) 2008-2010 The Paparazzi Team
 *
 * This file is part of Paparazzi.
 *
 * Paparazzi is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * Paparazzi is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Paparazzi; see the file COPYING.  If not, write to
 * the Free Software Foundation, 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 */

#define MODULES_C

#include <inttypes.h>
#include "init_hw.h"
#include "sys_time.h"
#include "led.h"
#include "interrupt_hw.h"

#include "downlink.h"
#include "booz2_telemetry.h"
#include "datalink.h"

#include "booz2_commands.h"
#include "booz_actuators.h"
#include "booz_radio_control.h"

#include "booz_imu.h"
#include "booz_gps.h"

#include "booz/booz2_analog.h"
#include "firmwares/rotorcraft/baro.h"

#include "booz2_battery.h"

#include "booz_fms.h"
#include "booz2_autopilot.h"

#include "booz_stabilization.h"
#include "booz_guidance.h"

#include "booz_ahrs.h"
#include "booz2_ins.h"

#if defined USE_CAM || USE_DROP
#include "booz2_pwm_hw.h"
#endif

#include "firmwares/rotorcraft/main.h"

#ifdef SITL
#include "nps_autopilot_booz.h"
#endif

#include "modules.h"

static inline void on_gyro_accel_event( void );
static inline void on_baro_abs_event( void );
static inline void on_baro_dif_event( void );
static inline void on_gps_event( void );
static inline void on_mag_event( void );

#ifndef SITL
int main( void ) {
  booz2_main_init();

  while(1) {
    if (sys_time_periodic())
      booz2_main_periodic();
    booz2_main_event();
  }
  return 0;
}
#endif /* SITL */

STATIC_INLINE void booz2_main_init( void ) {

#ifndef RADIO_CONTROL_LINK
  /* read the comment below the bind function needs to start as close to powerup as possible 
     it also blocks for 73ms which is longer than this loop could this be moved elsewhere */
  for (uint32_t startup_counter=0; startup_counter<2000000; startup_counter++){
    __asm("nop");
  }
#endif

  hw_init();

  sys_time_init();

  actuators_init();
  radio_control_init();

  booz2_analog_init();
  baro_init();

#if defined USE_CAM || USE_DROP
  booz2_pwm_init_hw();
#endif

  booz2_battery_init();
  booz_imu_init();

  booz_fms_init();
  booz2_autopilot_init();
  booz2_nav_init();
  booz2_guidance_h_init();
  booz2_guidance_v_init();
  booz_stabilization_init();

  booz_ahrs_aligner_init();
  booz_ahrs_init();

  booz_ins_init();

#ifdef USE_GPS
  booz_gps_init();
#endif

  modules_init();

  int_enable();

}


STATIC_INLINE void booz2_main_periodic( void ) {

  booz_imu_periodic();

  /* run control loops */
  booz2_autopilot_periodic();
  /* set actuators     */
  actuators_set(booz2_autopilot_motors_on);

  PeriodicPrescaleBy10(							\
    {						                        \
      radio_control_periodic();						\
      if (radio_control.status != RADIO_CONTROL_OK &&			\
          booz2_autopilot_mode != BOOZ2_AP_MODE_KILL &&			\
          booz2_autopilot_mode != BOOZ2_AP_MODE_NAV)			\
        booz2_autopilot_set_mode(BOOZ2_AP_MODE_FAILSAFE);		\
    },									\
    {									\
      booz_fms_periodic();						\
    },									\
    {									\
      /*BoozControlSurfacesSetFromCommands();*/				\
    },									\
    {									\
      LED_PERIODIC();		     					\
    },									\
    { baro_periodic();
    },									\
    {},									\
    {},									\
    {},									\
    {},									\
    {									\
      Booz2TelemetryPeriodic();						\
    }									\
    );									\
  
#ifdef USE_GPS
  if (radio_control.status != RADIO_CONTROL_OK &&			\
      booz2_autopilot_mode == BOOZ2_AP_MODE_NAV && GpsIsLost())		\
    booz2_autopilot_set_mode(BOOZ2_AP_MODE_FAILSAFE);			\
  booz_gps_periodic();
#endif

#ifdef USE_EXTRA_ADC
  booz2_analog_periodic();
#endif

  modules_periodic_task();

  if (booz2_autopilot_in_flight) {
    RunOnceEvery(512, { booz2_autopilot_flight_time++; datalink_time++; });
  }

}

STATIC_INLINE void booz2_main_event( void ) {

  DatalinkEvent();

  if (booz2_autopilot_rc) {
    RadioControlEvent(booz2_autopilot_on_rc_frame);
  }

  BoozImuEvent(on_gyro_accel_event, on_mag_event);

  BaroEvent(on_baro_abs_event, on_baro_dif_event);

#ifdef USE_GPS
  BoozGpsEvent(on_gps_event);
#endif

#ifdef BOOZ_FAILSAFE_GROUND_DETECT
  BoozDetectGroundEvent();
#endif

  modules_event_task();

}

static inline void on_gyro_accel_event( void ) {

  BoozImuScaleGyro(booz_imu);
  BoozImuScaleAccel(booz_imu);

  if (booz_ahrs.status == BOOZ_AHRS_UNINIT) {
    booz_ahrs_aligner_run();
    if (booz_ahrs_aligner.status == BOOZ_AHRS_ALIGNER_LOCKED)
      booz_ahrs_align();
  }
  else {
    booz_ahrs_propagate();
    booz_ahrs_update_accel();
#ifdef SITL
    if (nps_bypass_ahrs) sim_overwrite_ahrs();
#endif
    booz_ins_propagate();
  }
}

static inline void on_baro_abs_event( void ) {
  booz_ins_update_baro();
}

static inline void on_baro_dif_event( void ) {

}

static inline void on_gps_event(void) {
  booz_ins_update_gps();
}

static inline void on_mag_event(void) {
  BoozImuScaleMag(booz_imu);
  if (booz_ahrs.status == BOOZ_AHRS_RUNNING)
    booz_ahrs_update_mag();
}