/*! 
 *  Takes care of manual, stabilized and autopilot control!
 *
 *  Input: RC-transmitter, sensor data
 *  Output: Servo positions
 *
 *  Conventions: For RC-transmitter and mixer input:
 *               Right roll > 0 -> input > 1500ms
 *               Up pitch > 0 -> input > 1500ms
 *
 *
 *  @file     control.c
 *  @author   Tom Pycke
 *  @date     24-dec-2009
 *  @since    0.1
 */

#include <math.h>

#include "FreeRTOS/FreeRTOS.h"
#include "FreeRTOS/task.h"
#include "FreeRTOS/queue.h"
#include "FreeRTOS/croutine.h"
#include "FreeRTOS/semphr.h"

#include "ppm_in/ppm_in.h"
#include "servo/servo.h"
#include "pid/pid.h"
#include "uart1_queue/uart1_queue.h"

#include "control.h"
#include "configuration.h"
#include "sensors.h"
#include "navigation.h"

#define PI 3.14159

void control_manual();
void control_stabilized(double dt, int h);
void control_navigate(double dt);
void control_mix_aileron_uint(int pitch, int roll, int yaw, int motor);
void control_desired_to_servos();


//! Contains the last calculated servo position
int servo_out[6];

//! Different outputs before mixing. Set by any of the 3 control methods, and used by the servo mixing type.
int elevator_out = 0, aileron_out = 0, yaw_out = 0, motor_out = 0;

//! Mix xyz_out to servo_out
void control_mix_out();

//! Contains the currect state of the control loop
struct ControlState control_state;
      
	          

/*!
 *    Initializes the control module.
 *    It uses the current RC-transmitter's input to determine the servo's neutral settings.
 *    This shall only work when the stick of the transmitter are not touched during this method!
 */
void control_init()
{
	int i;
	
	//ppm_in_open(); MOVED TO MAIN
	
	// Manual trim mode: the servo's neutral settings are defined by the RC-transmitters trim settings. See wiki.
	if (config.control.manual_trim)
	{	
		for (i = 0; i < 3; i++)
			config.control.servo_neutral[i] = 1500;
			
		// The current position of the sticks on the RC-transmitter are 
		// saved as the neutral values
		for (i = 0; i < 8; i++)
			config.control.channel_neutral[i] = ppm.channel[i];
		
		// This procedure calculates the servo's neutral settings using the RC-transmitter's input
		elevator_out = config.control.channel_neutral[config.control.channel_pitch] - 1500;
		aileron_out = config.control.channel_neutral[config.control.channel_roll] - 1500;
		yaw_out = config.control.channel_neutral[config.control.channel_yaw] - 1500;
		motor_out = config.control.channel_neutral[config.control.channel_motor] - 1500;
		
		control_mix_out();  // mix the neutral RC-channels so we can get a neutral setting for the servos
		
		// mixed outputs are our servo's neutral values
		for (i = 0; i < 6; i++)
			config.control.servo_neutral[i] = servo_out[i];
	}
}


/*!
 *   
 */
void control_task( void *parameters )
{
	enum FlightModes lastMode = MANUAL;
	
	/* Used to wake the task at the correct frequency. */
	portTickType xLastExecutionTime; 

	uart1_puts("Control task initializing...");
	
	servo_init();
	control_init();

#ifdef ENABLE_QUADROCOPTER
	vTaskDelayUntil( &xLastExecutionTime, ( ( portTickType ) 1000 / portTICK_RATE_MS ) );
	servo_turbopwm();
#endif

	uart1_puts("done\r\n");
	
	/* Initialise xLastExecutionTime so the first call to vTaskDelayUntil()	works correctly. */
	xLastExecutionTime = xTaskGetTickCount();

	for( ;; )
	{
#ifdef ENABLE_QUADROCOPTER
		vTaskDelayUntil( &xLastExecutionTime, ( ( portTickType ) 5 / portTICK_RATE_MS ) );    //!> 200Hz
		#define DT 0.005
#else
		vTaskDelayUntil( &xLastExecutionTime, ( ( portTickType ) 10 / portTICK_RATE_MS ) );   //!> 100Hz
		#define DT 0.01
#endif

		if (ppm.channel[config.control.channel_ap] < 1333)
		{
			control_state.flight_mode = AUTOPILOT;
			
			//control_state.desired_height = home_height + 65.0;
			if (lastMode != control_state.flight_mode)  // target altitude = altitude when switching from manual to stabilized
				control_state.desired_height = sensor_data.pressure_height;
				
			control_navigate(DT); // stabilized mode as long as navigation isn't available
			//control_stabilized(DT, 1); // stabilized mode
		} 
		else if (ppm.channel[config.control.channel_ap] < 1666)
		{
			control_state.flight_mode = STABILIZED;
			if (lastMode != control_state.flight_mode)  // target altitude = altitude when switching from manual to stabilized
				control_state.desired_height = sensor_data.pressure_height; //home_height + 65.0;
			control_stabilized(DT, 0); // stabilized mode
		} 
		else
		{
			control_state.flight_mode = MANUAL;
			control_manual(); // manual mode
			
		}
		lastMode = control_state.flight_mode;
	}
}


