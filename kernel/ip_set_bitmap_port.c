/* Copyright (C) 2003-2011 Jozsef Kadlecsik <kadlec@blackhole.kfki.hu>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

/* Kernel module implementing an IP set type: the bitmap:port type */

#include <linux/module.h>
#include <linux/ip.h>
#include <linux/tcp.h>
#include <linux/udp.h>
#include <linux/skbuff.h>
#include <linux/errno.h>
#include <linux/uaccess.h>
#include <linux/bitops.h>
#include <linux/spinlock.h>
#include <linux/netlink.h>
#include <linux/jiffies.h>
#include <linux/timer.h>
#include <net/netlink.h>

#include <linux/netfilter/ipset/ip_set.h>
#include <linux/netfilter/ipset/ip_set_bitmap.h>
#include <linux/netfilter/ipset/ip_set_getport.h>
#define IP_SET_BITMAP_TIMEOUT
#include <linux/netfilter/ipset/ip_set_timeout.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Jozsef Kadlecsik <kadlec@blackhole.kfki.hu>");
MODULE_DESCRIPTION("bitmap:port type of IP sets");
MODULE_ALIAS("ip_set_bitmap:port");

/* Base variant */

struct bitmap_port {
	void *members;		/* the set members */
	u16 first_port;		/* host byte order, included in range */
	u16 last_port;		/* host byte order, included in range */
	size_t memsize;		/* members size */
};

static inline int
bitmap_port_test(const struct bitmap_port *map, u16 id)
{
	return !!test_bit(id, map->members);
}

static inline int
bitmap_port_add(struct bitmap_port *map, u16 id)
{
	if (test_and_set_bit(id, map->members))
		return -IPSET_ERR_EXIST;

	return 0;
}

static int
bitmap_port_del(struct bitmap_port *map, u16 id)
{
	if (!test_and_clear_bit(id, map->members))
		return -IPSET_ERR_EXIST;

	return 0;
}

static int
bitmap_port_kadt(struct ip_set *set, const struct sk_buff *skb,
		 enum ipset_adt adt, u8 pf, u8 dim, u8 flags)
{
	struct bitmap_port *map = set->data;
	__be16 __port;
	u16 port = 0;

	if (!ip_set_get_ip_port(skb, pf, flags & IPSET_DIM_ONE_SRC, &__port))
		return -EINVAL;

	port = ntohs(__port);

	if (port < map->first_port || port > map->last_port)
		return -IPSET_ERR_BITMAP_RANGE;

	port -= map->first_port;

	switch (adt) {
	case IPSET_TEST:
		return bitmap_port_test(map, port);
	case IPSET_ADD:
		return bitmap_port_add(map, port);
	case IPSET_DEL:
		return bitmap_port_del(map, port);
	default:
		return -EINVAL;
	}
}

static const struct nla_policy bitmap_port_adt_policy[IPSET_ATTR_ADT_MAX+1] = {
	[IPSET_ATTR_PORT]	= { .type = NLA_U16 },
	[IPSET_ATTR_PORT_TO]	= { .type = NLA_U16 },
	[IPSET_ATTR_TIMEOUT]	= { .type = NLA_U32 },
	[IPSET_ATTR_LINENO]	= { .type = NLA_U32 },
};

static int
bitmap_port_uadt(struct ip_set *set, struct nlattr *head, int len,
		 enum ipset_adt adt, u32 *lineno, u32 flags)
{
	struct bitmap_port *map = set->data;
	struct nlattr *tb[IPSET_ATTR_ADT_MAX+1];
	u32 port;	/* wraparound */
	u16 id, port_to;
	int ret = 0;

	if (nla_parse(tb, IPSET_ATTR_ADT_MAX, head, len,
		      bitmap_port_adt_policy))
		return -IPSET_ERR_PROTOCOL;

	if (unlikely(!ip_set_attr_netorder(tb, IPSET_ATTR_PORT) ||
		     !ip_set_optattr_netorder(tb, IPSET_ATTR_PORT_TO)))
		return -IPSET_ERR_PROTOCOL;

	if (tb[IPSET_ATTR_LINENO])
		*lineno = nla_get_u32(tb[IPSET_ATTR_LINENO]);

