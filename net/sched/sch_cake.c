// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause

/* COMMON Applications Kept Enhanced (CAKE) discipline
 *
 * Copyright (C) 2014-2018 Jonathan Morton <chromatix99@gmail.com>
 * Copyright (C) 2015-2018 Toke Høiland-Jørgensen <toke@toke.dk>
 * Copyright (C) 2014-2018 Dave Täht <dave.taht@gmail.com>
 * Copyright (C) 2015-2018 Sebastian Moeller <moeller0@gmx.de>
 * (C) 2015-2018 Kevin Darbyshire-Bryant <kevin@darbyshire-bryant.me.uk>
 * Copyright (C) 2017-2018 Ryan Mounce <ryan@mounce.com.au>
 *
 * The CAKE Principles:
 *		   (or, how to have your cake and eat it too)
 *
 * This is a combination of several shaping, AQM and FQ techniques into one
 * easy-to-use package:
 *
 * - An overall bandwidth shaper, to move the bottleneck away from dumb CPE
 *   equipment and bloated MACs.  This operates in deficit mode (as in sch_fq),
 *   eliminating the need for any sort of burst parameter (eg. token bucket
 *   depth).  Burst support is limited to that necessary to overcome scheduling
 *   latency.
 *
 * - A Diffserv-aware priority queue, giving more priority to certain classes,
 *   up to a specified fraction of bandwidth.  Above that bandwidth threshold,
 *   the priority is reduced to avoid starving other tins.
 *
 * - Each priority tin has a separate Flow Queue system, to isolate traffic
 *   flows from each other.  This prevents a burst on one flow from increasing
 *   the delay to another.  Flows are distributed to queues using a
 *   set-associative hash function.
 *
 * - Each queue is actively managed by Cobalt, which is a combination of the
 *   Codel and Blue AQM algorithms.  This serves flows fairly, and signals
 *   congestion early via ECN (if available) and/or packet drops, to keep
 *   latency low.  The codel parameters are auto-tuned based on the bandwidth
 *   setting, as is necessary at low bandwidths.
 *
 * The configuration parameters are kept deliberately simple for ease of use.
 * Everything has sane defaults.  Complete generality of configuration is *not*
 * a goal.
 *
 * The priority queue operates according to a weighted DRR scheme, combined with
 * a bandwidth tracker which reuses the shaper logic to detect which side of the
 * bandwidth sharing threshold the tin is operating.  This determines whether a
 * priority-based weight (high) or a bandwidth-based weight (low) is used for
 * that tin in the current pass.
 *
 * This qdisc was inspired by Eric Dumazet's fq_codel code, which he kindly
 * granted us permission to leverage.
 */

#include <linux/module.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/jiffies.h>
#include <linux/string.h>
#include <linux/in.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/skbuff.h>
#include <linux/jhash.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/reciprocal_div.h>
#include <net/netlink.h>
#include <linux/version.h>
#include <linux/if_vlan.h>
#include <net/pkt_sched.h>
#include <net/pkt_cls.h>
#include <net/tcp.h>
#include <net/flow_dissector.h>

#define CAKE_SET_WAYS (8)
#define CAKE_MAX_TINS (8)
#define CAKE_QUEUES (1024)
#define CAKE_FLOW_MASK 63
#define CAKE_FLOW_NAT_FLAG 64

/* struct cobalt_params - contains codel and blue parameters
 * @interval:	codel initial drop rate
 * @target:     maximum persistent sojourn time & blue update rate
 * @mtu_time:   serialisation delay of maximum-size packet
 * @p_inc:      increment of blue drop probability (0.32 fxp)
 * @p_dec:      decrement of blue drop probability (0.32 fxp)
 */
struct cobalt_params {
	u64	interval;
	u64	target;
	u64	mtu_time;
	u32	p_inc;
	u32	p_dec;
};

/* struct cobalt_vars - contains codel and blue variables
 * @count:		codel dropping frequency
 * @rec_inv_sqrt:	reciprocal value of sqrt(count) >> 1
 * @drop_next:		time to drop next packet, or when we dropped last
 * @blue_timer:		Blue time to next drop
 * @p_drop:		BLUE drop probability (0.32 fxp)
 * @dropping:		set if in dropping state
 * @ecn_marked:		set if marked
 */
struct cobalt_vars {
	u32	count;
	u32	rec_inv_sqrt;
	ktime_t	drop_next;
	ktime_t	blue_timer;
	u32     p_drop;
	bool	dropping;
	bool    ecn_marked;
};

enum {
	CAKE_SET_NONE = 0,
	CAKE_SET_SPARSE,
	CAKE_SET_SPARSE_WAIT, /* counted in SPARSE, actually in BULK */
	CAKE_SET_BULK,
	CAKE_SET_DECAYING
};

struct cake_flow {
	/* this stuff is all needed per-flow at dequeue time */
	struct sk_buff	  *head;
	struct sk_buff	  *tail;
	struct list_head  flowchain;
	s32		  deficit;
	u32		  dropped;
	struct cobalt_vars cvars;
	u16		  srchost; /* index into cake_host table */
	u16		  dsthost;
	u8		  set;
}; /* please try to keep this structure <= 64 bytes */

struct cake_host {
	u32 srchost_tag;
	u32 dsthost_tag;
	u16 srchost_refcnt;
	u16 dsthost_refcnt;
};

struct cake_heap_entry {
	u16 t:3, b:10;
};

struct cake_tin_data {
	struct cake_flow flows[CAKE_QUEUES];
	u32	backlogs[CAKE_QUEUES];
	u32	tags[CAKE_QUEUES]; /* for set association */
	u16	overflow_idx[CAKE_QUEUES];
	struct cake_host hosts[CAKE_QUEUES]; /* for triple isolation */
	u16	flow_quantum;

	struct cobalt_params cparams;
	u32	drop_overlimit;
	u16	bulk_flow_count;
	u16	sparse_flow_count;
	u16	decaying_flow_count;
	u16	unresponsive_flow_count;

	u32	max_skblen;

	struct list_head new_flows;
	struct list_head old_flows;
	struct list_head decaying_flows;

	/* time_next = time_this + ((len * rate_ns) >> rate_shft) */
	ktime_t	time_next_packet;
	u64	tin_rate_ns;
	u64	tin_rate_bps;
	u16	tin_rate_shft;

	u16	tin_quantum_prio;
	u16	tin_quantum_band;
	s32	tin_deficit;
	u32	tin_backlog;
	u32	tin_dropped;
	u32	tin_ecn_mark;

	u32	packets;
	u64	bytes;

	u32	ack_drops;

	/* moving averages */
	u64 avge_delay;
	u64 peak_delay;
	u64 base_delay;

	/* hash function stats */
	u32	way_directs;
	u32	way_hits;
	u32	way_misses;
	u32	way_collisions;
}; /* number of tins is small, so size of this struct doesn't matter much */

struct cake_sched_data {
	struct tcf_proto __rcu *filter_list; /* optional external classifier */
	struct tcf_block *block;
	struct cake_tin_data *tins;

	struct cake_heap_entry overflow_heap[CAKE_QUEUES * CAKE_MAX_TINS];
	u16		overflow_timeout;

	u16		tin_cnt;
	u8		tin_mode;
	u8		flow_mode;
	u8		ack_filter;
	u8		atm_mode;

	/* time_next = time_this + ((len * rate_ns) >> rate_shft) */
	u16		rate_shft;
	ktime_t		time_next_packet;
	ktime_t		failsafe_next_packet;
	u64		rate_ns;
	u64		rate_bps;
	u16		rate_flags;
	s16		rate_overhead;
	u16		rate_mpu;
	u64		interval;
	u64		target;

	/* resource tracking */
	u32		buffer_used;
	u32		buffer_max_used;
	u32		buffer_limit;
	u32		buffer_config_limit;

	/* indices for dequeue */
	u16		cur_tin;
	u16		cur_flow;

	struct qdisc_watchdog watchdog;
	const u8	*tin_index;
	const u8	*tin_order;

	/* bandwidth capacity estimate */
	ktime_t		last_packet_time;
	ktime_t		avg_window_begin;
	u64		avg_packet_interval;
	u64		avg_window_bytes;
	u64		avg_peak_bandwidth;
	ktime_t		last_reconfig_time;

	/* packet length stats */
	u32		avg_netoff;
	u16		max_netlen;
	u16		max_adjlen;
	u16		min_netlen;
	u16		min_adjlen;
};