/*!
 *   Reads the input from the RC-transmitter, mixes it and sends it to the servos.
 */
void control_manual()
{
	// > 0 => Up
	elevator_out = ppm.channel[config.control.channel_pitch] - config.control.channel_neutral[config.control.channel_pitch];
	// > 0 => Right
	aileron_out = ppm.channel[config.control.channel_roll] - config.control.channel_neutral[config.control.channel_roll];
	// > 0 => Motor on
	motor_out = ppm.channel[config.control.channel_motor] - config.control.channel_neutral[config.control.channel_motor];
	// > 0 => Right
	yaw_out = ppm.channel[config.control.channel_yaw] - config.control.channel_neutral[config.control.channel_yaw];
	
	control_mix_out();
}


/*!
 *   The RC-transmitter's sticks define the "desired" attitude. The sensors that define
 *   the module's attitude are used in a PID loop to position the servo's so the
 *   desired attitude can be obtained.
 */
void control_stabilized(double dt, int altitude_hold)
{
	control_state.desired_roll = (double)((int)ppm.channel[config.control.channel_roll]
	                             - config.control.channel_neutral[config.control.channel_roll]) / 500.0 * (config.control.max_roll);

	// Comment this line if you want pitch stabilization instead of altitude hold
	if (altitude_hold)
	{
		if (abs(control_state.desired_pitch) > (config.control.max_pitch / 5.0)) // elevator stick not in neutral position
		{
			control_state.desired_pitch = (double)((int)ppm.channel[config.control.channel_pitch]
	        		                      - config.control.channel_neutral[config.control.channel_pitch]) / 500.0 * (config.control.max_pitch);
			control_state.desired_height = sensor_data.pressure_height;  // keep height in case stick goes back to neutral
		}	
		else
		  control_state.desired_pitch = (control_state.desired_height - sensor_data.pressure_height)  / 20.0 * config.control.max_pitch; 
	} 
	else
		control_state.desired_pitch = (double)((int)ppm.channel[config.control.channel_pitch]
		                              - config.control.channel_neutral[config.control.channel_pitch]) / 500.0 * (config.control.max_pitch);

	control_desired_to_servos(dt);
}



/*!
 *   The RC-transmitter's sticks define the "desired" attitude. The sensors that define
 *   the module's attitude are used in a PID loop to position the servo's so the
 *   desired attitude can be obtained.
 */
void control_navigate(double dt)
{
	double heading_error_rad = navigation_data.desired_heading_rad - sensor_data.gps.heading_rad;
	
	// Choose shortest turn-direction
	if (heading_error_rad >= PI)
		heading_error_rad -= (PI*2.0);
	else if (heading_error_rad <= -PI)
		heading_error_rad += (PI*2.0);
		
	control_state.desired_roll = pid_update_only_p(&config.control.pid_heading2roll, heading_error_rad, dt);	
	
	// from paparazzi
	double speed_depend_nav = sensor_data.gps.speed_ms/20.0;   // cruising airspeed = 20m/s
 	if (speed_depend_nav > 1.5)
 		control_state.desired_roll *= 1.5;
 	else if (speed_depend_nav < 0.66)
 		control_state.desired_roll *= 0.66;
	else
		control_state.desired_roll *= speed_depend_nav;
	
	
	control_state.desired_pitch = (control_state.desired_height - sensor_data.pressure_height)  / 20.0 * config.control.max_pitch; 

	control_desired_to_servos(dt);
}


