// SPDX-License-Identifier: GPL-2.0-or-later
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>

#include "ec_core.h"

struct byte {
	unsigned int port;
	u8 value;
};

struct simulator {
	u64 time;
	u64 busy_until;
	bool busy, no_output, stale_forever, ready;
	u8 count, mode, selected, duty[2], output;
	u8 payload[3];
	int phase, payload_count;
	int ignore_duty, ignore_mode, quantize;
	int fail_duty_transport, fail_mode_transport;
	bool permanent_duty_busy;
	int duty_writes[2], mode_writes, recovery_errors;
	bool fail_snapshot;
	struct byte bytes[200000];
	unsigned int byte_count;
};

static u8 sim_in(void *ctx, unsigned int port)
{
	struct simulator *s = ctx;

	if (port == EC_CMD_PORT) {
		if (s->busy || s->time < s->busy_until)
			return 2;
		return s->stale_forever || s->ready ? 1 : 0;
	}
	assert(port == EC_DATA_PORT);
	s->ready = false;
	return s->output;
}

static void sim_out(void *ctx, unsigned int port, u8 value)
{
	struct simulator *s = ctx;
	u8 reg, direction;

	assert(s->byte_count < sizeof(s->bytes) / sizeof(s->bytes[0]));
	s->bytes[s->byte_count++] = (struct byte){port, value};
	if (port == EC_CMD_PORT) {
		if (value == 0xff) {
			s->phase = 1;
			s->payload_count = 0;
		} else {
			assert(value == 0xdd && s->phase == 1);
			s->phase = 2;
		}
		return;
	}
	assert(port == EC_DATA_PORT && s->phase == 2);
	s->payload[s->payload_count++] = value;
	if (s->payload_count != 3)
		return;
	s->phase = 0;
	direction = s->payload[0];
	reg = s->payload[1];
	assert(direction == 0x02 || direction == 0x82);
	if (direction == 0x02) {
		assert(value == 0);
		switch (reg) {
		case EC_FAN_COUNT_REG: s->output = s->count; break;
		case EC_MODE_REG: s->output = s->mode; break;
		case EC_FAN_DUTY_REG: s->output = s->duty[s->selected]; break;
		default: assert(false);
		}
		s->ready = !s->no_output &&
			!(s->fail_snapshot && reg == EC_FAN_DUTY_REG);
	} else {
		switch (reg) {
		case EC_FAN_SELECT_REG:
			assert(value < 2);
			s->selected = value;
			break;
		case EC_MODE_REG:
			assert(value < 2);
			s->mode_writes++;
			if (s->ignore_mode != value)
				s->mode = value;
			/* Reproduce the selector reset observed in the reference. */
			s->selected = 0;
			if (s->fail_mode_transport == value)
				s->busy_until = s->time + 100001;
			break;
		case EC_FAN_DUTY_REG:
			s->duty_writes[s->selected]++;
			if (s->ignore_duty != s->selected)
				s->duty[s->selected] = value >= s->quantize ?
					value - s->quantize : 0;
			if (s->fail_duty_transport == s->selected) {
				s->busy_until = s->time + 100001;
				s->busy = s->permanent_duty_busy;
			}
			break;
		default: assert(false);
		}
	}
}

static u64 sim_now(void *ctx)
{
	return ((struct simulator *)ctx)->time;
}

static void sim_sleep(void *ctx, unsigned int delay)
{
	((struct simulator *)ctx)->time += delay;
	sched_yield();
}

static void sim_recovery(void *ctx, int error)
{
	assert(error < 0);
	((struct simulator *)ctx)->recovery_errors++;
}

static const struct ec_io sim_ops = {
	.in = sim_in, .out = sim_out, .now_us = sim_now,
	.sleep_us = sim_sleep, .recovery_error = sim_recovery,
};

static void init(struct simulator *s, struct ec *ec)
{
	memset(s, 0, sizeof(*s));
	memset(ec, 0, sizeof(*ec));
	s->count = 2;
	s->duty[0] = 90;
	s->duty[1] = 100;
	s->ignore_duty = -1;
	s->ignore_mode = -1;
	s->fail_duty_transport = -1;
	s->fail_mode_transport = -1;
	ec->io = &sim_ops;
	ec->ctx = s;
}