enum {
	CAKE_FLAG_OVERHEAD	   = BIT(0),
	CAKE_FLAG_AUTORATE_INGRESS = BIT(1),
	CAKE_FLAG_INGRESS	   = BIT(2),
	CAKE_FLAG_WASH		   = BIT(3),
	CAKE_FLAG_SPLIT_GSO	   = BIT(4)
};

/* COBALT operates the Codel and BLUE algorithms in parallel, in order to
 * obtain the best features of each.  Codel is excellent on flows which
 * respond to congestion signals in a TCP-like way.  BLUE is more effective on
 * unresponsive flows.
 */

struct cobalt_skb_cb {
	ktime_t enqueue_time;
};

static u64 us_to_ns(u64 us)
{
	return us * NSEC_PER_USEC;
}

static struct cobalt_skb_cb *get_cobalt_cb(const struct sk_buff *skb)
{
	qdisc_cb_private_validate(skb, sizeof(struct cobalt_skb_cb));
	return (struct cobalt_skb_cb *)qdisc_skb_cb(skb)->data;
}

static ktime_t cobalt_get_enqueue_time(const struct sk_buff *skb)
{
	return get_cobalt_cb(skb)->enqueue_time;
}

static void cobalt_set_enqueue_time(struct sk_buff *skb,
				    ktime_t now)
{
	get_cobalt_cb(skb)->enqueue_time = now;
}

static u16 quantum_div[CAKE_QUEUES + 1] = {0};

#define REC_INV_SQRT_CACHE (16)
static u32 cobalt_rec_inv_sqrt_cache[REC_INV_SQRT_CACHE] = {0};

/* http://en.wikipedia.org/wiki/Methods_of_computing_square_roots
 * new_invsqrt = (invsqrt / 2) * (3 - count * invsqrt^2)
 *
 * Here, invsqrt is a fixed point number (< 1.0), 32bit mantissa, aka Q0.32
 */

static void cobalt_newton_step(struct cobalt_vars *vars)
{
	u32 invsqrt, invsqrt2;
	u64 val;

	invsqrt = vars->rec_inv_sqrt;
	invsqrt2 = ((u64)invsqrt * invsqrt) >> 32;
	val = (3LL << 32) - ((u64)vars->count * invsqrt2);

	val >>= 2; /* avoid overflow in following multiply */
	val = (val * invsqrt) >> (32 - 2 + 1);

	vars->rec_inv_sqrt = val;
}

static void cobalt_invsqrt(struct cobalt_vars *vars)
{
	if (vars->count < REC_INV_SQRT_CACHE)
		vars->rec_inv_sqrt = cobalt_rec_inv_sqrt_cache[vars->count];
	else
		cobalt_newton_step(vars);
}

/* There is a big difference in timing between the accurate values placed in
 * the cache and the approximations given by a single Newton step for small
 * count values, particularly when stepping from count 1 to 2 or vice versa.
 * Above 16, a single Newton step gives sufficient accuracy in either
 * direction, given the precision stored.
 *
 * The magnitude of the error when stepping up to count 2 is such as to give
 * the value that *should* have been produced at count 4.
 */

static void cobalt_cache_init(void)
{
	struct cobalt_vars v;

	memset(&v, 0, sizeof(v));
	v.rec_inv_sqrt = ~0U;
	cobalt_rec_inv_sqrt_cache[0] = v.rec_inv_sqrt;

	for (v.count = 1; v.count < REC_INV_SQRT_CACHE; v.count++) {
		cobalt_newton_step(&v);
		cobalt_newton_step(&v);
		cobalt_newton_step(&v);
		cobalt_newton_step(&v);

		cobalt_rec_inv_sqrt_cache[v.count] = v.rec_inv_sqrt;
	}
}

static void cobalt_vars_init(struct cobalt_vars *vars)
{
	memset(vars, 0, sizeof(*vars));

	if (!cobalt_rec_inv_sqrt_cache[0]) {
		cobalt_cache_init();
		cobalt_rec_inv_sqrt_cache[0] = ~0;
	}
}

/* CoDel control_law is t + interval/sqrt(count)
 * We maintain in rec_inv_sqrt the reciprocal value of sqrt(count) to avoid
 * both sqrt() and divide operation.
 */
static ktime_t cobalt_control(ktime_t t,
			      u64 interval,
			      u32 rec_inv_sqrt)
{
	return ktime_add_ns(t, reciprocal_scale(interval,
						rec_inv_sqrt));
}

/* Call this when a packet had to be dropped due to queue overflow.  Returns
 * true if the BLUE state was quiescent before but active after this call.
 */
static bool cobalt_queue_full(struct cobalt_vars *vars,
			      struct cobalt_params *p,
			      ktime_t now)
{
	bool up = false;

	if (ktime_to_ns(ktime_sub(now, vars->blue_timer)) > p->target) {
		up = !vars->p_drop;
		vars->p_drop += p->p_inc;
		if (vars->p_drop < p->p_inc)
			vars->p_drop = ~0;
		vars->blue_timer = now;
	}
	vars->dropping = true;
	vars->drop_next = now;
	if (!vars->count)
		vars->count = 1;

	return up;
}

/* Call this when the queue was serviced but turned out to be empty.  Returns
 * true if the BLUE state was active before but quiescent after this call.
 */
static bool cobalt_queue_empty(struct cobalt_vars *vars,
			       struct cobalt_params *p,
			       ktime_t now)
{
	bool down = false;

	if (vars->p_drop &&
	    ktime_to_ns(ktime_sub(now, vars->blue_timer)) > p->target) {
		if (vars->p_drop < p->p_dec)
			vars->p_drop = 0;
		else
			vars->p_drop -= p->p_dec;
		vars->blue_timer = now;
		down = !vars->p_drop;
	}
	vars->dropping = false;

	if (vars->count && ktime_to_ns(ktime_sub(now, vars->drop_next)) >= 0) {
		vars->count--;
		cobalt_invsqrt(vars);
		vars->drop_next = cobalt_control(vars->drop_next,
						 p->interval,
						 vars->rec_inv_sqrt);
	}

	return down;
}

/* Call this with a freshly dequeued packet for possible congestion marking.
 * Returns true as an instruction to drop the packet, false for delivery.
 */
static bool cobalt_should_drop(struct cobalt_vars *vars,
			       struct cobalt_params *p,
			       ktime_t now,
			       struct sk_buff *skb,
			       u32 bulk_flows)
{
	bool next_due, over_target, drop = false;
	ktime_t schedule;
	u64 sojourn;

/* The 'schedule' variable records, in its sign, whether 'now' is before or
 * after 'drop_next'.  This allows 'drop_next' to be updated before the next
 * scheduling decision is actually branched, without destroying that
 * information.  Similarly, the first 'schedule' value calculated is preserved
 * in the boolean 'next_due'.
 *
 * As for 'drop_next', we take advantage of the fact that 'interval' is both
 * the delay between first exceeding 'target' and the first signalling event,
 * *and* the scaling factor for the signalling frequency.  It's therefore very
 * natural to use a single mechanism for both purposes, and eliminates a
 * significant amount of reference Codel's spaghetti code.  To help with this,
 * both the '0' and '1' entries in the invsqrt cache are 0xFFFFFFFF, as close
 * as possible to 1.0 in fixed-point.
 */

	sojourn = ktime_to_ns(ktime_sub(now, cobalt_get_enqueue_time(skb)));
	schedule = ktime_sub(now, vars->drop_next);
	over_target = sojourn > p->target &&
		      sojourn > p->mtu_time * bulk_flows * 2 &&
		      sojourn > p->mtu_time * 4;
	next_due = vars->count && ktime_to_ns(schedule) >= 0;

	vars->ecn_marked = false;

	if (over_target) {
		if (!vars->dropping) {
			vars->dropping = true;
			vars->drop_next = cobalt_control(now,
							 p->interval,
							 vars->rec_inv_sqrt);
		}
		if (!vars->count)
			vars->count = 1;
	} else if (vars->dropping) {
		vars->dropping = false;
	}

	if (next_due && vars->dropping) {
		/* Use ECN mark if possible, otherwise drop */
		drop = !(vars->ecn_marked = INET_ECN_set_ce(skb));

		vars->count++;
		if (!vars->count)
			vars->count--;
		cobalt_invsqrt(vars);
		vars->drop_next = cobalt_control(vars->drop_next,
						 p->interval,
						 vars->rec_inv_sqrt);
		schedule = ktime_sub(now, vars->drop_next);
	} else {
		while (next_due) {
			vars->count--;
			cobalt_invsqrt(vars);
			vars->drop_next = cobalt_control(vars->drop_next,
							 p->interval,
							 vars->rec_inv_sqrt);
			schedule = ktime_sub(now, vars->drop_next);
			next_due = vars->count && ktime_to_ns(schedule) >= 0;
		}
	}

	/* Simple BLUE implementation.  Lack of ECN is deliberate. */
	if (vars->p_drop)
		drop |= (prandom_u32() < vars->p_drop);

	/* Overload the drop_next field as an activity timeout */
	if (!vars->count)
		vars->drop_next = ktime_add_ns(now, p->interval);
	else if (ktime_to_ns(schedule) > 0 && !drop)
		vars->drop_next = now;

	return drop;
}

