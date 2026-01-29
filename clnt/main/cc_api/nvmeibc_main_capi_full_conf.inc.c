/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#pragma push_macro("__FILE_LITERAL__")
#undef __FILE_LITERAL__
#define __FILE_LITERAL__ nvmeibc_main_capi_full_conf_inc_c

static void __cc_api_full_conf_work(struct work_struct *w)
{
	struct delayed_work *dw = container_of(w, struct delayed_work, work);
	struct c_api_perrep *pr = container_of(dw, struct c_api_perrep, dwork);
	struct nvmeibc_control_api* cc_api = container_of(pr, struct nvmeibc_control_api, full_conf);
	int rv = __schedule_request_config(cc_api, REQUEST_ALL_VOLUMES, "Delayed full conf request");
	_ND(trace_cc_api_cc_api_full_conf_work, "Full conf request schedule ok_fail=@OK_FAIL", ((rv<0) ? "fail" : "ok"));
}

// Initializes the delayed workqueue and lock
void __full_conf_create(struct c_api_perrep* _this)
{	// Initial value, when adding to this value add by 1 second
	_this->delay_jiffies = 0;
	INIT_DELAYED_WORK(&_this->dwork, __cc_api_full_conf_work);
	spin_lock_init(&_this->lock);
}

void __full_conf_destroy(struct c_api_perrep* _this)
{
	cancel_delayed_work_sync(&_this->dwork);
}

void __full_conf_mark_received(struct nvmeibc_control_api* cc_api)
{
	unsigned long flags;
	_NT(trace_3_cc_api_parse_array_volume_conf, "full conf arrived OK, resetting delay to 0");
	spin_lock_irqsave(&cc_api->full_conf.lock, flags);
	cc_api->full_conf.delay_jiffies = 0;
	spin_unlock_irqrestore(&cc_api->full_conf.lock, flags);
}

void __full_conf_request_on_err(struct nvmeibc_control_api* cc_api)
{	// If there is no delay schedule the request now and increase the next request time
	unsigned long flags;
	spin_lock_irqsave(&cc_api->full_conf.lock, flags);
	if (!cc_api->full_conf.delay_jiffies) {
		_NT(trace_cc_api_request_full_conf, "Scheduling first full conf request increasing delay");
		cc_api->full_conf.delay_jiffies = HZ;
		spin_unlock_irqrestore(&cc_api->full_conf.lock, flags);
		__schedule_request_config(cc_api, REQUEST_ALL_VOLUMES, "MCS Error");
	} else {// If we already have a delay schedule the request with the delay time and multiple the delay
		const ulong delay_jiff = cc_api->full_conf.delay_jiffies;
		cc_api->full_conf.delay_jiffies *= 2;
		cc_api->full_conf.delay_jiffies  = min(cc_api->full_conf.delay_jiffies,
											 (unsigned long)600*HZ);
		spin_unlock_irqrestore(&cc_api->full_conf.lock, flags);
		_NT(trace_1_cc_api_request_full_conf, "Delayed full conf request to delay=@DELAY seconds", delay_jiff/HZ);
		schedule_delayed_work(&cc_api->full_conf.dwork, delay_jiff);
	}
}

#pragma pop_macro("__FILE_LITERAL__")