/*!
 *   Takes control_state.desired_roll and control_state.desired_pitch as input,
 *   and calculates elevator_out and aileron_out.
 */
void control_desired_to_servos(double dt)
{
	double elevator_out_radians,
	       aileron_out_radians;
	
	// Keep pitch & roll within limits
	if (control_state.desired_pitch > config.control.max_pitch)
		control_state.desired_pitch = config.control.max_pitch;
	else if (control_state.desired_pitch < -config.control.max_pitch)
		control_state.desired_pitch = -config.control.max_pitch;
	if (control_state.desired_roll > config.control.max_roll)
		control_state.desired_roll = config.control.max_roll;
	else if (control_state.desired_roll < -config.control.max_roll)
		control_state.desired_roll = -config.control.max_roll;

	// compensate the loss in lift
	//control_state.desired_pitch += (1.0/cosf(sensor_data.roll) - 1.0)*0.25; // (0.5: 12� up at 45� roll)
	
#ifdef ENABLE_QUADROCOPTER	
	elevator_out_radians = pid_update_only_p(&config.control.pid_pitch2elevator, 
	                                         control_state.desired_pitch - sensor_data.pitch, dt);
	elevator_out_radians -= sensor_data.q * config.control.pid_pitch2elevator.d_gain;
	aileron_out_radians = pid_update_only_p(&config.control.pid_roll2aileron, 
	                                        control_state.desired_roll - sensor_data.roll, dt);
	aileron_out_radians -= (sensor_data.p) * config.control.pid_roll2aileron.d_gain;
	
	yaw_out -= (int)((sensor_data.r) * config.control.pid_heading2roll.d_gain * 630.0);
#else
	elevator_out_radians = pid_update(&config.control.pid_pitch2elevator, 
	                                         control_state.desired_pitch - sensor_data.pitch, dt);
	aileron_out_radians = pid_update(&config.control.pid_roll2aileron, 
	                                        control_state.desired_roll - sensor_data.roll, dt);
#endif
	
	// Experimental: when flying with the wind, the elevons become less effective. Avoid have a too large roll angle!
	// Fixme: gain scheduling depending on V_air
	/*if (sensor_data.roll > config.control.max_roll)
		aileron_out_radians *= 1.2;
	if (sensor_data.roll < -config.control.max_roll)
		aileron_out_radians *= 1.2;
	if (sensor_data.pitch > config.control.max_pitch)
		elevator_out_radians *= 1.2;*/
	

	motor_out = ppm.channel[config.control.channel_motor] - config.control.channel_neutral[config.control.channel_motor];
	yaw_out = ppm.channel[config.control.channel_yaw] - config.control.channel_neutral[config.control.channel_yaw];
	
	elevator_out = (int)(elevator_out_radians * 630.0); // +-45� -> +- 500
	aileron_out = (int)(aileron_out_radians * 630.0);

	control_mix_out();
}	


/*!
 *   Mixes variables aileron_out and elevator_out into correct servo positions, according to the configured mixing type.
 * 
 *   Input: 
 *      aileron_out: > 0 means right [-500..500]
 *      elevator_out: > 0 means up   [-500..500]
 *      motor_out: > 0 means on/more gas [0..1000]
 *
 */