/* Cake has several subtle multiple bit settings. In these cases you
 *  would be matching triple isolate mode as well.
 */

static bool cake_dsrc(int flow_mode)
{
	return (flow_mode & CAKE_FLOW_DUAL_SRC) == CAKE_FLOW_DUAL_SRC;
}

static bool cake_ddst(int flow_mode)
{
	return (flow_mode & CAKE_FLOW_DUAL_DST) == CAKE_FLOW_DUAL_DST;
}

static u32 cake_hash(struct cake_tin_data *q, const struct sk_buff *skb,
		     int flow_mode)
{
	u32 flow_hash = 0, srchost_hash, dsthost_hash;
	u16 reduced_hash, srchost_idx, dsthost_idx;
	struct flow_keys keys, host_keys;

	if (unlikely(flow_mode == CAKE_FLOW_NONE))
		return 0;

	skb_flow_dissect_flow_keys(skb, &keys,
				   FLOW_DISSECTOR_F_STOP_AT_FLOW_LABEL);

	/* flow_hash_from_keys() sorts the addresses by value, so we have
	 * to preserve their order in a separate data structure to treat
	 * src and dst host addresses as independently selectable.
	 */
	host_keys = keys;
	host_keys.ports.ports     = 0;
	host_keys.basic.ip_proto  = 0;
	host_keys.keyid.keyid     = 0;
	host_keys.tags.flow_label = 0;

	switch (host_keys.control.addr_type) {
	case FLOW_DISSECTOR_KEY_IPV4_ADDRS:
		host_keys.addrs.v4addrs.src = 0;
		dsthost_hash = flow_hash_from_keys(&host_keys);
		host_keys.addrs.v4addrs.src = keys.addrs.v4addrs.src;
		host_keys.addrs.v4addrs.dst = 0;
		srchost_hash = flow_hash_from_keys(&host_keys);
		break;

	case FLOW_DISSECTOR_KEY_IPV6_ADDRS:
		memset(&host_keys.addrs.v6addrs.src, 0,
		       sizeof(host_keys.addrs.v6addrs.src));
		dsthost_hash = flow_hash_from_keys(&host_keys);
		host_keys.addrs.v6addrs.src = keys.addrs.v6addrs.src;
		memset(&host_keys.addrs.v6addrs.dst, 0,
		       sizeof(host_keys.addrs.v6addrs.dst));
		srchost_hash = flow_hash_from_keys(&host_keys);
		break;

	default:
		dsthost_hash = 0;
		srchost_hash = 0;
	}

	/* This *must* be after the above switch, since as a
	 * side-effect it sorts the src and dst addresses.
	 */
	if (flow_mode & CAKE_FLOW_FLOWS)
		flow_hash = flow_hash_from_keys(&keys);

	if (!(flow_mode & CAKE_FLOW_FLOWS)) {
		if (flow_mode & CAKE_FLOW_SRC_IP)
			flow_hash ^= srchost_hash;

		if (flow_mode & CAKE_FLOW_DST_IP)
			flow_hash ^= dsthost_hash;
	}

	reduced_hash = flow_hash % CAKE_QUEUES;

	/* set-associative hashing */
	/* fast path if no hash collision (direct lookup succeeds) */
	if (likely(q->tags[reduced_hash] == flow_hash &&
		   q->flows[reduced_hash].set)) {
		q->way_directs++;
	} else {
		u32 inner_hash = reduced_hash % CAKE_SET_WAYS;
		u32 outer_hash = reduced_hash - inner_hash;
		bool allocate_src = false;
		bool allocate_dst = false;
		u32 i, k;

		/* check if any active queue in the set is reserved for
		 * this flow.
		 */
		for (i = 0, k = inner_hash; i < CAKE_SET_WAYS;
		     i++, k = (k + 1) % CAKE_SET_WAYS) {
			if (q->tags[outer_hash + k] == flow_hash) {
				if (i)
					q->way_hits++;

				if (!q->flows[outer_hash + k].set) {
					/* need to increment host refcnts */
					allocate_src = cake_dsrc(flow_mode);
					allocate_dst = cake_ddst(flow_mode);
				}

				goto found;
			}
		}

		/* no queue is reserved for this flow, look for an
		 * empty one.
		 */
		for (i = 0; i < CAKE_SET_WAYS;
			 i++, k = (k + 1) % CAKE_SET_WAYS) {
			if (!q->flows[outer_hash + k].set) {
				q->way_misses++;
				allocate_src = cake_dsrc(flow_mode);
				allocate_dst = cake_ddst(flow_mode);
				goto found;
			}
		}

		/* With no empty queues, default to the original
		 * queue, accept the collision, update the host tags.
		 */
		q->way_collisions++;
		q->hosts[q->flows[reduced_hash].srchost].srchost_refcnt--;
		q->hosts[q->flows[reduced_hash].dsthost].dsthost_refcnt--;
		allocate_src = cake_dsrc(flow_mode);
		allocate_dst = cake_ddst(flow_mode);
found:
		/* reserve queue for future packets in same flow */
		reduced_hash = outer_hash + k;
		q->tags[reduced_hash] = flow_hash;

		if (allocate_src) {
			srchost_idx = srchost_hash % CAKE_QUEUES;
			inner_hash = srchost_idx % CAKE_SET_WAYS;
			outer_hash = srchost_idx - inner_hash;
			for (i = 0, k = inner_hash; i < CAKE_SET_WAYS;
				i++, k = (k + 1) % CAKE_SET_WAYS) {
				if (q->hosts[outer_hash + k].srchost_tag ==
				    srchost_hash)
					goto found_src;
			}
			for (i = 0; i < CAKE_SET_WAYS;
				i++, k = (k + 1) % CAKE_SET_WAYS) {
				if (!q->hosts[outer_hash + k].srchost_refcnt)
					break;
			}
			q->hosts[outer_hash + k].srchost_tag = srchost_hash;
found_src:
			srchost_idx = outer_hash + k;
			q->hosts[srchost_idx].srchost_refcnt++;
			q->flows[reduced_hash].srchost = srchost_idx;
		}

		if (allocate_dst) {
			dsthost_idx = dsthost_hash % CAKE_QUEUES;
			inner_hash = dsthost_idx % CAKE_SET_WAYS;
			outer_hash = dsthost_idx - inner_hash;
			for (i = 0, k = inner_hash; i < CAKE_SET_WAYS;
			     i++, k = (k + 1) % CAKE_SET_WAYS) {
				if (q->hosts[outer_hash + k].dsthost_tag ==
				    dsthost_hash)
					goto found_dst;
			}
			for (i = 0; i < CAKE_SET_WAYS;
			     i++, k = (k + 1) % CAKE_SET_WAYS) {
				if (!q->hosts[outer_hash + k].dsthost_refcnt)
					break;
			}
			q->hosts[outer_hash + k].dsthost_tag = dsthost_hash;
found_dst:
			dsthost_idx = outer_hash + k;
			q->hosts[dsthost_idx].dsthost_refcnt++;
			q->flows[reduced_hash].dsthost = dsthost_idx;
		}
	}

	return reduced_hash;
}

/* helper functions : might be changed when/if skb use a standard list_head */
/* remove one skb from head of slot queue */

static struct sk_buff *dequeue_head(struct cake_flow *flow)
{
	struct sk_buff *skb = flow->head;

	if (skb) {
		flow->head = skb->next;
		skb->next = NULL;
	}

	return skb;
}

/* add skb to flow queue (tail add) */

static void flow_queue_add(struct cake_flow *flow, struct sk_buff *skb)
{
	if (!flow->head)
		flow->head = skb;
	else
		flow->tail->next = skb;
	flow->tail = skb;
	skb->next = NULL;
}

