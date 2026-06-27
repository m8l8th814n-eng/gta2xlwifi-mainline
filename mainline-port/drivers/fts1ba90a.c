// SPDX-License-Identifier: GPL-2.0-only
/*
 * STMicroelectronics FTS1BA90A capacitive touchscreen driver
 *
 * Minimal mainline driver for the FTS1BA90A touch controller as used on the
 * Samsung Galaxy Tab A 10.5 (2018) LTE (gta2xllte). The protocol (system
 * reset, ready handshake and the 8-byte coordinate event format) was derived
 * from Samsung's downstream "stm/fts1ba90a" driver; everything device-specific
 * to Android (factory test sysfs, sponge/AOD, firmware update, ESD recovery)
 * is intentionally omitted.
 *
 * The mainline stmfts driver does NOT work on this controller: FTS1BA90A uses
 * a different system-reset command (a write to system register 0x20000000),
 * which is why stmfts' SYSTEM_RESET times out with -110.
 */

#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/input/touchscreen.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/property.h>
#include <linux/regulator/consumer.h>

#define FTS_CMD_SENSE_ON		0x10
#define FTS_READ_DEVICE_ID		0x22
#define FTS_READ_ONE_EVENT		0x60

#define FTS_EVENT_SIZE			8
#define FTS_MAX_FINGERS			10
#define FTS_RETRY_COUNT			100

/* Chip id, returned by FTS_READ_DEVICE_ID in bytes [2] and [3] */
#define FTS_ID0				0x39
#define FTS_ID1				0x36

/* event[0] bits [1:0]: event id */
#define FTS_EV_COORDINATE		0
#define FTS_EV_STATUS			1

/* coordinate touch status (action) */
#define FTS_ACTION_NONE			0
#define FTS_ACTION_PRESS		1
#define FTS_ACTION_MOVE			2
#define FTS_ACTION_RELEASE		3

/* status event: event[0] stype field == INFORMATION, event[1] == READY */
#define FTS_STATUS_INFORMATION		2
#define FTS_INFO_READY			0x00

/* Bitfields within the 8-byte event, byte 0 */
#define FTS_EV0_EID			GENMASK(1, 0)
#define FTS_EV0_TID			GENMASK(5, 2)
#define FTS_EV0_TCHSTA			GENMASK(7, 6)
#define FTS_EV0_STYPE			GENMASK(5, 2)

struct fts1ba90a {
	struct i2c_client *client;
	struct input_dev *input;
	struct touchscreen_properties prop;
	struct regulator_bulk_data regulators[2];
};

/* Write a command byte (optionally with payload) to the controller. */
static int fts_write(struct fts1ba90a *ts, const u8 *cmd, int len)
{
	struct i2c_msg msg = {
		.addr = ts->client->addr,
		.flags = 0,
		.len = len,
		.buf = (u8 *)cmd,
	};
	int ret = i2c_transfer(ts->client->adapter, &msg, 1);

	return ret == 1 ? 0 : (ret < 0 ? ret : -EIO);
}

/* Write a 1-byte register address, then read len bytes back. */
static int fts_read(struct fts1ba90a *ts, u8 reg, u8 *buf, int len)
{
	struct i2c_msg msg[2] = {
		{ .addr = ts->client->addr, .flags = 0, .len = 1, .buf = &reg },
		{ .addr = ts->client->addr, .flags = I2C_M_RD, .len = len, .buf = buf },
	};
	int ret = i2c_transfer(ts->client->adapter, msg, 2);

	return ret == 2 ? 0 : (ret < 0 ? ret : -EIO);
}

static int fts_system_reset(struct fts1ba90a *ts)
{
	/* Write 0x2481 to system register 0x20000000 — the FTS1BA90A reset. */
	static const u8 reset_cmd[6] = { 0xfa, 0x20, 0x00, 0x00, 0x24, 0x81 };
	int ret = fts_write(ts, reset_cmd, sizeof(reset_cmd));

	if (ret)
		return ret;
	msleep(10);
	return 0;
}

static int fts_check_id(struct fts1ba90a *ts)
{
	u8 val[5];
	int ret = fts_read(ts, FTS_READ_DEVICE_ID, val, sizeof(val));

	if (ret)
		return ret;

	if (val[2] != FTS_ID0 || val[3] != FTS_ID1) {
		dev_err(&ts->client->dev,
			"unexpected chip id %02x %02x (want %02x %02x)\n",
			val[2], val[3], FTS_ID0, FTS_ID1);
		return -ENODEV;
	}
	return 0;
}

static int fts_wait_for_ready(struct fts1ba90a *ts)
{
	u8 data[FTS_EVENT_SIZE];
	int retry, ret;

	for (retry = 0; retry < FTS_RETRY_COUNT; retry++) {
		ret = fts_read(ts, FTS_READ_ONE_EVENT, data, FTS_EVENT_SIZE);
		if (ret)
			return ret;

		if (FIELD_GET(FTS_EV0_EID, data[0]) == FTS_EV_STATUS &&
		    FIELD_GET(FTS_EV0_STYPE, data[0]) == FTS_STATUS_INFORMATION &&
		    data[1] == FTS_INFO_READY)
			return 0;

		msleep(20);
	}

	dev_err(&ts->client->dev, "timed out waiting for ready\n");
	return -ETIMEDOUT;
}