	port = ip_set_get_h16(tb[IPSET_ATTR_PORT]);
	if (port < map->first_port || port > map->last_port)
		return -IPSET_ERR_BITMAP_RANGE;

	if (tb[IPSET_ATTR_TIMEOUT])
		return -IPSET_ERR_TIMEOUT;

	if (adt == IPSET_TEST)
		return bitmap_port_test(map, port - map->first_port);

	if (tb[IPSET_ATTR_PORT_TO]) {
		port_to = ip_set_get_h16(tb[IPSET_ATTR_PORT_TO]);
		if (port > port_to) {
			swap(port, port_to);
			if (port < map->first_port)
				return -IPSET_ERR_BITMAP_RANGE;
		}
	} else
		port_to = port;

	if (port_to > map->last_port)
		return -IPSET_ERR_BITMAP_RANGE;

	for (; port <= port_to; port++) {
		id = port - map->first_port;
		ret = adt == IPSET_ADD ? bitmap_port_add(map, id)
				       : bitmap_port_del(map, id);

		if (ret && !ip_set_eexist(ret, flags))
			return ret;
		else
			ret = 0;
	}
	return ret;
}

static void
bitmap_port_destroy(struct ip_set *set)
{
	struct bitmap_port *map = set->data;

	ip_set_free(map->members);
	kfree(map);

	set->data = NULL;
}

static void
bitmap_port_flush(struct ip_set *set)
{
	struct bitmap_port *map = set->data;

	memset(map->members, 0, map->memsize);
}

static int
bitmap_port_head(struct ip_set *set, struct sk_buff *skb)
{
	const struct bitmap_port *map = set->data;
	struct nlattr *nested;

	nested = ipset_nest_start(skb, IPSET_ATTR_DATA);
	if (!nested)
		goto nla_put_failure;
	NLA_PUT_NET16(skb, IPSET_ATTR_PORT, htons(map->first_port));
	NLA_PUT_NET16(skb, IPSET_ATTR_PORT_TO, htons(map->last_port));
	NLA_PUT_NET32(skb, IPSET_ATTR_REFERENCES,
		      htonl(atomic_read(&set->ref) - 1));
	NLA_PUT_NET32(skb, IPSET_ATTR_MEMSIZE,
		      htonl(sizeof(*map) + map->memsize));
	ipset_nest_end(skb, nested);

	return 0;
nla_put_failure:
	return -EFAULT;
}

static int
bitmap_port_list(const struct ip_set *set,
		 struct sk_buff *skb, struct netlink_callback *cb)
{
	const struct bitmap_port *map = set->data;
	struct nlattr *atd, *nested;
	u16 id, first = cb->args[2];
	u16 last = map->last_port - map->first_port;

	atd = ipset_nest_start(skb, IPSET_ATTR_ADT);
	if (!atd)
		return -EFAULT;
	for (; cb->args[2] <= last; cb->args[2]++) {
		id = cb->args[2];
		if (!test_bit(id, map->members))
			continue;
		nested = ipset_nest_start(skb, IPSET_ATTR_DATA);
		if (!nested) {
			if (id == first) {
				nla_nest_cancel(skb, atd);
				return -EFAULT;
			} else
				goto nla_put_failure;
		}
		NLA_PUT_NET16(skb, IPSET_ATTR_PORT,
			      htons(map->first_port + id));
		ipset_nest_end(skb, nested);
	}
	ipset_nest_end(skb, atd);
	/* Set listing finished */
	cb->args[2] = 0;

	return 0;

nla_put_failure:
	nla_nest_cancel(skb, nested);
	ipset_nest_end(skb, atd);
	return 0;
}

static bool
bitmap_port_same_set(const struct ip_set *a, const struct ip_set *b)
{
	const struct bitmap_port *x = a->data;
	const struct bitmap_port *y = b->data;

	return x->first_port == y->first_port &&
	       x->last_port == y->last_port;
}

static const struct ip_set_type_variant bitmap_port = {
	.kadt	= bitmap_port_kadt,
	.uadt	= bitmap_port_uadt,
	.destroy = bitmap_port_destroy,
	.flush	= bitmap_port_flush,
	.head	= bitmap_port_head,
	.list	= bitmap_port_list,
	.same_set = bitmap_port_same_set,
};

/* Timeout variant */

