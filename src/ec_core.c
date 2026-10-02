// SPDX-License-Identifier: GPL-2.0-or-later
/* Independent implementation of the documented ASUS table-2 protocol. */
#ifdef __KERNEL__
#include <linux/errno.h>
#else
#include <errno.h>
#endif
#include "ec_core.h"

#define EC_OPERATION_BUDGET_US 5000000ULL
#define EC_RECOVERY_BUDGET_US 2000000ULL
#define EC_STATUS_TIMEOUT_US 100000ULL
#define EC_WRITE_GAP_US 20000
#define EC_POLL_INTERVAL_US 100
#define EC_MAX_ATTEMPTS 3

static u64 ec_now_us(struct ec *ec)
{
	return ec->io->now_us(ec->ctx);
}

static int ec_pause(struct ec *ec, unsigned int delay, u64 end)
{
	u64 current = ec_now_us(ec);
	u64 remaining;

	if (current >= end)
		return -ETIMEDOUT;
	remaining = end - current;
	if (remaining < delay)
		delay = remaining;
	ec->io->sleep_us(ec->ctx, delay);
	return ec_now_us(ec) >= end ? -ETIMEDOUT : 0;
}

static int ec_wait_status(struct ec *ec, u8 mask, bool set, u64 end)
{
	u64 stop = ec_now_us(ec) + EC_STATUS_TIMEOUT_US;

	if (stop > end)
		stop = end;
	while (ec_now_us(ec) < stop) {
		if (!!(ec->io->in(ec->ctx, EC_CMD_PORT) & mask) == set)
			return 0;
		if (ec_pause(ec, EC_POLL_INTERVAL_US, stop))
			break;
	}
	return -ETIMEDOUT;
}

static int ec_drain_output(struct ec *ec, u64 end)
{
	u64 stop = ec_now_us(ec) + EC_STATUS_TIMEOUT_US;

	if (stop > end)
		stop = end;
	while (ec_now_us(ec) < stop) {
		if (!(ec->io->in(ec->ctx, EC_CMD_PORT) & EC_OBF))
			return 0;
		ec->io->in(ec->ctx, EC_DATA_PORT);
		if (ec_pause(ec, EC_POLL_INTERVAL_US, stop))
			break;
	}
	return -ETIMEDOUT;
}

static int ec_send_byte(struct ec *ec, unsigned int port, u8 byte, u64 end)
{
	int ret = ec_wait_status(ec, EC_IBF, false, end);

	if (!ret)
		ec->io->out(ec->ctx, port, byte);
	return ret;
}

static int ec_reg_xact(struct ec *ec, bool write, u8 reg, u8 *value, u64 end)
{
	int ret;

	/* No arbitrary register access, even through the internal transport. */
	if (write ? (reg != EC_MODE_REG && reg != EC_FAN_SELECT_REG &&
		     reg != EC_FAN_DUTY_REG) :
	    (reg != EC_FAN_COUNT_REG && reg != EC_MODE_REG && reg != EC_FAN_DUTY_REG))
		return -EINVAL;
	ret = ec_drain_output(ec, end);
	if (!ret)
		ret = ec_send_byte(ec, EC_CMD_PORT, EC_PREAMBLE, end);
	if (!ret)
		ret = ec_send_byte(ec, EC_CMD_PORT, EC_CMD, end);
	if (!ret)
		ret = ec_send_byte(ec, EC_DATA_PORT,
				   write ? EC_WRITE : EC_READ, end);
	if (!ret)
		ret = ec_send_byte(ec, EC_DATA_PORT, reg, end);
	if (!ret)
		ret = ec_send_byte(ec, EC_DATA_PORT, write ? *value : 0, end);
	if (!ret)
		ret = ec_wait_status(ec, EC_IBF, false, end);
	if (ret)
		return ret;
	if (write)
		return ec_pause(ec, EC_WRITE_GAP_US, end);
	ret = ec_wait_status(ec, EC_OBF, true, end);
	if (!ret)
		*value = ec->io->in(ec->ctx, EC_DATA_PORT);
	return ret;
}

static int ec_read_reg(struct ec *ec, u8 reg, u8 *value, u64 end)
{
	return ec_reg_xact(ec, false, reg, value, end);
}

static int ec_write_reg(struct ec *ec, u8 reg, u8 value, u64 end)
{
	return ec_reg_xact(ec, true, reg, &value, end);
}

static int ec_validate(struct ec *ec, u64 end)
{
	u8 count, mode;
	int ret = ec_read_reg(ec, EC_FAN_COUNT_REG, &count, end);

	if (ret)
		return ret;
	if (count != EC_FAN_COUNT)
		return -ENODEV;
	ret = ec_read_reg(ec, EC_MODE_REG, &mode, end);
	if (!ret && mode > EC_MODE_MANUAL)
		ret = -EIO;
	return ret;
}

static int ec_get_duty(struct ec *ec, int fan, u8 *value, u64 end)
{
	int ret = ec_write_reg(ec, EC_FAN_SELECT_REG, fan, end);

	if (!ret)
		ret = ec_read_reg(ec, EC_FAN_DUTY_REG, value, end);
	return ret;
}

static int ec_set_mode(struct ec *ec, u8 mode, u64 end)
{
	int attempt, ret = -EIO;

	for (attempt = 0; attempt < EC_MAX_ATTEMPTS && ec_now_us(ec) < end; attempt++) {
		ret = ec_validate(ec, end);
		if (!ret)
			ret = ec_write_reg(ec, EC_MODE_REG, mode, end);
		if (!ret)
			return 0;
	}
	return ret;
}