static u64 cake_ewma(u64 avg, u64 sample, u32 shift)
{
	avg -= avg >> shift;
	avg += sample >> shift;
	return avg;
}

static void cake_heap_swap(struct cake_sched_data *q, u16 i, u16 j)
{
	struct cake_heap_entry ii = q->overflow_heap[i];
	struct cake_heap_entry jj = q->overflow_heap[j];

	q->overflow_heap[i] = jj;
	q->overflow_heap[j] = ii;

	q->tins[ii.t].overflow_idx[ii.b] = j;
	q->tins[jj.t].overflow_idx[jj.b] = i;
}

static u32 cake_heap_get_backlog(const struct cake_sched_data *q, u16 i)
{
	struct cake_heap_entry ii = q->overflow_heap[i];

	return q->tins[ii.t].backlogs[ii.b];
}

static void cake_heapify(struct cake_sched_data *q, u16 i)
{
	static const u32 a = CAKE_MAX_TINS * CAKE_QUEUES;
	u32 mb = cake_heap_get_backlog(q, i);
	u32 m = i;

	while (m < a) {
		u32 l = m + m + 1;
		u32 r = l + 1;

		if (l < a) {
			u32 lb = cake_heap_get_backlog(q, l);

			if (lb > mb) {
				m  = l;
				mb = lb;
			}
		}

		if (r < a) {
			u32 rb = cake_heap_get_backlog(q, r);

			if (rb > mb) {
				m  = r;
				mb = rb;
			}
		}

		if (m != i) {
			cake_heap_swap(q, i, m);
			i = m;
		} else {
			break;
		}
	}
}

static void cake_heapify_up(struct cake_sched_data *q, u16 i)
{
	while (i > 0 && i < CAKE_MAX_TINS * CAKE_QUEUES) {
		u16 p = (i - 1) >> 1;
		u32 ib = cake_heap_get_backlog(q, i);
		u32 pb = cake_heap_get_backlog(q, p);

		if (ib > pb) {
			cake_heap_swap(q, i, p);
			i = p;
		} else {
			break;
		}
	}
}

static int cake_advance_shaper(struct cake_sched_data *q,
			       struct cake_tin_data *b,
			       struct sk_buff *skb,
			       ktime_t now, bool drop)
{
	u32 len = qdisc_pkt_len(skb);

	/* charge packet bandwidth to this tin
	 * and to the global shaper.
	 */
	if (q->rate_ns) {
		u64 tin_dur = (len * b->tin_rate_ns) >> b->tin_rate_shft;
		u64 global_dur = (len * q->rate_ns) >> q->rate_shft;
		u64 failsafe_dur = global_dur + (global_dur >> 1);

		if (ktime_before(b->time_next_packet, now))
			b->time_next_packet = ktime_add_ns(b->time_next_packet,
							   tin_dur);

		else if (ktime_before(b->time_next_packet,
				      ktime_add_ns(now, tin_dur)))
			b->time_next_packet = ktime_add_ns(now, tin_dur);

		q->time_next_packet = ktime_add_ns(q->time_next_packet,
						   global_dur);
		if (!drop)
			q->failsafe_next_packet = \
				ktime_add_ns(q->failsafe_next_packet,
					     failsafe_dur);
	}
	return len;
}

static unsigned int cake_drop(struct Qdisc *sch, struct sk_buff **to_free)
{
	struct cake_sched_data *q = qdisc_priv(sch);
	ktime_t now = ktime_get();
	u32 idx = 0, tin = 0, len;
	struct cake_heap_entry qq;
	struct cake_tin_data *b;
	struct cake_flow *flow;
	struct sk_buff *skb;

	if (!q->overflow_timeout) {
		int i;
		/* Build fresh max-heap */
		for (i = CAKE_MAX_TINS * CAKE_QUEUES / 2; i >= 0; i--)
			cake_heapify(q, i);
	}
	q->overflow_timeout = 65535;

	/* select longest queue for pruning */
	qq  = q->overflow_heap[0];
	tin = qq.t;
	idx = qq.b;

	b = &q->tins[tin];
	flow = &b->flows[idx];
	skb = dequeue_head(flow);
	if (unlikely(!skb)) {
		/* heap has gone wrong, rebuild it next time */
		q->overflow_timeout = 0;
		return idx + (tin << 16);
	}

	if (cobalt_queue_full(&flow->cvars, &b->cparams, now))
		b->unresponsive_flow_count++;

	len = qdisc_pkt_len(skb);
	q->buffer_used      -= skb->truesize;
	b->backlogs[idx]    -= len;
	b->tin_backlog      -= len;
	sch->qstats.backlog -= len;
	qdisc_tree_reduce_backlog(sch, 1, len);

	flow->dropped++;
	b->tin_dropped++;
	sch->qstats.drops++;

	if (q->rate_flags & CAKE_FLAG_INGRESS)
		cake_advance_shaper(q, b, skb, now, true);

	__qdisc_drop(skb, to_free);
	sch->q.qlen--;

	cake_heapify(q, 0);

	return idx + (tin << 16);
}

static u32 cake_classify(struct Qdisc *sch, struct cake_tin_data *t,
			 struct sk_buff *skb, int flow_mode, int *qerr)
{
	struct cake_sched_data *q = qdisc_priv(sch);
	struct tcf_proto *filter;
	struct tcf_result res;
	int result;

	filter = rcu_dereference_bh(q->filter_list);
	if (!filter)
		return cake_hash(t, skb, flow_mode) + 1;

	*qerr = NET_XMIT_SUCCESS | __NET_XMIT_BYPASS;
	result = tcf_classify(skb, filter, &res, false);
	if (result >= 0) {
#ifdef CONFIG_NET_CLS_ACT
		switch (result) {
		case TC_ACT_STOLEN:
		case TC_ACT_QUEUED:
		case TC_ACT_TRAP:
			*qerr = NET_XMIT_SUCCESS | __NET_XMIT_STOLEN;
			/* fall through */
		case TC_ACT_SHOT:
			return 0;
		}
#endif
		if (TC_H_MIN(res.classid) <= CAKE_QUEUES)
			return TC_H_MIN(res.classid);
	}
	return 0;
}

static void cake_reconfigure(struct Qdisc *sch);

static s32 cake_enqueue(struct sk_buff *skb, struct Qdisc *sch,
			struct sk_buff **to_free)
{
	struct cake_sched_data *q = qdisc_priv(sch);
	int len = qdisc_pkt_len(skb);
	int uninitialized_var(ret);
	ktime_t now = ktime_get();
	struct cake_tin_data *b;
	struct cake_flow *flow;
	u32 idx, tin;

	tin = 0;
	b = &q->tins[tin];

	/* choose flow to insert into */
	idx = cake_classify(sch, b, skb, q->flow_mode, &ret);
	if (idx == 0) {
		if (ret & __NET_XMIT_BYPASS)
			qdisc_qstats_drop(sch);
		__qdisc_drop(skb, to_free);
		return ret;
	}
	idx--;
	flow = &b->flows[idx];

	/* ensure shaper state isn't stale */
	if (!b->tin_backlog) {
		if (ktime_before(b->time_next_packet, now))
			b->time_next_packet = now;

		if (!sch->q.qlen) {
			if (ktime_before(q->time_next_packet, now)) {
				q->failsafe_next_packet = now;
				q->time_next_packet = now;
			} else if (ktime_after(q->time_next_packet, now) &&
				   ktime_after(q->failsafe_next_packet, now)) {
				u64 next = \
					min(ktime_to_ns(q->time_next_packet),
					    ktime_to_ns(
						   q->failsafe_next_packet));
				sch->qstats.overlimits++;
				qdisc_watchdog_schedule_ns(&q->watchdog, next);
			}
		}
	}

	if (unlikely(len > b->max_skblen))
		b->max_skblen = len;

	cobalt_set_enqueue_time(skb, now);
	flow_queue_add(flow, skb);

	sch->q.qlen++;
	q->buffer_used      += skb->truesize;

	/* stats */
	b->packets++;
	b->bytes	    += len;
	b->backlogs[idx]    += len;
	b->tin_backlog      += len;
	sch->qstats.backlog += len;
	q->avg_window_bytes += len;

	if (q->overflow_timeout)
		cake_heapify_up(q, b->overflow_idx[idx]);