struct bitmap_port_timeout {
	unsigned long *members;	/* the set members */
	u16 first_port;		/* host byte order, included in range */
	u16 last_port;		/* host byte order, included in range */
	size_t memsize;		/* members size */

	u32 timeout;		/* timeout parameter */
	struct timer_list gc;	/* garbage collection */
};

static inline bool
bitmap_port_timeout_test(const struct bitmap_port_timeout *map, u16 id)
{
	return ip_set_timeout_test(map->members[id]);
}

static int
bitmap_port_timeout_add(const struct bitmap_port_timeout *map,
			u16 id, u32 timeout)
{
	if (bitmap_port_timeout_test(map, id))
		return -IPSET_ERR_EXIST;

	map->members[id] = ip_set_timeout_set(timeout);

	return 0;
}

static int
bitmap_port_timeout_del(const struct bitmap_port_timeout *map,
			u16 id)
{
	int ret = -IPSET_ERR_EXIST;

	if (bitmap_port_timeout_test(map, id))
		ret = 0;

	map->members[id] = IPSET_ELEM_UNSET;
	return ret;
}

static int
bitmap_port_timeout_kadt(struct ip_set *set, const struct sk_buff *skb,
			 enum ipset_adt adt, u8 pf, u8 dim, u8 flags)
{
	struct bitmap_port_timeout *map = set->data;
	__be16 __port;
	u16 port = 0;

	if (!ip_set_get_ip_port(skb, pf, flags & IPSET_DIM_ONE_SRC, &__port))
		return -EINVAL;

	port = ntohs(__port);

	if (port < map->first_port || port > map->last_port)
		return -IPSET_ERR_BITMAP_RANGE;

	port -= map->first_port;

	switch (adt) {
	case IPSET_TEST:
		return bitmap_port_timeout_test(map, port);
	case IPSET_ADD:
		return bitmap_port_timeout_add(map, port, map->timeout);
	case IPSET_DEL:
		return bitmap_port_timeout_del(map, port);
	default:
		return -EINVAL;
	}
}

static int
bitmap_port_timeout_uadt(struct ip_set *set, struct nlattr *head, int len,
			 enum ipset_adt adt, u32 *lineno, u32 flags)
{
	const struct bitmap_port_timeout *map = set->data;
	struct nlattr *tb[IPSET_ATTR_ADT_MAX+1];
	u16 id, port_to;
	u32 port, timeout = map->timeout;	/* wraparound */
	int ret = 0;

	if (nla_parse(tb, IPSET_ATTR_ADT_MAX, head, len,
		      bitmap_port_adt_policy))
		return -IPSET_ERR_PROTOCOL;

	if (unlikely(!ip_set_attr_netorder(tb, IPSET_ATTR_PORT) ||
		     !ip_set_optattr_netorder(tb, IPSET_ATTR_PORT_TO) ||
		     !ip_set_optattr_netorder(tb, IPSET_ATTR_TIMEOUT)))
		return -IPSET_ERR_PROTOCOL;

	if (tb[IPSET_ATTR_LINENO])
		*lineno = nla_get_u32(tb[IPSET_ATTR_LINENO]);

	port = ip_set_get_h16(tb[IPSET_ATTR_PORT]);
	if (port < map->first_port || port > map->last_port)
		return -IPSET_ERR_BITMAP_RANGE;

	if (adt == IPSET_TEST)
		return bitmap_port_timeout_test(map, port - map->first_port);

	if (tb[IPSET_ATTR_PORT_TO]) {
		port_to = ip_set_get_h16(tb[IPSET_ATTR_PORT_TO]);
		if (port > port_to) {
			swap(port, port_to);
			if (port < map->first_port)
				return -IPSET_ERR_BITMAP_RANGE;
		}
	} else
		port_to = port;

	if (port_to > map->last_port)
		return -IPSET_ERR_BITMAP_RANGE;

	if (tb[IPSET_ATTR_TIMEOUT])
		timeout = ip_set_timeout_uget(tb[IPSET_ATTR_TIMEOUT]);

	for (; port <= port_to; port++) {
		id = port - map->first_port;
		ret = adt == IPSET_ADD
			? bitmap_port_timeout_add(map, id, timeout)
			: bitmap_port_timeout_del(map, id);

		if (ret && !ip_set_eexist(ret, flags))
			return ret;
		else
			ret = 0;
	}
	return ret;
}

