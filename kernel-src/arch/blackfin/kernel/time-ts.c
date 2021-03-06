/*
 * linux/arch/kernel/time-ts.c
 *
 * Based on arm clockevents implementation and old bfin time tick.
 *
 * Copyright(C) 2008, GeoTechnologies, Vitja Makarov
 *
 * This code is licenced under the GPL version 2. For details see
 * kernel-base/COPYING.
 */
#include <linux/module.h>
#include <linux/profile.h>
#include <linux/interrupt.h>
#include <linux/time.h>
#include <linux/timex.h>
#include <linux/irq.h>
#include <linux/clocksource.h>
#include <linux/clockchips.h>
#include <linux/cpufreq.h>

#include <asm/blackfin.h>
#include <asm/time.h>

#ifdef CONFIG_CYCLES_CLOCKSOURCE

/* Accelerators for sched_clock()
 * convert from cycles(64bits) => nanoseconds (64bits)
 *  basic equation:
 *		ns = cycles / (freq / ns_per_sec)
 *		ns = cycles * (ns_per_sec / freq)
 *		ns = cycles * (10^9 / (cpu_khz * 10^3))
 *		ns = cycles * (10^6 / cpu_khz)
 *
 *	Then we use scaling math (suggested by george@mvista.com) to get:
 *		ns = cycles * (10^6 * SC / cpu_khz) / SC
 *		ns = cycles * cyc2ns_scale / SC
 *
 *	And since SC is a constant power of two, we can convert the div
 *  into a shift.
 *
 *  We can use khz divisor instead of mhz to keep a better precision, since
 *  cyc2ns_scale is limited to 10^6 * 2^10, which fits in 32 bits.
 *  (mathieu.desnoyers@polymtl.ca)
 *
 *			-johnstul@us.ibm.com "math is hard, lets go shopping!"
 */

static unsigned long cyc2ns_scale;
#define CYC2NS_SCALE_FACTOR 10 /* 2^10, carefully chosen */

static inline void set_cyc2ns_scale(unsigned long cpu_khz)
{
	cyc2ns_scale = (1000000 << CYC2NS_SCALE_FACTOR) / cpu_khz;
}

static inline unsigned long long cycles_2_ns(cycle_t cyc)
{
	return (cyc * cyc2ns_scale) >> CYC2NS_SCALE_FACTOR;
}

static cycle_t read_cycles(void)
{
	return __bfin_cycles_off + (get_cycles() << __bfin_cycles_mod);
}

unsigned long long sched_clock(void)
{
	return cycles_2_ns(read_cycles());
}

static struct clocksource clocksource_bfin = {
	.name		= "bfin_cycles",
	.rating		= 350,
	.read		= read_cycles,
	.mask		= CLOCKSOURCE_MASK(64),
	.shift		= 22,
	.flags		= CLOCK_SOURCE_IS_CONTINUOUS,
};

static int __init bfin_clocksource_init(void)
{
	set_cyc2ns_scale(get_cclk() / 1000);

	clocksource_bfin.mult = clocksource_hz2mult(get_cclk(), clocksource_bfin.shift);

	if (clocksource_register(&clocksource_bfin))
		panic("failed to register clocksource");

	return 0;
}

#else
# define bfin_clocksource_init()
#endif

static int bfin_timer_set_next_event(unsigned long cycles,
                                     struct clock_event_device *evt)
{
	bfin_write_TCOUNT(cycles);
	CSYNC();
	return 0;
}

