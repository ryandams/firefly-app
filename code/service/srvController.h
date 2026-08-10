/* SPDX-License-Identifier: Apache-2.0 */
/*
 * srvController - mission sequencing service.
 *
 * This header is the extern "C" boundary: core/ and port/ above it are C++20,
 * everything below can stay plain C like the vestfrost services being ported
 * across.
 */
#ifndef _SRVCONTROLLER_H_
#define _SRVCONTROLLER_H_

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Mirrors firefly::core::Event::Kind. Plain enum so C services and zbus
 * message definitions never need to see any C++. */
enum firefly_evt {
	FIREFLY_EVT_RTC_ALARM = 0,
	FIREFLY_EVT_MOTION,
	FIREFLY_EVT_FIX_OK,
	FIREFLY_EVT_FIX_TIMEOUT,
	FIREFLY_EVT_REPORT_STORED,
	FIREFLY_EVT_SESSION_OK,
	FIREFLY_EVT_SESSION_TRANSIENT_FAIL,
	FIREFLY_EVT_SESSION_PERMANENT_FAIL,
	FIREFLY_EVT_POWER_LOW,
	FIREFLY_EVT_POWER_OK,
};

struct firefly_msg {
	uint8_t  evt;          /* enum firefly_evt */
	uint8_t  motion;       /* 0 still, 1 moving, 2 crash */
	uint32_t queue_depth;
};

void srvController_init(void);

/* Feeds one event to the mission FSM and executes whatever it returns.
 * Called from the controller thread on every zbus message. */
void srvController_dispatch(const struct firefly_msg *msg);

/* Current state, for logging and the integration tests. */
uint8_t srvController_state(void);

#ifdef __cplusplus
}
#endif
#endif /* _SRVCONTROLLER_H_ */