	/* incoming bandwidth capacity estimate */
	if (q->rate_flags & CAKE_FLAG_AUTORATE_INGRESS) {
		u64 packet_interval = \
			ktime_to_ns(ktime_sub(now, q->last_packet_time));

		if (packet_interval > NSEC_PER_SEC)
			packet_interval = NSEC_PER_SEC;

		/* filter out short-term bursts, eg. wifi aggregation */
		q->avg_packet_interval = \
			cake_ewma(q->avg_packet_interval,
				  packet_interval,
				  (packet_interval > q->avg_packet_interval ?
					  2 : 8));

		q->last_packet_time = now;

		if (packet_interval > q->avg_packet_interval) {
			u64 window_interval = \
				ktime_to_ns(ktime_sub(now,
						      q->avg_window_begin));
			u64 b = q->avg_window_bytes * (u64)NSEC_PER_SEC;

			do_div(b, window_interval);
			q->avg_peak_bandwidth =
				cake_ewma(q->avg_peak_bandwidth, b,
					  b > q->avg_peak_bandwidth ? 2 : 8);
			q->avg_window_bytes = 0;
			q->avg_window_begin = now;

			if (ktime_after(now,
					ktime_add_ms(q->last_reconfig_time,
						     250))) {
				q->rate_bps = (q->avg_peak_bandwidth * 15) >> 4;
				cake_reconfigure(sch);
			}
		}
	} else {
		q->avg_window_bytes = 0;
		q->last_packet_time = now;
	}

	/* flowchain */
	if (!flow->set || flow->set == CAKE_SET_DECAYING) {
		struct cake_host *srchost = &b->hosts[flow->srchost];
		struct cake_host *dsthost = &b->hosts[flow->dsthost];
		u16 host_load = 1;

		if (!flow->set) {
			list_add_tail(&flow->flowchain, &b->new_flows);
		} else {
			b->decaying_flow_count--;
			list_move_tail(&flow->flowchain, &b->new_flows);
		}
		flow->set = CAKE_SET_SPARSE;
		b->sparse_flow_count++;

		if (cake_dsrc(q->flow_mode))
			host_load = max(host_load, srchost->srchost_refcnt);

		if (cake_ddst(q->flow_mode))
			host_load = max(host_load, dsthost->dsthost_refcnt);

		flow->deficit = (b->flow_quantum *
				 quantum_div[host_load]) >> 16;
	} else if (flow->set == CAKE_SET_SPARSE_WAIT) {
		/* this flow was empty, accounted as a sparse flow, but actually
		 * in the bulk rotation.
		 */
		flow->set = CAKE_SET_BULK;
		b->sparse_flow_count--;
		b->bulk_flow_count++;
	}

	if (q->buffer_used > q->buffer_max_used)
		q->buffer_max_used = q->buffer_used;

	if (q->buffer_used > q->buffer_limit) {
		u32 dropped = 0;

		while (q->buffer_used > q->buffer_limit) {
			dropped++;
			cake_drop(sch, to_free);
		}
		b->drop_overlimit += dropped;
	}
	return NET_XMIT_SUCCESS;
}

static struct sk_buff *cake_dequeue_one(struct Qdisc *sch)
{
	struct cake_sched_data *q = qdisc_priv(sch);
	struct cake_tin_data *b = &q->tins[q->cur_tin];
	struct cake_flow *flow = &b->flows[q->cur_flow];
	struct sk_buff *skb = NULL;
	u32 len;

	if (flow->head) {
		skb = dequeue_head(flow);
		len = qdisc_pkt_len(skb);
		b->backlogs[q->cur_flow] -= len;
		b->tin_backlog		 -= len;
		sch->qstats.backlog      -= len;
		q->buffer_used		 -= skb->truesize;
		sch->q.qlen--;

		if (q->overflow_timeout)
			cake_heapify(q, b->overflow_idx[q->cur_flow]);
	}
	return skb;
}

/* Discard leftover packets from a tin no longer in use. */
static void cake_clear_tin(struct Qdisc *sch, u16 tin)
{
	struct cake_sched_data *q = qdisc_priv(sch);
	struct sk_buff *skb;

	q->cur_tin = tin;
	for (q->cur_flow = 0; q->cur_flow < CAKE_QUEUES; q->cur_flow++)
		while (!!(skb = cake_dequeue_one(sch)))
			kfree_skb(skb);
}

static struct sk_buff *cake_dequeue(struct Qdisc *sch)
{
	struct cake_sched_data *q = qdisc_priv(sch);
	struct cake_tin_data *b = &q->tins[q->cur_tin];
	struct cake_host *srchost, *dsthost;
	ktime_t now = ktime_get();
	struct cake_flow *flow;
	struct list_head *head;
	bool first_flow = true;
	struct sk_buff *skb;
	u16 host_load;
	u64 delay;
	u32 len;

begin:
	if (!sch->q.qlen)
		return NULL;

	/* global hard shaper */
	if (ktime_after(q->time_next_packet, now) &&
	    ktime_after(q->failsafe_next_packet, now)) {
		u64 next = min(ktime_to_ns(q->time_next_packet),
			       ktime_to_ns(q->failsafe_next_packet));

		sch->qstats.overlimits++;
		qdisc_watchdog_schedule_ns(&q->watchdog, next);
		return NULL;
	}

	/* Choose a class to work on. */
	if (!q->rate_ns) {
		/* In unlimited mode, can't rely on shaper timings, just balance
		 * with DRR
		 */
		bool wrapped = false, empty = true;

		while (b->tin_deficit < 0 ||
		       !(b->sparse_flow_count + b->bulk_flow_count)) {
			if (b->tin_deficit <= 0)
				b->tin_deficit += b->tin_quantum_band;
			if (b->sparse_flow_count + b->bulk_flow_count)
				empty = false;

			q->cur_tin++;
			b++;
			if (q->cur_tin >= q->tin_cnt) {
				q->cur_tin = 0;
				b = q->tins;

				if (wrapped) {
					/* It's possible for q->qlen to be
					 * nonzero when we actually have no
					 * packets anywhere.
					 */
					if (empty)
						return NULL;
				} else {
					wrapped = true;
				}
			}
		}
	} else {
		/* In shaped mode, choose:
		 * - Highest-priority tin with queue and meeting schedule, or
		 * - The earliest-scheduled tin with queue.
		 */
		ktime_t best_time = KTIME_MAX;
		int tin, best_tin = 0;

		for (tin = 0; tin < q->tin_cnt; tin++) {
			b = q->tins + tin;
			if ((b->sparse_flow_count + b->bulk_flow_count) > 0) {
				ktime_t time_to_pkt = \
					ktime_sub(b->time_next_packet, now);

				if (ktime_to_ns(time_to_pkt) <= 0 ||
				    ktime_compare(time_to_pkt,
						  best_time) <= 0) {
					best_time = time_to_pkt;
					best_tin = tin;
				}
			}
		}

		q->cur_tin = best_tin;
		b = q->tins + best_tin;

		/* No point in going further if no packets to deliver. */
		if (unlikely(!(b->sparse_flow_count + b->bulk_flow_count)))
			return NULL;
	}

retry:
	/* service this class */
	head = &b->decaying_flows;
	if (!first_flow || list_empty(head)) {
		head = &b->new_flows;
		if (list_empty(head)) {
			head = &b->old_flows;
			if (unlikely(list_empty(head))) {
				head = &b->decaying_flows;
				if (unlikely(list_empty(head)))
					goto begin;
			}
		}
	}
	flow = list_first_entry(head, struct cake_flow, flowchain);
	q->cur_flow = flow - b->flows;
	first_flow = false;

	/* triple isolation (modified DRR++) */
	srchost = &b->hosts[flow->srchost];
	dsthost = &b->hosts[flow->dsthost];
	host_load = 1;

	if (cake_dsrc(q->flow_mode))
		host_load = max(host_load, srchost->srchost_refcnt);

	if (cake_ddst(q->flow_mode))
		host_load = max(host_load, dsthost->dsthost_refcnt);

	WARN_ON(host_load > CAKE_QUEUES);

	/* flow isolation (DRR++) */
	if (flow->deficit <= 0) {
		/* The shifted prandom_u32() is a way to apply dithering to
		 * avoid accumulating roundoff errors
		 */
		flow->deficit += (b->flow_quantum * quantum_div[host_load] +
				  (prandom_u32() >> 16)) >> 16;
		list_move_tail(&flow->flowchain, &b->old_flows);

		/* Keep all flows with deficits out of the sparse and decaying
		 * rotations.  No non-empty flow can go into the decaying
		 * rotation, so they can't get deficits
		 */
		if (flow->set == CAKE_SET_SPARSE) {
			if (flow->head) {
				b->sparse_flow_count--;
				b->bulk_flow_count++;
				flow->set = CAKE_SET_BULK;
			} else {
				/* we've moved it to the bulk rotation for
				 * correct deficit accounting but we still want
				 * to count it as a sparse flow, not a bulk one.
				 */
				flow->set = CAKE_SET_SPARSE_WAIT;
			}
		}
		goto retry;
	}

