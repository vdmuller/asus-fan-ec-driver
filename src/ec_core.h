/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ASUS_FAN_EC_CORE_H
#define ASUS_FAN_EC_CORE_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include "../tests/types.h"
#endif

#define EC_DATA_PORT 0x25c
/* Commands are written here; reads return buffer status. */
#define EC_CMD_PORT 0x25d
/* Output buffer full (OBF) and input buffer full (IBF) status bits. */
#define EC_OBF 0x01
#define EC_IBF 0x02
#define EC_PREAMBLE 0xff
#define EC_CMD 0xdd
#define EC_READ 0x02
#define EC_WRITE 0x82
#define EC_FAN_COUNT_REG 0x30
#define EC_MODE_REG 0x31
#define EC_FAN_SELECT_REG 0x32
#define EC_FAN_DUTY_REG 0x35
#define EC_MODE_AUTO 0
#define EC_MODE_MANUAL 1
#define EC_FAN_COUNT 2

/* All public core operations require the caller's device mutex. */
struct ec_io {
	u8 (*in)(void *ctx, unsigned int port);
	void (*out)(void *ctx, unsigned int port, u8 value);
	u64 (*now_us)(void *ctx);
	void (*sleep_us)(void *ctx, unsigned int delay);
	void (*recovery_error)(void *ctx, int error);
};

struct ec {
	const struct ec_io *io;
	void *ctx;
	bool blocked;
	bool removing;
	bool saved;
	u8 saved_mode;
	u8 saved_duty[EC_FAN_COUNT];
};

int ec_probe(struct ec *ec);
int ec_read_pwm(struct ec *ec, int fan, long *value);
int ec_read_enable(struct ec *ec, long *value);
int ec_write_pwm(struct ec *ec, int fan, long value);
int ec_write_enable(struct ec *ec, long value);
int ec_prepare(struct ec *ec);
int ec_restore(struct ec *ec);
int ec_remove(struct ec *ec);

#endif
