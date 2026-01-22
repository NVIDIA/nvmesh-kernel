/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/


#include "nvmeib_trace_stress_test.h"
#include "nvmeibp_trace.h"

struct nvmeib_trace_stress_prog_rpt {
	struct nvmeib_trace_stress_prog prog;
	struct list_head subprog;
	size_t times;
};

struct nvmeib_trace_stress_prog_sleep {
	struct nvmeib_trace_stress_prog prog;
	u64 usec;
};

struct nvmeib_trace_stress_prog_trace {
	struct nvmeib_trace_stress_prog prog;
	char text_payload[3000];
};

#define GOTONEXTLINE(ptr, len, off)                                            \
	({                                                                         \
		while ((off) < (len) && (ptr)[off] && (ptr)[off] != '\n' &&            \
		       (ptr)[off] != ':') {                                            \
			(off)++;                                                           \
		}                                                                      \
		while ((off) < (len) && ((ptr)[off] == '\n' || (ptr)[off] == ':' ||    \
		                         (ptr)[off] == ' ' || (ptr)[off] == '\t')) {   \
			(off)++;                                                           \
		}                                                                      \
		if ((off) < (len) && !(ptr)[off]) off = len;                           \
                                                                               \
		(len) - (off);                                                         \
	})

static struct nvmeib_trace_stress_prog *
__copy_gen(struct nvmeib_trace_stress_prog *prog) {
	struct nvmeib_trace_stress_prog *cp = kzalloc(sizeof(*cp), GFP_KERNEL);
	BUG_ON(!cp);
	*cp = *prog;
	return cp;
}

static struct nvmeib_trace_stress_prog *
__copy_rpt(struct nvmeib_trace_stress_prog *prog) {
	struct nvmeib_trace_stress_prog_rpt *cp = kzalloc(sizeof(*cp), GFP_KERNEL);
	struct nvmeib_trace_stress_prog *iter;
	BUG_ON(!cp);
	*cp = *(struct nvmeib_trace_stress_prog_rpt *)prog;
	INIT_LIST_HEAD(&cp->subprog);
	list_for_each_entry(
	    iter, &((struct nvmeib_trace_stress_prog_rpt *)prog)->subprog, link) {
		list_add_tail(&iter->copy(iter)->link, &cp->subprog);
	}
	return &cp->prog;
}

static struct nvmeib_trace_stress_prog *
__copy_sleep(struct nvmeib_trace_stress_prog *prog) {
	struct nvmeib_trace_stress_prog_sleep *cp =
	    kzalloc(sizeof(*cp), GFP_KERNEL);
	BUG_ON(!cp);
	*cp = *(struct nvmeib_trace_stress_prog_sleep *)prog;
	return &cp->prog;
}

static struct nvmeib_trace_stress_prog *
__copy_trace(struct nvmeib_trace_stress_prog *prog) {
	struct nvmeib_trace_stress_prog_trace *cp =
	    kzalloc(sizeof(*cp), GFP_KERNEL);
	BUG_ON(!cp);
	*cp = *(struct nvmeib_trace_stress_prog_trace *)prog;
	return &cp->prog;
}

static void __print_rpt(struct nvmeib_trace_stress_prog *prog_) {
	struct nvmeib_trace_stress_prog_rpt *prog = (void *)prog_;
	struct nvmeib_trace_stress_prog *iter;
	pr_info("> REPEAT %lu\n", prog->times);
	list_for_each_entry(iter, &prog->subprog, link) { iter->print(iter); }
	pr_info("> END\n");
}

static void __print_sleep(struct nvmeib_trace_stress_prog *prog_) {
	struct nvmeib_trace_stress_prog_sleep *prog = (void *)prog_;
	pr_info("> SLEEP %llu usec\n", prog->usec);
}

static void __print_trace(struct nvmeib_trace_stress_prog *prog_) {
	struct nvmeib_trace_stress_prog_trace *prog = (void *)prog_;
	pr_info("> TRACE payload='%s'\n", prog->text_payload);
}

static void __print_reset_count(struct nvmeib_trace_stress_prog *prog_) {
	pr_info("> RESET PERCPU COUNTER\n");
	(void)prog_;
}

static void __dtor_gen(struct nvmeib_trace_stress_prog *prog) { kfree(prog); }

static void __dtor_rpt(struct nvmeib_trace_stress_prog *prog_) {
	struct nvmeib_trace_stress_prog_rpt *prog = (void *)prog_;
	struct nvmeib_trace_stress_prog *iter, *tmp;
	list_for_each_entry_safe(iter, tmp, &prog->subprog, link) {
		list_del_init(&iter->link);
		iter->dtor(iter);
	}
}

static void __action_rpt(struct nvmeib_trace_stress_prog *prog_) {
	struct nvmeib_trace_stress_prog_rpt *prog = (void *)prog_;
	struct nvmeib_trace_stress_prog *iter;
	size_t i;
	for (i = 0; i < prog->times; ++i)
		list_for_each_entry(iter, &prog->subprog, link) { iter->action(iter); }
}

static void __action_sleep(struct nvmeib_trace_stress_prog *prog_) {
	struct nvmeib_trace_stress_prog_sleep *prog = (void *)prog_;
	cond_resched();
	udelay(prog->usec);
}

DEFINE_PER_CPU(u64, pcpu_count) = {0};