	/* Retrieve a packet via the AQM */
	while (1) {
		skb = cake_dequeue_one(sch);
		if (!skb) {
			/* this queue was actually empty */
			if (cobalt_queue_empty(&flow->cvars, &b->cparams, now))
				b->unresponsive_flow_count--;

			if (flow->cvars.p_drop || flow->cvars.count ||
			    ktime_before(now, flow->cvars.drop_next)) {
				/* keep in the flowchain until the state has
				 * decayed to rest
				 */
				list_move_tail(&flow->flowchain,
					       &b->decaying_flows);
				if (flow->set == CAKE_SET_BULK) {
					b->bulk_flow_count--;
					b->decaying_flow_count++;
				} else if (flow->set == CAKE_SET_SPARSE ||
					   flow->set == CAKE_SET_SPARSE_WAIT) {
					b->sparse_flow_count--;
					b->decaying_flow_count++;
				}
				flow->set = CAKE_SET_DECAYING;
			} else {
				/* remove empty queue from the flowchain */
				list_del_init(&flow->flowchain);
				if (flow->set == CAKE_SET_SPARSE ||
				    flow->set == CAKE_SET_SPARSE_WAIT)
					b->sparse_flow_count--;
				else if (flow->set == CAKE_SET_BULK)
					b->bulk_flow_count--;
				else
					b->decaying_flow_count--;

				flow->set = CAKE_SET_NONE;
				srchost->srchost_refcnt--;
				dsthost->dsthost_refcnt--;
			}
			goto begin;
		}

		/* Last packet in queue may be marked, shouldn't be dropped */
		if (!cobalt_should_drop(&flow->cvars, &b->cparams, now, skb,
					(b->bulk_flow_count *
					 !!(q->rate_flags &
					    CAKE_FLAG_INGRESS))) ||
		    !flow->head)
			break;

		/* drop this packet, get another one */
		if (q->rate_flags & CAKE_FLAG_INGRESS) {
			len = cake_advance_shaper(q, b, skb,
						  now, true);
			flow->deficit -= len;
			b->tin_deficit -= len;
		}
		flow->dropped++;
		b->tin_dropped++;
		qdisc_tree_reduce_backlog(sch, 1, qdisc_pkt_len(skb));
		qdisc_qstats_drop(sch);
		kfree_skb(skb);
		if (q->rate_flags & CAKE_FLAG_INGRESS)
			goto retry;
	}

	b->tin_ecn_mark += !!flow->cvars.ecn_marked;
	qdisc_bstats_update(sch, skb);

	/* collect delay stats */
	delay = ktime_to_ns(ktime_sub(now, cobalt_get_enqueue_time(skb)));
	b->avge_delay = cake_ewma(b->avge_delay, delay, 8);
	b->peak_delay = cake_ewma(b->peak_delay, delay,
				  delay > b->peak_delay ? 2 : 8);
	b->base_delay = cake_ewma(b->base_delay, delay,
				  delay < b->base_delay ? 2 : 8);

	len = cake_advance_shaper(q, b, skb, now, false);
	flow->deficit -= len;
	b->tin_deficit -= len;

	if (ktime_after(q->time_next_packet, now) && sch->q.qlen) {
		u64 next = min(ktime_to_ns(q->time_next_packet),
			       ktime_to_ns(q->failsafe_next_packet));

		qdisc_watchdog_schedule_ns(&q->watchdog, next);
	} else if (!sch->q.qlen) {
		int i;

		for (i = 0; i < q->tin_cnt; i++) {
			if (q->tins[i].decaying_flow_count) {
				ktime_t next = \
					ktime_add_ns(now,
						     q->tins[i].cparams.target);

				qdisc_watchdog_schedule_ns(&q->watchdog,
							   ktime_to_ns(next));
				break;
			}
		}
	}

	if (q->overflow_timeout)
		q->overflow_timeout--;

	return skb;
}

static void cake_reset(struct Qdisc *sch)
{
	u32 c;

	for (c = 0; c < CAKE_MAX_TINS; c++)
		cake_clear_tin(sch, c);
}

static const struct nla_policy cake_policy[TCA_CAKE_MAX + 1] = {
	[TCA_CAKE_BASE_RATE64]   = { .type = NLA_U64 },
	[TCA_CAKE_DIFFSERV_MODE] = { .type = NLA_U32 },
	[TCA_CAKE_ATM]		 = { .type = NLA_U32 },
	[TCA_CAKE_FLOW_MODE]     = { .type = NLA_U32 },
	[TCA_CAKE_OVERHEAD]      = { .type = NLA_S32 },
	[TCA_CAKE_RTT]		 = { .type = NLA_U32 },
	[TCA_CAKE_TARGET]	 = { .type = NLA_U32 },
	[TCA_CAKE_AUTORATE]      = { .type = NLA_U32 },
	[TCA_CAKE_MEMORY]	 = { .type = NLA_U32 },
	[TCA_CAKE_NAT]		 = { .type = NLA_U32 },
	[TCA_CAKE_RAW]		 = { .type = NLA_U32 },
	[TCA_CAKE_WASH]		 = { .type = NLA_U32 },
	[TCA_CAKE_MPU]		 = { .type = NLA_U32 },
	[TCA_CAKE_INGRESS]	 = { .type = NLA_U32 },
	[TCA_CAKE_ACK_FILTER]	 = { .type = NLA_U32 },
};

static void cake_set_rate(struct cake_tin_data *b, u64 rate, u32 mtu,
			  u64 target_ns, u64 rtt_est_ns)
{
	/* convert byte-rate into time-per-byte
	 * so it will always unwedge in reasonable time.
	 */
	static const u64 MIN_RATE = 64;
	u32 byte_target = mtu;
	u64 byte_target_ns;
	u8  rate_shft = 0;
	u64 rate_ns = 0;

	b->flow_quantum = 1514;
	if (rate) {
		b->flow_quantum = max(min(rate >> 12, 1514ULL), 300ULL);
		rate_shft = 34;
		rate_ns = ((u64)NSEC_PER_SEC) << rate_shft;
		rate_ns = div64_u64(rate_ns, max(MIN_RATE, rate));
		while (!!(rate_ns >> 34)) {
			rate_ns >>= 1;
			rate_shft--;
		}
	} /* else unlimited, ie. zero delay */

	b->tin_rate_bps  = rate;
	b->tin_rate_ns   = rate_ns;
	b->tin_rate_shft = rate_shft;

	byte_target_ns = (byte_target * rate_ns) >> rate_shft;

	b->cparams.target = max((byte_target_ns * 3) / 2, target_ns);
	b->cparams.interval = max(rtt_est_ns +
				     b->cparams.target - target_ns,
				     b->cparams.target * 2);
	b->cparams.mtu_time = byte_target_ns;
	b->cparams.p_inc = 1 << 24; /* 1/256 */
	b->cparams.p_dec = 1 << 20; /* 1/4096 */
}

static void cake_reconfigure(struct Qdisc *sch)
{
	struct cake_sched_data *q = qdisc_priv(sch);
	struct cake_tin_data *b = &q->tins[0];
	int c, ft = 0;

	q->tin_cnt = 1;
	cake_set_rate(b, q->rate_bps, psched_mtu(qdisc_dev(sch)),
		      us_to_ns(q->target), us_to_ns(q->interval));
	b->tin_quantum_band = 65535;
	b->tin_quantum_prio = 65535;

	for (c = q->tin_cnt; c < CAKE_MAX_TINS; c++) {
		cake_clear_tin(sch, c);
		q->tins[c].cparams.mtu_time = q->tins[ft].cparams.mtu_time;
	}

	q->rate_ns   = q->tins[ft].tin_rate_ns;
	q->rate_shft = q->tins[ft].tin_rate_shft;

	if (q->buffer_config_limit) {
		q->buffer_limit = q->buffer_config_limit;
	} else if (q->rate_bps) {
		u64 t = q->rate_bps * q->interval;

		do_div(t, USEC_PER_SEC / 4);
		q->buffer_limit = max_t(u32, t, 4U << 20);
	} else {
		q->buffer_limit = ~0;
	}

	sch->flags &= ~TCQ_F_CAN_BYPASS;

	q->buffer_limit = min(q->buffer_limit,
			      max(sch->limit * psched_mtu(qdisc_dev(sch)),
				  q->buffer_config_limit));
}