static void fts_report_event(struct fts1ba90a *ts, const u8 *e)
{
	struct input_dev *input = ts->input;
	int id = FIELD_GET(FTS_EV0_TID, e[0]);
	int action = FIELD_GET(FTS_EV0_TCHSTA, e[0]);
	unsigned int x, y;

	if (id >= FTS_MAX_FINGERS)
		return;

	input_mt_slot(input, id);

	if (action == FTS_ACTION_RELEASE || action == FTS_ACTION_NONE) {
		input_mt_report_slot_inactive(input);
		return;
	}

	/* x = [x_11_4][x_3_0], y = [y_11_4][y_3_0] */
	x = (e[1] << 4) | (e[3] & 0x0f);
	y = (e[2] << 4) | (e[3] >> 4);

	input_mt_report_slot_state(input, MT_TOOL_FINGER, true);
	touchscreen_report_pos(input, &ts->prop, x, y, true);
	input_report_abs(input, ABS_MT_TOUCH_MAJOR, e[4]);
	input_report_abs(input, ABS_MT_TOUCH_MINOR, e[5]);
	input_report_abs(input, ABS_MT_PRESSURE, e[6] & 0x3f);
}

static irqreturn_t fts_irq(int irq, void *dev_id)
{
	struct fts1ba90a *ts = dev_id;
	u8 e[FTS_EVENT_SIZE];
	int left, guard = 0;

	do {
		if (fts_read(ts, FTS_READ_ONE_EVENT, e, FTS_EVENT_SIZE))
			break;

		left = e[7] & 0x3f;	/* left_event: events remaining after this */

		if (FIELD_GET(FTS_EV0_EID, e[0]) == FTS_EV_COORDINATE)
			fts_report_event(ts, e);
	} while (left > 0 && ++guard < FTS_MAX_FINGERS * 2);

	input_mt_sync_frame(ts->input);
	input_sync(ts->input);
	return IRQ_HANDLED;
}

static void fts_regulators_disable(void *data)
{
	struct fts1ba90a *ts = data;

	regulator_bulk_disable(ARRAY_SIZE(ts->regulators), ts->regulators);
}

static int fts_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct fts1ba90a *ts;
	int ret;

	ts = devm_kzalloc(dev, sizeof(*ts), GFP_KERNEL);
	if (!ts)
		return -ENOMEM;

	ts->client = client;
	i2c_set_clientdata(client, ts);

	ts->regulators[0].supply = "avdd";	/* 3.3V analog */
	ts->regulators[1].supply = "vdd";	/* 1.8V digital */
	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(ts->regulators),
				      ts->regulators);
	if (ret)
		return dev_err_probe(dev, ret, "failed to get regulators\n");

	ret = regulator_bulk_enable(ARRAY_SIZE(ts->regulators), ts->regulators);
	if (ret)
		return dev_err_probe(dev, ret, "failed to enable regulators\n");

	ret = devm_add_action_or_reset(dev, fts_regulators_disable, ts);
	if (ret)
		return ret;

	/* Let the analog/digital rails settle before talking to the IC. */
	msleep(20);

	ret = fts_system_reset(ts);
	if (ret)
		return dev_err_probe(dev, ret, "system reset failed\n");

	ret = fts_check_id(ts);
	if (ret)
		return ret;

	ret = fts_wait_for_ready(ts);
	if (ret)
		return ret;

	ts->input = devm_input_allocate_device(dev);
	if (!ts->input)
		return -ENOMEM;

	ts->input->name = "STMicroelectronics FTS1BA90A Touchscreen";
	ts->input->id.bustype = BUS_I2C;

	input_set_abs_params(ts->input, ABS_MT_POSITION_X, 0, 0, 0, 0);
	input_set_abs_params(ts->input, ABS_MT_POSITION_Y, 0, 0, 0, 0);
	input_set_abs_params(ts->input, ABS_MT_TOUCH_MAJOR, 0, 255, 0, 0);
	input_set_abs_params(ts->input, ABS_MT_TOUCH_MINOR, 0, 255, 0, 0);
	input_set_abs_params(ts->input, ABS_MT_PRESSURE, 0, 63, 0, 0);

	touchscreen_parse_properties(ts->input, true, &ts->prop);
	if (!ts->prop.max_x || !ts->prop.max_y)
		return dev_err_probe(dev, -EINVAL,
			"touchscreen-size-x/y must be set in the device tree\n");

	ret = input_mt_init_slots(ts->input, FTS_MAX_FINGERS,
				  INPUT_MT_DIRECT | INPUT_MT_DROP_UNUSED);
	if (ret)
		return ret;

	ret = devm_request_threaded_irq(dev, client->irq, NULL, fts_irq,
					IRQF_ONESHOT, "fts1ba90a", ts);
	if (ret)
		return dev_err_probe(dev, ret, "failed to request irq\n");

	/* Start scanning. */
	ret = fts_write(ts, (const u8[]){ FTS_CMD_SENSE_ON }, 1);
	if (ret)
		return dev_err_probe(dev, ret, "failed to enable sensing\n");

	return input_register_device(ts->input);
}

static const struct of_device_id fts1ba90a_of_match[] = {
	{ .compatible = "st,fts1ba90a" },
	{ }
};
MODULE_DEVICE_TABLE(of, fts1ba90a_of_match);

static const struct i2c_device_id fts1ba90a_id[] = {
	{ "fts1ba90a" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, fts1ba90a_id);

static struct i2c_driver fts1ba90a_driver = {
	.driver = {
		.name = "fts1ba90a",
		.of_match_table = fts1ba90a_of_match,
	},
	.probe = fts_probe,
	.id_table = fts1ba90a_id,
};
module_i2c_driver(fts1ba90a_driver);

MODULE_DESCRIPTION("STMicroelectronics FTS1BA90A touchscreen driver");
MODULE_LICENSE("GPL");