void control_mix_out()
{
	static int j = 0;
	int i;
	int aileron_out_left, aileron_out_right;
	
	// aileron differential
	/*if (aileron_out > 0)
	{
		aileron_out_right = aileron_out + (aileron_out / 10) * 5;
		aileron_out_left = aileron_out - (aileron_out / 10) * 5;
	} 
	else
	{
		aileron_out_right = aileron_out - (aileron_out / 10) * 5;
		aileron_out_left = aileron_out + (aileron_out / 10) * 5;		
	}*/
	
	// no differential
	aileron_out_right = aileron_out;
	aileron_out_left = aileron_out;		
		
	
	switch(config.control.servo_mix)
	{
		case DELTA_PLUS:
			if (config.control.reverse_servo1)
				servo_out[0] = +aileron_out_right + elevator_out + config.control.servo_neutral[0];
			else
				servo_out[0] = -aileron_out_right - elevator_out + config.control.servo_neutral[0];
				
			if (config.control.reverse_servo2)
				servo_out[1] = +aileron_out_left - elevator_out + config.control.servo_neutral[1];
			else
				servo_out[1] = -aileron_out_left + elevator_out + config.control.servo_neutral[1];
				
			if (config.control.reverse_servo4)
				servo_out[3] = -motor_out + config.control.servo_neutral[3];
			else 
				servo_out[3] = motor_out + config.control.servo_neutral[3];

			break;
		case DELTA_MIN:
			if (config.control.reverse_servo1)
				servo_out[0] = +aileron_out_right - elevator_out + config.control.servo_neutral[0];
			else
				servo_out[0] = -aileron_out_right + elevator_out + config.control.servo_neutral[0];
				
			if (config.control.reverse_servo2)
				servo_out[1] = +aileron_out_left + elevator_out + config.control.servo_neutral[1];
			else
				servo_out[1] = -aileron_out_left - elevator_out + config.control.servo_neutral[1];
			
			if (config.control.reverse_servo4)
				servo_out[3] = -motor_out + config.control.servo_neutral[3];
			else 
				servo_out[3] = motor_out + config.control.servo_neutral[3];

			break;
		case QUADROCOPTER:
		{
			/*          >
			 *          0
			 *      < /   \ <
			 *      3       1
			 *        \ > /
			 *          2
			 */
			
			/*if (motor_out < 100) // about zero throttle
			{
				servo_out[0] = config.control.servo_neutral[3];
				servo_out[1] = config.control.servo_neutral[3];
				servo_out[2] = config.control.servo_neutral[3];
				servo_out[3] = config.control.servo_neutral[3];
			} 
			else*/
			{	
				// channel[motor] neutral = 1200
				// servo_neutral(init) = 1500
				// motor_out(init) = 1200 - 1500 = -300 -> in mix -> 
				// servo_neutral(mix) = 1200
				
				// P:0.4 (convergeert traag), D: 0.45 (0.6 oscilleert)

				servo_out[0] = motor_out + aileron_out/5 + yaw_out/5 + config.control.servo_neutral[0];
				servo_out[1] = motor_out + elevator_out/5 - yaw_out/5 + config.control.servo_neutral[1];
				servo_out[2] = motor_out - aileron_out/5  + yaw_out/5 + config.control.servo_neutral[2];
				servo_out[3] = motor_out - elevator_out/5 - yaw_out/5 + config.control.servo_neutral[3];
			}
			break;
		}	
		default:  // aileron
			if (config.control.reverse_servo1)
				servo_out[0] = -aileron_out_right + config.control.servo_neutral[0];
			else
				servo_out[0] = +aileron_out_right + config.control.servo_neutral[0];
				
			if (config.control.reverse_servo2)
				servo_out[1] = +aileron_out_left + config.control.servo_neutral[1];
			else
				servo_out[1] = -aileron_out_left + config.control.servo_neutral[1];
				
			if (config.control.reverse_servo3)
				servo_out[2] = -elevator_out + config.control.servo_neutral[2];
			else
				servo_out[2] = elevator_out + config.control.servo_neutral[2];
				
			if (config.control.reverse_servo4)
				servo_out[3] = -motor_out + config.control.servo_neutral[3];
			else
				servo_out[3] = motor_out + config.control.servo_neutral[3];
			break;
			
			if (config.control.reverse_servo5)
				servo_out[4] = +yaw_out + config.control.servo_neutral[4];
			else 
				servo_out[4] = -yaw_out + config.control.servo_neutral[4];

	}
	
	for(i = 0; i < 6; i++)
	{	
		if (servo_out[i] > config.control.servo_max[i])
			servo_out[i] = config.control.servo_max[i];
		if (servo_out[i] < config.control.servo_min[i])
			servo_out[i] = config.control.servo_min[i];

		servo_set_us(i, servo_out[i]);
	}
}