static int cake_change(struct Qdisc *sch, struct nlattr *opt)
{
	struct cake_sched_data *q = qdisc_priv(sch);
	struct nlattr *tb[TCA_CAKE_MAX + 1];
	int err;

	if (!opt)
		return -EINVAL;

	err = nla_parse_nested(tb, TCA_CAKE_MAX, opt, cake_policy, NULL);
	if (err < 0)
		return err;

	if (tb[TCA_CAKE_BASE_RATE64])
		q->rate_bps = nla_get_u64(tb[TCA_CAKE_BASE_RATE64]);

	if (tb[TCA_CAKE_FLOW_MODE])
		q->flow_mode = (nla_get_u32(tb[TCA_CAKE_FLOW_MODE]) &
				CAKE_FLOW_MASK);

	if (tb[TCA_CAKE_RTT]) {
		q->interval = nla_get_u32(tb[TCA_CAKE_RTT]);

		if (!q->interval)
			q->interval = 1;
	}

	if (tb[TCA_CAKE_TARGET]) {
		q->target = nla_get_u32(tb[TCA_CAKE_TARGET]);

		if (!q->target)
			q->target = 1;
	}

	if (tb[TCA_CAKE_AUTORATE]) {
		if (!!nla_get_u32(tb[TCA_CAKE_AUTORATE]))
			q->rate_flags |= CAKE_FLAG_AUTORATE_INGRESS;
		else
			q->rate_flags &= ~CAKE_FLAG_AUTORATE_INGRESS;
	}

	if (tb[TCA_CAKE_INGRESS]) {
		if (!!nla_get_u32(tb[TCA_CAKE_INGRESS]))
			q->rate_flags |= CAKE_FLAG_INGRESS;
		else
			q->rate_flags &= ~CAKE_FLAG_INGRESS;
	}

	if (tb[TCA_CAKE_MEMORY])
		q->buffer_config_limit = nla_get_u32(tb[TCA_CAKE_MEMORY]);

	if (q->tins) {
		sch_tree_lock(sch);
		cake_reconfigure(sch);
		sch_tree_unlock(sch);
	}

	return 0;
}

static void cake_destroy(struct Qdisc *sch)
{
	struct cake_sched_data *q = qdisc_priv(sch);

	qdisc_watchdog_cancel(&q->watchdog);
	tcf_block_put(q->block);
	kvfree(q->tins);
}

static int cake_init(struct Qdisc *sch, struct nlattr *opt)
{
	struct cake_sched_data *q = qdisc_priv(sch);
	int i, j, err;

	sch->limit = 10240;
	q->tin_mode = CAKE_DIFFSERV_BESTEFFORT;
	q->flow_mode  = CAKE_FLOW_TRIPLE;

	q->rate_bps = 0; /* unlimited by default */

	q->interval = 100000; /* 100ms default */
	q->target   =   5000; /* 5ms: codel RFC argues
			       * for 5 to 10% of interval
			       */

	q->cur_tin = 0;
	q->cur_flow  = 0;

	qdisc_watchdog_init(&q->watchdog, sch);

	if (opt) {
		int err = cake_change(sch, opt);

		if (err)
			return err;
	}

	err = tcf_block_get(&q->block, &q->filter_list);
	if (err)
		return err;

	quantum_div[0] = ~0;
	for (i = 1; i <= CAKE_QUEUES; i++)
		quantum_div[i] = 65535 / i;

	q->tins = kvzalloc(CAKE_MAX_TINS * sizeof(struct cake_tin_data),
			   GFP_KERNEL);
	if (!q->tins)
		goto nomem;

	for (i = 0; i < CAKE_MAX_TINS; i++) {
		struct cake_tin_data *b = q->tins + i;

		INIT_LIST_HEAD(&b->new_flows);
		INIT_LIST_HEAD(&b->old_flows);
		INIT_LIST_HEAD(&b->decaying_flows);
		b->sparse_flow_count = 0;
		b->bulk_flow_count = 0;
		b->decaying_flow_count = 0;

		for (j = 0; j < CAKE_QUEUES; j++) {
			struct cake_flow *flow = b->flows + j;
			u32 k = j * CAKE_MAX_TINS + i;

			INIT_LIST_HEAD(&flow->flowchain);
			cobalt_vars_init(&flow->cvars);

			q->overflow_heap[k].t = i;
			q->overflow_heap[k].b = j;
			b->overflow_idx[j] = k;
		}
	}

	cake_reconfigure(sch);
	q->avg_peak_bandwidth = q->rate_bps;
	q->min_netlen = ~0;
	q->min_adjlen = ~0;
	return 0;

nomem:
	cake_destroy(sch);
	return -ENOMEM;
}

static int cake_dump(struct Qdisc *sch, struct sk_buff *skb)
{
	struct cake_sched_data *q = qdisc_priv(sch);
	struct nlattr *opts;

	opts = nla_nest_start(skb, TCA_OPTIONS);
	if (!opts)
		goto nla_put_failure;

	if (nla_put_u64_64bit(skb, TCA_CAKE_BASE_RATE64, q->rate_bps,
			      TCA_CAKE_PAD))
		goto nla_put_failure;

	if (nla_put_u32(skb, TCA_CAKE_FLOW_MODE,
			q->flow_mode & CAKE_FLOW_MASK))
		goto nla_put_failure;

	if (nla_put_u32(skb, TCA_CAKE_RTT, q->interval))
		goto nla_put_failure;

	if (nla_put_u32(skb, TCA_CAKE_TARGET, q->target))
		goto nla_put_failure;

	if (nla_put_u32(skb, TCA_CAKE_MEMORY, q->buffer_config_limit))
		goto nla_put_failure;

	if (nla_put_u32(skb, TCA_CAKE_AUTORATE,
			!!(q->rate_flags & CAKE_FLAG_AUTORATE_INGRESS)))
		goto nla_put_failure;

	if (nla_put_u32(skb, TCA_CAKE_INGRESS,
			!!(q->rate_flags & CAKE_FLAG_INGRESS)))
		goto nla_put_failure;

	return nla_nest_end(skb, opts);

nla_put_failure:
	return -1;
}

