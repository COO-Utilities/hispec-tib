/*
 * Copyright (c) 2024 Caltech Optical Observatories
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_LIB_PID_H_
#define APP_LIB_PID_H_

#include <stdint.h>

/**
 * @file pid.h
 * @brief PID controller implementation for COO applications
 *
 * This module provides a reusable PID (Proportional-Integral-Derivative)
 * controller used across COO instruments for temperature control,
 * motion control, and other closed-loop applications.
 */

/**
 * @brief PID controller state structure
 */
struct coo_pid {
	/** Proportional gain */
	float kp;
	/** Integral gain */
	float ki;
	/** Derivative gain */
	float kd;

	/** Integral accumulator */
	float integral;
	/** Previous error for derivative calculation */
	float prev_error;

	/** Output limits */
	float output_min;
	float output_max;

	/** Anti-windup: limit integral accumulation */
	float integral_min;
	float integral_max;
};

/**
 * @brief Initialize a PID controller
 *
 * @param pid Pointer to PID controller structure
 * @param kp Proportional gain
 * @param ki Integral gain
 * @param kd Derivative gain
 * @param output_min Minimum output value
 * @param output_max Maximum output value
 */
void coo_pid_init(struct coo_pid *pid, float kp, float ki, float kd,
		  float output_min, float output_max);

/**
 * @brief Reset PID controller state (clear integral and error history)
 *
 * @param pid Pointer to PID controller structure
 */
void coo_pid_reset(struct coo_pid *pid);

/**
 * @brief Update PID gains
 *
 * @param pid Pointer to PID controller structure
 * @param kp Proportional gain
 * @param ki Integral gain
 * @param kd Derivative gain
 */
void coo_pid_set_gains(struct coo_pid *pid, float kp, float ki, float kd);

/**
 * @brief Compute PID output
 *
 * @param pid Pointer to PID controller structure
 * @param setpoint Desired target value
 * @param measured Current measured value
 * @param dt Time delta since last update (seconds)
 * @return Computed control output (clamped to output limits)
 */
float coo_pid_update(struct coo_pid *pid, float setpoint, float measured, float dt);

#endif /* APP_LIB_PID_H_ */