static void bfin_timer_set_mode(enum clock_event_mode mode,
                                struct clock_event_device *evt)
{
	switch (mode) {
	case CLOCK_EVT_MODE_PERIODIC: {
		unsigned long tcount = ((get_cclk() / (HZ * TIME_SCALE)) - 1);
		bfin_write_TCNTL(TMPWR);
		bfin_write_TSCALE(TIME_SCALE - 1);
		CSYNC();
		bfin_write_TPERIOD(tcount);
		bfin_write_TCOUNT(tcount);
		bfin_write_TCNTL(TMPWR | TMREN | TAUTORLD);
		CSYNC();
		break;
	}
	case CLOCK_EVT_MODE_ONESHOT:
		bfin_write_TSCALE(TIME_SCALE - 1);
		bfin_write_TCOUNT(0);
		bfin_write_TCNTL(TMPWR | TMREN);
		CSYNC();
		break;
	case CLOCK_EVT_MODE_UNUSED:
	case CLOCK_EVT_MODE_SHUTDOWN:
		bfin_write_TCNTL(0);
		CSYNC();
		break;
	case CLOCK_EVT_MODE_RESUME:
		break;
	}
}

static void __init bfin_timer_init(void)
{
	/* power up the timer, but don't enable it just yet */
	bfin_write_TCNTL(TMPWR);
	CSYNC();

	/*
	 * the TSCALE prescaler counter.
	 */
	bfin_write_TSCALE(TIME_SCALE - 1);
	bfin_write_TPERIOD(0);
	bfin_write_TCOUNT(0);

	/* now enable the timer */
	CSYNC();
}

/*
 * timer_interrupt() needs to keep up the real-time clock,
 * as well as call the "do_timer()" routine every clocktick
 */
#ifdef CONFIG_CORE_TIMER_IRQ_L1
__attribute__((l1_text))
#endif
irqreturn_t timer_interrupt(int irq, void *dev_id);

static struct clock_event_device clockevent_bfin = {
	.name		= "bfin_core_timer",
	.features	= CLOCK_EVT_FEAT_PERIODIC | CLOCK_EVT_FEAT_ONESHOT,
	.shift		= 32,
	.set_next_event = bfin_timer_set_next_event,
	.set_mode	= bfin_timer_set_mode,
};

static struct irqaction bfin_timer_irq = {
	.name		= "Blackfin Core Timer",
	.flags		= IRQF_DISABLED | IRQF_TIMER | IRQF_IRQPOLL,
	.handler	= timer_interrupt,
	.dev_id		= &clockevent_bfin,
};

irqreturn_t timer_interrupt(int irq, void *dev_id)
{
	struct clock_event_device *evt = dev_id;
	evt->event_handler(evt);
	return IRQ_HANDLED;
}

static int __init bfin_clockevent_init(void)
{
	unsigned long timer_clk;

	timer_clk = get_cclk() / TIME_SCALE;

	setup_irq(IRQ_CORETMR, &bfin_timer_irq);
	bfin_timer_init();

	clockevent_bfin.mult = div_sc(timer_clk, NSEC_PER_SEC, clockevent_bfin.shift);
	clockevent_bfin.max_delta_ns = clockevent_delta2ns(-1, &clockevent_bfin);
	clockevent_bfin.min_delta_ns = clockevent_delta2ns(100, &clockevent_bfin);
	clockevent_bfin.cpumask = cpumask_of(0);
	clockevents_register_device(&clockevent_bfin);

	return 0;
}

void __init time_init(void)
{
	time_t secs_since_1970 = (365 * 37 + 9) * 24 * 60 * 60;	/* 1 Jan 2007 */

#ifdef CONFIG_RTC_DRV_BFIN
	/* [#2663] hack to filter junk RTC values that would cause
	 * userspace to have to deal with time values greater than
	 * 2^31 seconds (which uClibc cannot cope with yet)
	 */
	if ((bfin_read_RTC_STAT() & 0xC0000000) == 0xC0000000) {
		printk(KERN_NOTICE "bfin-rtc: invalid date; resetting\n");
		bfin_write_RTC_STAT(0);
	}
#endif

	/* Initialize xtime. From now on, xtime is updated with timer interrupts */
	xtime.tv_sec = secs_since_1970;
	xtime.tv_nsec = 0;
	set_normalized_timespec(&wall_to_monotonic, -xtime.tv_sec, -xtime.tv_nsec);

	bfin_clocksource_init();
	bfin_clockevent_init();
}