static void basics(struct simulator *s, struct ec *ec)
{
	long value;
	unsigned int before;
	const struct byte read_count[] = {
		{0x25d, 0xff}, {0x25d, 0xdd},
		{0x25c, 0x02}, {0x25c, 0x30}, {0x25c, 0},
	};

	/* Numeric wire expectations stay independent of production constants. */
	const struct byte write_duty[] = {
		{0x25d, 0xff}, {0x25d, 0xdd},
		{0x25c, 0x02}, {0x25c, 0x30}, {0x25c, 0},
		{0x25d, 0xff}, {0x25d, 0xdd},
		{0x25c, 0x02}, {0x25c, 0x31}, {0x25c, 0},
		{0x25d, 0xff}, {0x25d, 0xdd},
		{0x25c, 0x82}, {0x25c, 0x32}, {0x25c, 1},
		{0x25d, 0xff}, {0x25d, 0xdd},
		{0x25c, 0x82}, {0x25c, 0x31}, {0x25c, 1},
		{0x25d, 0xff}, {0x25d, 0xdd},
		{0x25c, 0x82}, {0x25c, 0x32}, {0x25c, 1},
		{0x25d, 0xff}, {0x25d, 0xdd},
		{0x25c, 0x82}, {0x25c, 0x35}, {0x25c, 192},
	};

	init(s, ec);
	s->mode = 1;
	assert(ec_probe(ec) == 0 && s->mode == 0);
	for (unsigned int i = 0; i < 5; i++) {
		assert(s->bytes[i].port == read_count[i].port);
		assert(s->bytes[i].value == read_count[i].value);
	}
	assert(ec_read_enable(ec, &value) == 0 && value == 2);
	before = s->byte_count;
	assert(ec_write_enable(ec, 0) == -EINVAL);
	assert(ec_write_enable(ec, 3) == -EINVAL);
	assert(ec_write_pwm(ec, 0, -1) == -EINVAL);
	assert(ec_write_pwm(ec, 0, 256) == -EINVAL);
	assert(ec_write_pwm(ec, 2, 100) == -EINVAL);
	assert(ec_read_pwm(ec, -1, &value) == -EINVAL);
	assert(s->byte_count == before);
	assert(ec_write_pwm(ec, 1, 192) == 0);
	/* Two preflight reads + four writes, with no post-write transactions. */
	assert(s->byte_count - before == 30);
	for (unsigned int i = 0; i < 30; i++) {
		assert(s->bytes[before + i].port == write_duty[i].port);
		assert(s->bytes[before + i].value == write_duty[i].value);
	}
	assert(s->mode == 1 && s->duty[0] == 90 && s->duty[1] == 192);
	assert(ec_read_enable(ec, &value) == 0 && value == 1);
	assert(ec_read_pwm(ec, 1, &value) == 0 && value == 192);
	assert(ec_write_pwm(ec, 0, 0) == 0 && s->duty[0] == 0);
	assert(ec_write_pwm(ec, 1, 255) == 0 && s->duty[1] == 255);
	assert(ec_write_enable(ec, 2) == 0 && s->mode == 0);
	assert(s->duty[0] == 0 && s->duty[1] == 255);
	assert(ec_write_enable(ec, 1) == 0 && s->mode == 1);
	s->quantize = 2;
	assert(ec_write_pwm(ec, 0, 140) == 0);
	assert(ec_read_pwm(ec, 0, &value) == 0 && value == 138);
}

static void faults(struct simulator *s, struct ec *ec)
{
	long value = 999;
	u64 started;

	init(s, ec);
	s->count = 3;
	assert(ec_probe(ec) == -ENODEV && s->mode_writes == 0);
	init(s, ec);
	s->mode = 7;
	assert(ec_probe(ec) == -EIO && s->mode_writes == 0);
	assert(ec_read_enable(ec, &value) == -EIO && value == 999);
	init(s, ec);
	s->no_output = true;
	assert(ec_read_pwm(ec, 0, &value) == -ETIMEDOUT && value == 999);
	init(s, ec);
	s->busy = true;
	started = s->time;
	assert(ec_write_pwm(ec, 0, 192) == -ETIMEDOUT);
	assert(s->time - started <= 7000000 && s->recovery_errors == 1);
	assert(s->byte_count == 0);
	init(s, ec);
	s->stale_forever = true;
	assert(ec_probe(ec) == -ETIMEDOUT && s->byte_count == 0);
	init(s, ec);
	s->busy_until = 120000;
	assert(ec_write_enable(ec, 1) == 0); /* second attempt succeeds */
	init(s, ec);
	s->ignore_duty = 1;
	assert(ec_write_pwm(ec, 1, 192) == 0);
	assert(s->duty_writes[1] == 1 && s->duty[1] == 100);
	assert(ec_read_pwm(ec, 1, &value) == 0 && value == 100);
	init(s, ec);
	s->ignore_mode = 1;
	assert(ec_write_enable(ec, 1) == 0 && s->mode == 0);
	assert(ec_read_enable(ec, &value) == 0 && value == 2);
	init(s, ec);
	s->fail_duty_transport = 1;
	assert(ec_write_pwm(ec, 1, 192) == -ETIMEDOUT);
	assert(s->duty_writes[1] == 3 && s->mode == 0);
	init(s, ec);
	s->fail_duty_transport = 1;
	s->permanent_duty_busy = true;
	assert(ec_write_pwm(ec, 1, 192) == -ETIMEDOUT);
	assert(s->mode == 1 && s->recovery_errors == 1);
	s->busy = false;
	assert(ec_read_enable(ec, &value) == 0 && value == 1);
}