static int cake_dump_stats(struct Qdisc *sch, struct gnet_dump *d)
{
	struct nlattr *stats = nla_nest_start(d->skb, TCA_STATS_APP);
	struct cake_sched_data *q = qdisc_priv(sch);
	struct nlattr *tstats, *ts;
	int i;

	if (!stats)
		return -1;

#define PUT_STAT_U32(attr, data) do {				       \
		if (nla_put_u32(d->skb, TCA_CAKE_STATS_ ## attr, data)) \
			goto nla_put_failure;			       \
	} while (0)
#define PUT_STAT_U64(attr, data) do {				       \
		if (nla_put_u64_64bit(d->skb, TCA_CAKE_STATS_ ## attr, \
					data, TCA_CAKE_STATS_PAD)) \
			goto nla_put_failure;			       \
	} while (0)

	PUT_STAT_U64(CAPACITY_ESTIMATE64, q->avg_peak_bandwidth);
	PUT_STAT_U32(MEMORY_LIMIT, q->buffer_limit);
	PUT_STAT_U32(MEMORY_USED, q->buffer_max_used);
	PUT_STAT_U32(AVG_NETOFF, ((q->avg_netoff + 0x8000) >> 16));
	PUT_STAT_U32(MAX_NETLEN, q->max_netlen);
	PUT_STAT_U32(MAX_ADJLEN, q->max_adjlen);
	PUT_STAT_U32(MIN_NETLEN, q->min_netlen);
	PUT_STAT_U32(MIN_ADJLEN, q->min_adjlen);

#undef PUT_STAT_U32
#undef PUT_STAT_U64

	tstats = nla_nest_start(d->skb, TCA_CAKE_STATS_TIN_STATS);
	if (!tstats)
		goto nla_put_failure;

#define PUT_TSTAT_U32(attr, data) do {					\
		if (nla_put_u32(d->skb, TCA_CAKE_TIN_STATS_ ## attr, data)) \
			goto nla_put_failure;				\
	} while (0)
#define PUT_TSTAT_U64(attr, data) do {					\
		if (nla_put_u64_64bit(d->skb, TCA_CAKE_TIN_STATS_ ## attr, \
					data, TCA_CAKE_TIN_STATS_PAD))	\
			goto nla_put_failure;				\
	} while (0)

	for (i = 0; i < q->tin_cnt; i++) {
		struct cake_tin_data *b = &q->tins[i];

		ts = nla_nest_start(d->skb, i + 1);
		if (!ts)
			goto nla_put_failure;

		PUT_TSTAT_U64(THRESHOLD_RATE64, b->tin_rate_bps);
		PUT_TSTAT_U64(SENT_BYTES64, b->bytes);
		PUT_TSTAT_U32(BACKLOG_BYTES, b->tin_backlog);

		PUT_TSTAT_U32(TARGET_US,
			      ktime_to_us(ns_to_ktime(b->cparams.target)));
		PUT_TSTAT_U32(INTERVAL_US,
			      ktime_to_us(ns_to_ktime(b->cparams.interval)));

		PUT_TSTAT_U32(SENT_PACKETS, b->packets);
		PUT_TSTAT_U32(DROPPED_PACKETS, b->tin_dropped);
		PUT_TSTAT_U32(ECN_MARKED_PACKETS, b->tin_ecn_mark);
		PUT_TSTAT_U32(ACKS_DROPPED_PACKETS, b->ack_drops);

		PUT_TSTAT_U32(PEAK_DELAY_US,
			      ktime_to_us(ns_to_ktime(b->peak_delay)));
		PUT_TSTAT_U32(AVG_DELAY_US,
			      ktime_to_us(ns_to_ktime(b->avge_delay)));
		PUT_TSTAT_U32(BASE_DELAY_US,
			      ktime_to_us(ns_to_ktime(b->base_delay)));

		PUT_TSTAT_U32(WAY_INDIRECT_HITS, b->way_hits);
		PUT_TSTAT_U32(WAY_MISSES, b->way_misses);
		PUT_TSTAT_U32(WAY_COLLISIONS, b->way_collisions);

		PUT_TSTAT_U32(SPARSE_FLOWS, b->sparse_flow_count +
					    b->decaying_flow_count);
		PUT_TSTAT_U32(BULK_FLOWS, b->bulk_flow_count);
		PUT_TSTAT_U32(UNRESPONSIVE_FLOWS, b->unresponsive_flow_count);
		PUT_TSTAT_U32(MAX_SKBLEN, b->max_skblen);

		PUT_TSTAT_U32(FLOW_QUANTUM, b->flow_quantum);
		nla_nest_end(d->skb, ts);
	}

#undef PUT_TSTAT_U32
#undef PUT_TSTAT_U64

	nla_nest_end(d->skb, tstats);
	return nla_nest_end(d->skb, stats);

nla_put_failure:
	nla_nest_cancel(d->skb, stats);
	return -1;
}

static struct Qdisc *cake_leaf(struct Qdisc *sch, unsigned long arg)
{
	return NULL;
}

static unsigned long cake_find(struct Qdisc *sch, u32 classid)
{
	return 0;
}

static unsigned long cake_bind(struct Qdisc *sch, unsigned long parent,
			       u32 classid)
{
	return 0;
}

static void cake_unbind(struct Qdisc *q, unsigned long cl)
{
}

static struct tcf_block *cake_tcf_block(struct Qdisc *sch, unsigned long cl)
{
	struct cake_sched_data *q = qdisc_priv(sch);

	if (cl)
		return NULL;
	return q->block;
}

static int cake_dump_class(struct Qdisc *sch, unsigned long cl,
			   struct sk_buff *skb, struct tcmsg *tcm)
{
	tcm->tcm_handle |= TC_H_MIN(cl);
	return 0;
}

static int cake_dump_class_stats(struct Qdisc *sch, unsigned long cl,
				 struct gnet_dump *d)
{
	struct cake_sched_data *q = qdisc_priv(sch);
	const struct cake_flow *flow = NULL;
	struct gnet_stats_queue qs = { 0 };
	struct nlattr *stats;
	u32 idx = cl - 1;

	if (idx < CAKE_QUEUES * q->tin_cnt) {
		const struct cake_tin_data *b = &q->tins[idx / CAKE_QUEUES];
		const struct sk_buff *skb;

		flow = &b->flows[idx % CAKE_QUEUES];

		if (flow->head) {
			sch_tree_lock(sch);
			skb = flow->head;
			while (skb) {
				qs.qlen++;
				skb = skb->next;
			}
			sch_tree_unlock(sch);
		}
		qs.backlog = b->backlogs[idx % CAKE_QUEUES];
		qs.drops = flow->dropped;
	}
	if (gnet_stats_copy_queue(d, NULL, &qs, qs.qlen) < 0)
		return -1;
	if (flow) {
		ktime_t now = ktime_get();

		stats = nla_nest_start(d->skb, TCA_STATS_APP);
		if (!stats)
			return -1;

#define PUT_STAT_U32(attr, data) do {				       \
		if (nla_put_u32(d->skb, TCA_CAKE_STATS_ ## attr, data)) \
			goto nla_put_failure;			       \
	} while (0)
#define PUT_STAT_S32(attr, data) do {				       \
		if (nla_put_s32(d->skb, TCA_CAKE_STATS_ ## attr, data)) \
			goto nla_put_failure;			       \
	} while (0)

		PUT_STAT_S32(DEFICIT, flow->deficit);
		PUT_STAT_U32(DROPPING, flow->cvars.dropping);
		PUT_STAT_U32(COBALT_COUNT, flow->cvars.count);
		PUT_STAT_U32(P_DROP, flow->cvars.p_drop);
		if (flow->cvars.p_drop) {
			PUT_STAT_S32(BLUE_TIMER_US,
				     ktime_to_us(
					     ktime_sub(now,
						     flow->cvars.blue_timer)));
		}
		if (flow->cvars.dropping) {
			PUT_STAT_S32(DROP_NEXT_US,
				     ktime_to_us(
					     ktime_sub(now,
						       flow->cvars.drop_next)));
		}

		if (nla_nest_end(d->skb, stats) < 0)
			return -1;
	}

	return 0;

nla_put_failure:
	nla_nest_cancel(d->skb, stats);
	return -1;
}

static void cake_walk(struct Qdisc *sch, struct qdisc_walker *arg)
{
	struct cake_sched_data *q = qdisc_priv(sch);
	unsigned int i, j;

	if (arg->stop)
		return;

	for (i = 0; i < q->tin_cnt; i++) {
		struct cake_tin_data *b = &q->tins[i];

		for (j = 0; j < CAKE_QUEUES; j++) {
			if (list_empty(&b->flows[j].flowchain) ||
			    arg->count < arg->skip) {
				arg->count++;
				continue;
			}
			if (arg->fn(sch, i * CAKE_QUEUES + j + 1, arg) < 0) {
				arg->stop = 1;
				break;
			}
			arg->count++;
		}
	}
}

static const struct Qdisc_class_ops cake_class_ops = {
	.leaf		=	cake_leaf,
	.find		=	cake_find,
	.tcf_block	=	cake_tcf_block,
	.bind_tcf	=	cake_bind,
	.unbind_tcf	=	cake_unbind,
	.dump		=	cake_dump_class,
	.dump_stats	=	cake_dump_class_stats,
	.walk		=	cake_walk,
};

static struct Qdisc_ops cake_qdisc_ops __read_mostly = {
	.cl_ops		=	&cake_class_ops,
	.id		=	"cake",
	.priv_size	=	sizeof(struct cake_sched_data),
	.enqueue	=	cake_enqueue,
	.dequeue	=	cake_dequeue,
	.peek		=	qdisc_peek_dequeued,
	.init		=	cake_init,
	.reset		=	cake_reset,
	.destroy	=	cake_destroy,
	.change		=	cake_change,
	.dump		=	cake_dump,
	.dump_stats	=	cake_dump_stats,
	.owner		=	THIS_MODULE,
};

static int __init cake_module_init(void)
{
	return register_qdisc(&cake_qdisc_ops);
}

static void __exit cake_module_exit(void)
{
	unregister_qdisc(&cake_qdisc_ops);
}

module_init(cake_module_init)
module_exit(cake_module_exit)
MODULE_AUTHOR("Jonathan Morton");
MODULE_LICENSE("Dual BSD/GPL");
MODULE_DESCRIPTION("The CAKE shaper.");