static void
bitmap_port_timeout_destroy(struct ip_set *set)
{
	struct bitmap_port_timeout *map = set->data;

	del_timer_sync(&map->gc);
	ip_set_free(map->members);
	kfree(map);

	set->data = NULL;
}

static void
bitmap_port_timeout_flush(struct ip_set *set)
{
	struct bitmap_port_timeout *map = set->data;

	memset(map->members, 0, map->memsize);
}

static int
bitmap_port_timeout_head(struct ip_set *set, struct sk_buff *skb)
{
	const struct bitmap_port_timeout *map = set->data;
	struct nlattr *nested;

	nested = ipset_nest_start(skb, IPSET_ATTR_DATA);
	if (!nested)
		goto nla_put_failure;
	NLA_PUT_NET16(skb, IPSET_ATTR_PORT, htons(map->first_port));
	NLA_PUT_NET16(skb, IPSET_ATTR_PORT_TO, htons(map->last_port));
	NLA_PUT_NET32(skb, IPSET_ATTR_TIMEOUT , htonl(map->timeout));
	NLA_PUT_NET32(skb, IPSET_ATTR_REFERENCES,
		      htonl(atomic_read(&set->ref) - 1));
	NLA_PUT_NET32(skb, IPSET_ATTR_MEMSIZE,
		      htonl(sizeof(*map) + map->memsize));
	ipset_nest_end(skb, nested);

	return 0;
nla_put_failure:
	return -EFAULT;
}

static int
bitmap_port_timeout_list(const struct ip_set *set,
			 struct sk_buff *skb, struct netlink_callback *cb)
{
	const struct bitmap_port_timeout *map = set->data;
	struct nlattr *adt, *nested;
	u16 id, first = cb->args[2];
	u16 last = map->last_port - map->first_port;
	const unsigned long *table = map->members;

	adt = ipset_nest_start(skb, IPSET_ATTR_ADT);
	if (!adt)
		return -EFAULT;
	for (; cb->args[2] <= last; cb->args[2]++) {
		id = cb->args[2];
		if (!bitmap_port_timeout_test(map, id))
			continue;
		nested = ipset_nest_start(skb, IPSET_ATTR_DATA);
		if (!nested) {
			if (id == first) {
				nla_nest_cancel(skb, adt);
				return -EFAULT;
			} else
				goto nla_put_failure;
		}
		NLA_PUT_NET16(skb, IPSET_ATTR_PORT,
			      htons(map->first_port + id));
		NLA_PUT_NET32(skb, IPSET_ATTR_TIMEOUT,
			      htonl(ip_set_timeout_get(table[id])));
		ipset_nest_end(skb, nested);
	}
	ipset_nest_end(skb, adt);

	/* Set listing finished */
	cb->args[2] = 0;

	return 0;

nla_put_failure:
	nla_nest_cancel(skb, nested);
	ipset_nest_end(skb, adt);
	return 0;
}

static bool
bitmap_port_timeout_same_set(const struct ip_set *a, const struct ip_set *b)
{
	const struct bitmap_port_timeout *x = a->data;
	const struct bitmap_port_timeout *y = b->data;

	return x->first_port == y->first_port &&
	       x->last_port == y->last_port &&
	       x->timeout == y->timeout;
}

static const struct ip_set_type_variant bitmap_port_timeout = {
	.kadt	= bitmap_port_timeout_kadt,
	.uadt	= bitmap_port_timeout_uadt,
	.destroy = bitmap_port_timeout_destroy,
	.flush	= bitmap_port_timeout_flush,
	.head	= bitmap_port_timeout_head,
	.list	= bitmap_port_timeout_list,
	.same_set = bitmap_port_timeout_same_set,
};

static void
bitmap_port_gc(unsigned long ul_set)
{
	struct ip_set *set = (struct ip_set *) ul_set;
	struct bitmap_port_timeout *map = set->data;
	unsigned long *table = map->members;
	u32 id;	/* wraparound */
	u16 last = map->last_port - map->first_port;

	/* We run parallel with other readers (test element)
	 * but adding/deleting new entries is locked out */
	read_lock_bh(&set->lock);
	for (id = 0; id <= last; id++)
		if (ip_set_timeout_expired(table[id]))
			table[id] = IPSET_ELEM_UNSET;
	read_unlock_bh(&set->lock);

	map->gc.expires = jiffies + IPSET_GC_PERIOD(map->timeout) * HZ;
	add_timer(&map->gc);
}

