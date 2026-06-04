/**
 * @file pid.c
 * @brief Reusable bounded PID controller helper.
 *
 * The current firmware does not put this helper on the command path, but it is
 * part of the app-local COO commons library included in API extraction.
 */
/*
 * Copyright (c) 2024 Caltech Optical Observatories
 * SPDX-License-Identifier: Apache-2.0
 */

#include <coo_commons/pid.h>
#include <string.h>

void coo_pid_init(struct coo_pid *pid, double kp, double ki, double kd,
		  double output_min, double output_max)
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
	pid->integral = 0.0;
	pid->prev_error = 0.0;
}

void coo_pid_set_gains(struct coo_pid *pid, double kp, double ki, double kd)
{
	pid->kp = kp;
	pid->ki = ki;
	pid->kd = kd;
}

double coo_pid_update(struct coo_pid *pid, double setpoint, double measured, double dt)
{
	double error, derivative, output;

	/* Calculate error */
	error = setpoint - measured;

	/* Proportional term */
	double p_term = pid->kp * error;

	/* Integral term with anti-windup */
	pid->integral += error * dt;
	if (pid->integral > pid->integral_max) {
		pid->integral = pid->integral_max;
	} else if (pid->integral < pid->integral_min) {
		pid->integral = pid->integral_min;
	}
	double i_term = pid->ki * pid->integral;

	/* Derivative term */
	derivative = (dt > 0.0) ? (error - pid->prev_error) / dt : 0.0;
	double d_term = pid->kd * derivative;

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