static void power_management(struct simulator *s, struct ec *ec)
{
	long value;
	unsigned int before;

	init(s, ec);
	assert(ec_prepare(ec) == 0 && ec->blocked && ec->saved);
	assert(ec_read_enable(ec, &value) == -EBUSY);
	assert(ec_write_pwm(ec, 0, 128) == -EBUSY);
	assert(ec_restore(ec) == 0 && !ec->blocked && s->mode == 0);
	init(s, ec);
	s->mode = 1;
	s->duty[0] = 0;
	s->duty[1] = 192;
	assert(ec_prepare(ec) == 0 && s->mode == 0);
	s->duty[0] = 80;
	s->duty[1] = 90;
	assert(ec_restore(ec) == 0 && s->mode == 1);
	assert(s->duty[0] == 0 && s->duty[1] == 192);
	before = s->byte_count;
	assert(ec_restore(ec) == 0 && s->byte_count == before);
	/* complete alone restores a cancelled transition, without resume. */
	assert(ec_prepare(ec) == 0);
	assert(ec_restore(ec) == 0 && s->mode == 1);
	init(s, ec);
	s->mode = 1;
	s->fail_snapshot = true;
	assert(ec_prepare(ec) == -ETIMEDOUT);
	assert(!ec->blocked && !ec->saved && s->mode == 1);
	assert(s->mode_writes == 0);
	init(s, ec);
	s->mode = 1;
	s->fail_mode_transport = 0;
	assert(ec_prepare(ec) == -ETIMEDOUT);
	assert(!ec->blocked && !ec->saved && s->mode == 1);
	init(s, ec);
	s->mode = 1;
	s->duty[1] = 192;
	assert(ec_prepare(ec) == 0);
	s->duty[1] = 80;
	s->fail_duty_transport = 1;
	assert(ec_restore(ec) == -ETIMEDOUT && s->mode == 0);
	assert(!ec->saved && !ec->blocked);
	assert(ec_read_enable(ec, &value) == 0 && value == 2);
	init(s, ec);
	s->mode = 1;
	assert(ec_remove(ec) == 0 && s->mode == 0);
	assert(ec_write_pwm(ec, 0, 100) == -ENODEV);
	assert(ec_read_pwm(ec, 1, &value) == -ENODEV);
}

struct parallel {
	struct ec *ec;
	pthread_mutex_t lock;
};

static void *writer(void *arg)
{
	struct parallel *p = arg;

	for (int i = 0; i < 100; i++) {
		assert(pthread_mutex_lock(&p->lock) == 0);
		assert(ec_write_pwm(p->ec, i % 2, 128 + i % 2 * 64) == 0);
		assert(pthread_mutex_unlock(&p->lock) == 0);
	}
	return NULL;
}

static void *sleeper(void *arg)
{
	struct parallel *p = arg;
	long value;

	for (int i = 0; i < 100; i++) {
		assert(pthread_mutex_lock(&p->lock) == 0);
		assert(ec_prepare(p->ec) == 0);
		assert(pthread_mutex_unlock(&p->lock) == 0);
		sched_yield();
		assert(pthread_mutex_lock(&p->lock) == 0);
		assert(ec_read_pwm(p->ec, 0, &value) == -EBUSY);
		assert(ec_restore(p->ec) == 0);
		assert(pthread_mutex_unlock(&p->lock) == 0);
	}
	return NULL;
}

static void *conditional_writer(void *arg)
{
	struct parallel *p = arg;
	int ret;

	for (int i = 0; i < 100; i++) {
		assert(pthread_mutex_lock(&p->lock) == 0);
		ret = ec_write_pwm(p->ec, 1, 192);
		assert(ret == 0 || ret == -EBUSY);
		assert(pthread_mutex_unlock(&p->lock) == 0);
	}
	return NULL;
}

static void concurrency(struct simulator *s, struct ec *ec)
{
	struct parallel p = {.ec = ec, .lock = PTHREAD_MUTEX_INITIALIZER};
	pthread_t threads[3];

	init(s, ec);
	for (int i = 0; i < 3; i++)
		assert(pthread_create(&threads[i], NULL, writer, &p) == 0);
	for (int i = 0; i < 3; i++)
		assert(pthread_join(threads[i], NULL) == 0);
	assert(s->duty[0] == 128 && s->duty[1] == 192);
	assert(pthread_create(&threads[0], NULL, sleeper, &p) == 0);
	assert(pthread_create(&threads[1], NULL, conditional_writer, &p) == 0);
	assert(pthread_join(threads[0], NULL) == 0);
	assert(pthread_join(threads[1], NULL) == 0);
	assert(pthread_mutex_lock(&p.lock) == 0);
	assert(ec_remove(ec) == 0);
	assert(ec_write_pwm(ec, 0, 128) == -ENODEV);
	assert(pthread_mutex_unlock(&p.lock) == 0);
	assert(pthread_mutex_destroy(&p.lock) == 0);
}

int main(void)
{
	static struct simulator sim;
	struct ec ec;

	basics(&sim, &ec);
	faults(&sim, &ec);
	power_management(&sim, &ec);
	concurrency(&sim, &ec);
	puts("PASS: protocol, ABI values, faults, PM restore and serialized concurrency");
	return 0;
}