static int ec_set_duty(struct ec *ec, int fan, u8 duty, u64 end)
{
	int attempt, ret = -EIO;

	for (attempt = 0; attempt < EC_MAX_ATTEMPTS && ec_now_us(ec) < end; attempt++) {
		ret = ec_validate(ec, end);
		if (!ret)
			ret = ec_write_reg(ec, EC_FAN_SELECT_REG, fan, end);
		if (!ret)
			ret = ec_write_reg(ec, EC_MODE_REG, EC_MODE_MANUAL, end);
		if (!ret)
			ret = ec_write_reg(ec, EC_FAN_SELECT_REG, fan, end);
		if (!ret)
			ret = ec_write_reg(ec, EC_FAN_DUTY_REG, duty, end);
		if (!ret)
			return 0;
	}
	return ret;
}

static void ec_recover(struct ec *ec)
{
	int ret = ec_set_mode(ec, EC_MODE_AUTO, ec_now_us(ec) + EC_RECOVERY_BUDGET_US);

	if (ret && ec->io->recovery_error)
		ec->io->recovery_error(ec->ctx, ret);
}

static int ec_available(struct ec *ec)
{
	if (ec->removing)
		return -ENODEV;
	return ec->blocked ? -EBUSY : 0;
}

int ec_probe(struct ec *ec)
{
	return ec_set_mode(ec, EC_MODE_AUTO, ec_now_us(ec) + EC_OPERATION_BUDGET_US);
}

int ec_read_pwm(struct ec *ec, int fan, long *value)
{
	u8 duty;
	int ret = ec_available(ec);

	if (fan < 0 || fan >= EC_FAN_COUNT)
		return -EINVAL;
	if (!ret)
		ret = ec_get_duty(ec, fan, &duty, ec_now_us(ec) + EC_OPERATION_BUDGET_US);
	if (!ret)
		*value = duty;
	return ret;
}

int ec_read_enable(struct ec *ec, long *value)
{
	u8 mode;
	int ret = ec_available(ec);

	if (!ret)
		ret = ec_read_reg(ec, EC_MODE_REG, &mode,
				  ec_now_us(ec) + EC_OPERATION_BUDGET_US);
	if (!ret && mode > EC_MODE_MANUAL)
		ret = -EIO;
	if (!ret)
		*value = mode ? 1 : 2;
	return ret;
}

int ec_write_pwm(struct ec *ec, int fan, long value)
{
	int ret = ec_available(ec);

	if (fan < 0 || fan >= EC_FAN_COUNT || value < 0 || value > 255)
		return -EINVAL;
	if (ret)
		return ret;
	ret = ec_set_duty(ec, fan, value, ec_now_us(ec) + EC_OPERATION_BUDGET_US);
	if (ret)
		ec_recover(ec);
	return ret;
}

int ec_write_enable(struct ec *ec, long value)
{
	int ret = ec_available(ec);

	if (value != 1 && value != 2)
		return -EINVAL;
	if (ret)
		return ret;
	ret = ec_set_mode(ec, value == 1 ? EC_MODE_MANUAL : EC_MODE_AUTO,
			  ec_now_us(ec) + EC_OPERATION_BUDGET_US);
	if (ret)
		ec_recover(ec);
	return ret;
}

int ec_restore(struct ec *ec)
{
	u64 end = ec_now_us(ec) + EC_OPERATION_BUDGET_US;
	int ret;

	if (!ec->saved)
		return 0;
	ret = ec_validate(ec, end);
	if (!ret && ec->saved_mode) {
		ret = ec_set_duty(ec, 0, ec->saved_duty[0], end);
		if (!ret)
			ret = ec_set_duty(ec, 1, ec->saved_duty[1], end);
	} else if (!ret) {
		ret = ec_set_mode(ec, EC_MODE_AUTO, end);
	}
	if (ret)
		ec_recover(ec);
	ec->saved = false;
	ec->blocked = false;
	return ret;
}

int ec_prepare(struct ec *ec)
{
	u64 end = ec_now_us(ec) + EC_OPERATION_BUDGET_US;
	u8 mode, duty[EC_FAN_COUNT] = { 0, 0 };
	int ret = ec_available(ec);

	if (ret)
		return ret;
	ec->blocked = true;
	ret = ec_validate(ec, end);
	if (!ret)
		ret = ec_read_reg(ec, EC_MODE_REG, &mode, end);
	if (!ret && mode > EC_MODE_MANUAL)
		ret = -EIO;
	if (!ret && mode)
		ret = ec_get_duty(ec, 0, &duty[0], end);
	if (!ret && mode)
		ret = ec_get_duty(ec, 1, &duty[1], end);
	if (ret) {
		ec->blocked = false;
		return ret;
	}
	ec->saved_mode = mode;
	ec->saved_duty[0] = duty[0];
	ec->saved_duty[1] = duty[1];
	ec->saved = true;
	ret = ec_set_mode(ec, EC_MODE_AUTO, end);
	if (ret) {
		/* prepare failures receive no complete callback from the PM core. */
		int restore_ret = ec_restore(ec);

		if (restore_ret && ec->io->recovery_error)
			ec->io->recovery_error(ec->ctx, restore_ret);
	}
	return ret;
}

int ec_remove(struct ec *ec)
{
	ec->removing = true;
	ec->saved = false;
	return ec_set_mode(ec, EC_MODE_AUTO, ec_now_us(ec) + EC_RECOVERY_BUDGET_US);
}
