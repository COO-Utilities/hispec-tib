/*
 * Copyright (c) 2024 Caltech Optical Observatories
 * SPDX-License-Identifier: Apache-2.0
 */

#include <coo_commons/pid.h>
#include <string.h>

void coo_pid_init(struct coo_pid *pid, float kp, float ki, float kd,
		  float output_min, float output_max)
{
	memset(pid, 0, sizeof(*pid));

	pid->kp = kp;
	pid->ki = ki;
	pid->kd = kd;

	pid->output_min = output_min;
	pid->output_max = output_max;

	/* Set integral limits to output limits by default */
	pid->integral_min = output_min;
	pid->integral_max = output_max;
}

void coo_pid_reset(struct coo_pid *pid)
{
	pid->integral = 0.0f;
	pid->prev_error = 0.0f;
}

void coo_pid_set_gains(struct coo_pid *pid, float kp, float ki, float kd)
{
	pid->kp = kp;
	pid->ki = ki;
	pid->kd = kd;
}

float coo_pid_update(struct coo_pid *pid, float setpoint, float measured, float dt)
{
	float error, derivative, output;

	/* Calculate error */
	error = setpoint - measured;

	/* Proportional term */
	float p_term = pid->kp * error;

	/* Integral term with anti-windup */
	pid->integral += error * dt;
	if (pid->integral > pid->integral_max) {
		pid->integral = pid->integral_max;
	} else if (pid->integral < pid->integral_min) {
		pid->integral = pid->integral_min;
	}
	float i_term = pid->ki * pid->integral;

	/* Derivative term */
	derivative = (dt > 0.0f) ? (error - pid->prev_error) / dt : 0.0f;
	float d_term = pid->kd * derivative;

	/* Compute output */
	output = p_term + i_term + d_term;

	/* Clamp output */
	if (output > pid->output_max) {
		output = pid->output_max;
	} else if (output < pid->output_min) {
		output = pid->output_min;
	}

	/* Save error for next iteration */
	pid->prev_error = error;

	return output;
}