static void __action_trace(struct nvmeib_trace_stress_prog *prog_) {
	struct nvmeib_trace_stress_prog_trace *prog = (void *)prog_;
	NVMEIB_LOG_LONGTERM("STRESS TEST: {@INT64} payload=@STR",
	                    _E /*High priority*/, /*Default scope*/,
	                    stress_action_trace, get_cpu_var(pcpu_count)++,
	                    prog->text_payload);
	put_cpu();
}

static void __action_reset_count(struct nvmeib_trace_stress_prog *prog_) {
	get_cpu_var(pcpu_count) = 0;
	(void)prog_;
}

static struct nvmeib_trace_stress_prog *
nvmeib_trace_stress_prog_from_string(const char *str, size_t len, size_t *off) {
	char *buf;
	struct nvmeib_trace_stress_prog *prog = NULL;
	bool ok = false;
	const char *ptr = str;

	if (!(len)) return NULL;

	if (!(buf = kzalloc(3000, GFP_KERNEL))) goto out;

	/* Quick and dirty string parsing, no benefit in doing fancy stuff here.
	 */

	if (sscanf(ptr + *off, "%s", buf) != 1) { goto out; }
	if (!GOTONEXTLINE(ptr, len, *off)) goto out;

	if (!strcmp(buf, "REPEAT")) {
		/* Repeat prog */
		size_t times;
		struct nvmeib_trace_stress_prog_rpt *rpt_prog;
		if (sscanf(ptr + *off, "%lu", &times) != 1) goto out;
		if (!GOTONEXTLINE(ptr, len, *off)) goto out;
		if (!(prog = (void *)(rpt_prog = kzalloc(
		                          sizeof(struct nvmeib_trace_stress_prog_rpt),
		                          GFP_KERNEL))))
			goto out;
		INIT_LIST_HEAD(&rpt_prog->subprog);
		rpt_prog->times = times;
		rpt_prog->prog.action = __action_rpt;
		rpt_prog->prog.dtor = __dtor_rpt;
		rpt_prog->prog.copy = __copy_rpt;
		rpt_prog->prog.print = __print_rpt;
		while (1) {
			struct nvmeib_trace_stress_prog *subprog;
			if (sscanf(ptr + *off, "%s", buf) != 1) goto out;
			if (!strcmp(buf, "END")) {
				GOTONEXTLINE(ptr, len, *off);
				break;
			}
			if (!(subprog =
			          nvmeib_trace_stress_prog_from_string(ptr, len, off)))
				goto out;
			list_add_tail(&subprog->link, &rpt_prog->subprog);
		}
	} else if (!strcmp(buf, "SLEEP")) {
		u64 usec;
		struct nvmeib_trace_stress_prog_sleep *sleep_prog;
		if (sscanf(ptr + *off, "%llu", &usec) != 1) goto out;
		if (!(prog = (void *)(sleep_prog = kzalloc(
		                          sizeof(struct nvmeib_trace_stress_prog_sleep),
		                          GFP_KERNEL))))
			goto out;
		sleep_prog->usec = usec;
		sleep_prog->prog.action = __action_sleep;
		sleep_prog->prog.dtor = __dtor_gen;
		sleep_prog->prog.copy = __copy_sleep;
		sleep_prog->prog.print = __print_sleep;
		GOTONEXTLINE(ptr, len, *off);
	} else if (!strcmp(buf, "TRACE")) {
		struct nvmeib_trace_stress_prog_trace *trace_prog;
		if (!(prog = (void *)(trace_prog = kzalloc(
		                          sizeof(struct nvmeib_trace_stress_prog_trace),
		                          GFP_KERNEL))))
			goto out;
		if (sscanf(ptr + *off, "%s", trace_prog->text_payload) != 1) {
			kfree(prog);
			prog = NULL;
			goto out;
		}
		trace_prog->prog.action = __action_trace;
		trace_prog->prog.dtor = __dtor_gen;
		trace_prog->prog.copy = __copy_trace;
		trace_prog->prog.print = __print_trace;
		GOTONEXTLINE(ptr, len, *off);
	} else if (!strcmp(buf, "RESET_COUNT")) {
		struct nvmeib_trace_stress_prog *reset_prog;
		if (!(prog = (void *)(reset_prog = kzalloc(
		                          sizeof(struct nvmeib_trace_stress_prog),
		                          GFP_KERNEL))))
			goto out;
		reset_prog->action = __action_reset_count;
		reset_prog->dtor = __dtor_gen;
		reset_prog->copy = __copy_gen;
		reset_prog->print = __print_reset_count;
	} else {
		goto out;
	}
	ok = true;
out:
	if (buf) kfree(buf);
	if (!ok && prog) {
		prog->dtor(prog);
		ok = false;
		prog = NULL;
	}
	return prog;
}

struct nvmeib_trace_stress_prog *
nvmeib_trace_stress_cpu_and_prog_from_string(const char *str, size_t len,
                                             size_t *off, size_t *cpu_start,
                                             size_t *cpu_end) {
	const char *ptr = str;
	*cpu_start = *cpu_end = -1;
	if (sscanf(ptr + *off, "CPU %lu to %lu", cpu_start, cpu_end) != 2)
		return NULL;
	GOTONEXTLINE(ptr, len, *off);
	return nvmeib_trace_stress_prog_from_string(ptr, len, off);
}