static void
bitmap_port_gc_init(struct ip_set *set)
{
	struct bitmap_port_timeout *map = set->data;

	init_timer(&map->gc);
	map->gc.data = (unsigned long) set;
	map->gc.function = bitmap_port_gc;
	map->gc.expires = jiffies + IPSET_GC_PERIOD(map->timeout) * HZ;
	add_timer(&map->gc);
}

/* Create bitmap:ip type of sets */

static const struct nla_policy
bitmap_port_create_policy[IPSET_ATTR_CREATE_MAX+1] = {
	[IPSET_ATTR_PORT]	= { .type = NLA_U16 },
	[IPSET_ATTR_PORT_TO]	= { .type = NLA_U16 },
	[IPSET_ATTR_TIMEOUT]	= { .type = NLA_U32 },
};

static bool
init_map_port(struct ip_set *set, struct bitmap_port *map,
	      u16 first_port, u16 last_port)
{
	map->members = ip_set_alloc(map->memsize, GFP_KERNEL);
	if (!map->members)
		return false;
	map->first_port = first_port;
	map->last_port = last_port;

	set->data = map;
	set->family = AF_UNSPEC;

	return true;
}

static int
bitmap_port_create(struct ip_set *set, struct nlattr *head, int len,
		 u32 flags)
{
	struct nlattr *tb[IPSET_ATTR_CREATE_MAX+1];
	u16 first_port, last_port;

	if (nla_parse(tb, IPSET_ATTR_CREATE_MAX, head, len,
		      bitmap_port_create_policy))
		return -IPSET_ERR_PROTOCOL;

	if (unlikely(!ip_set_attr_netorder(tb, IPSET_ATTR_PORT) ||
		     !ip_set_attr_netorder(tb, IPSET_ATTR_PORT_TO) ||
		     !ip_set_optattr_netorder(tb, IPSET_ATTR_TIMEOUT)))
		return -IPSET_ERR_PROTOCOL;

	first_port = ip_set_get_h16(tb[IPSET_ATTR_PORT]);
	last_port = ip_set_get_h16(tb[IPSET_ATTR_PORT_TO]);
	if (first_port > last_port) {
		u16 tmp = first_port;

		first_port = last_port;
		last_port = tmp;
	}

	if (tb[IPSET_ATTR_TIMEOUT]) {
		struct bitmap_port_timeout *map;

		map = kzalloc(sizeof(*map), GFP_KERNEL);
		if (!map)
			return -ENOMEM;

		map->memsize = (last_port - first_port + 1)
			       * sizeof(unsigned long);

		if (!init_map_port(set, (struct bitmap_port *) map,
				   first_port, last_port)) {
			kfree(map);
			return -ENOMEM;
		}

		map->timeout = ip_set_timeout_uget(tb[IPSET_ATTR_TIMEOUT]);
		set->variant = &bitmap_port_timeout;

		bitmap_port_gc_init(set);
	} else {
		struct bitmap_port *map;

		map = kzalloc(sizeof(*map), GFP_KERNEL);
		if (!map)
			return -ENOMEM;

		map->memsize = bitmap_bytes(0, last_port - first_port);
		pr_debug("memsize: %zu\n", map->memsize);
		if (!init_map_port(set, map, first_port, last_port)) {
			kfree(map);
			return -ENOMEM;
		}

		set->variant = &bitmap_port;
	}
	return 0;
}

static struct ip_set_type bitmap_port_type = {
	.name		= "bitmap:port",
	.protocol	= IPSET_PROTOCOL,
	.features	= IPSET_TYPE_PORT,
	.dimension	= IPSET_DIM_ONE,
	.family		= AF_UNSPEC,
	.revision	= 0,
	.create		= bitmap_port_create,
	.me		= THIS_MODULE,
};

static int __init
bitmap_port_init(void)
{
	return ip_set_type_register(&bitmap_port_type);
}

static void __exit
bitmap_port_fini(void)
{
	ip_set_type_unregister(&bitmap_port_type);
}

module_init(bitmap_port_init);
module_exit(bitmap_port_fini);
