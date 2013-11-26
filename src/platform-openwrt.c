#include <syslog.h>
#include <errno.h>
#include <assert.h>

#include <arpa/inet.h>
#include <unistd.h>
#include <stdint.h>
#include <stdlib.h>

#include <sys/un.h>
#include <sys/socket.h>

#include <libubox/blobmsg.h>
#include <libubus.h>

#include "platform.h"
#include "iface.h"

static struct ubus_context *ubus = NULL;
static struct ubus_subscriber netifd;
static uint32_t ubus_network_interface = 0;
static struct blob_buf b;

static int handle_update(__unused struct ubus_context *ctx,
		__unused struct ubus_object *obj, __unused struct ubus_request_data *req,
		__unused const char *method, struct blob_attr *msg);
static void handle_dump(__unused struct ubus_request *req,
		__unused int type, struct blob_attr *msg);

static void platform_commit(struct uloop_timeout *t);
struct platform_iface {
	struct iface *iface;
	struct uloop_timeout update;
	char handle[];
};


int platform_init(void)
{
	if (!(ubus = ubus_connect(NULL))) {
		syslog(LOG_ERR, "Failed to connect to ubus: %s", strerror(errno));
		return -1;
	}

	netifd.cb = handle_update;
	ubus_register_subscriber(ubus, &netifd);

	ubus_lookup_id(ubus, "network.interface", &ubus_network_interface);
	ubus_add_uloop(ubus);

	ubus_subscribe(ubus, &netifd, ubus_network_interface);
	ubus_invoke(ubus, ubus_network_interface, "dump", NULL, handle_dump, NULL, 0);

	return 0;
}

// Constructor for openwrt-specific interface part
void platform_iface_new(struct iface *c, const char *handle)
{
	assert(c->platform == NULL);

	size_t handlenamelen = strlen(handle) + 1;
	struct platform_iface *iface = calloc(1, sizeof(*iface) + handlenamelen);
	iface->iface = c;
	iface->update.cb = platform_commit;

	c->platform = iface;

	// Have to rerun dump here as to sync up on nested interfaces
	ubus_invoke(ubus, ubus_network_interface, "dump", NULL, handle_dump, NULL, 0);
}

// Destructor for openwrt-specific interface part
void platform_iface_free(struct iface *c)
{
	struct platform_iface *iface = c->platform;
	if (iface) {
		uloop_timeout_cancel(&iface->update);
		free(iface);
		c->platform = NULL;
	}
}

void platform_set_internal(struct iface *c,
		__unused bool internal)
{
	struct platform_iface *iface = c->platform;
	assert(iface);
	uloop_timeout_set(&iface->update, 100);
}

void platform_set_address(struct iface *c,
		__unused struct iface_addr *addr, __unused bool enable)
{
	platform_set_internal(c, false);
}


void platform_set_owner(struct iface *c,
		__unused bool enable)
{
	platform_set_internal(c, false);
}


// Commit platform changes to netifd
static void platform_commit(struct uloop_timeout *t)
{
	struct platform_iface *iface = container_of(t, struct platform_iface, update);
	struct iface *c = iface->iface;

	blob_buf_init(&b, 0);
	blobmsg_add_u32(&b, "action", 0);
	blobmsg_add_u8(&b, "link-up", 1);
	blobmsg_add_string(&b, "interface", iface->handle);

	void *k, *l;
	struct iface_addr *a;

	k = blobmsg_open_array(&b, "ipaddr");
	vlist_for_each_element(&c->assigned, a, node) {
		if (!IN6_IS_ADDR_V4MAPPED(&a->prefix.prefix))
			continue;

		l = blobmsg_open_table(&b, NULL);

		char *buf = blobmsg_alloc_string_buffer(&b, "ipaddr", INET_ADDRSTRLEN);
		inet_ntop(AF_INET, &a->prefix.prefix.s6_addr32[3], buf, INET_ADDRSTRLEN);
		blobmsg_add_string_buffer(&b);

		buf = blobmsg_alloc_string_buffer(&b, "mask", 4);
		snprintf(buf, 4, "%u", a->prefix.plen);
		blobmsg_add_string_buffer(&b);

		blobmsg_close_table(&b, l);
	}
	blobmsg_close_array(&b, k);

	k = blobmsg_open_array(&b, "ip6addr");
	vlist_for_each_element(&c->assigned, a, node) {
		if (IN6_IS_ADDR_V4MAPPED(&a->prefix.prefix))
			continue;

		l = blobmsg_open_table(&b, NULL);

		char *buf = blobmsg_alloc_string_buffer(&b, "ipaddr", INET6_ADDRSTRLEN);
		inet_ntop(AF_INET6, &a->prefix.prefix, buf, INET6_ADDRSTRLEN);
		blobmsg_add_string_buffer(&b);

		buf = blobmsg_alloc_string_buffer(&b, "mask", 4);
		snprintf(buf, 4, "%u", a->prefix.plen);
		blobmsg_add_string_buffer(&b);

		if (a->valid_until) {
			blobmsg_add_u32(&b, "preferred", a->preferred_until);
			blobmsg_add_u32(&b, "valid", a->valid_until);
		}

		blobmsg_close_table(&b, l);
	}
	blobmsg_close_array(&b, k);

	k = blobmsg_open_table(&b, "data");

	if (c->domain) {
		l = blobmsg_open_array(&b, "domain");
		blobmsg_add_string(&b, NULL, c->domain);
		blobmsg_close_array(&b, l);
	}

	const char *service = (c->linkowner) ? "server" : "disabled";
	blobmsg_add_string(&b, "ra", service);
	blobmsg_add_string(&b, "dhcpv4", service);
	blobmsg_add_string(&b, "dhcpv6", service);

	const char *zone = (c->internal) ? "lan" : "wan";
	blobmsg_add_string(&b, "zone", zone);

	blobmsg_close_table(&b, k);

	// TODO: test return code
	ubus_invoke(ubus, ubus_network_interface, "proto_update", b.head, NULL, NULL, 1000);
}


enum {
	PREFIX_ATTR_ADDRESS,
	PREFIX_ATTR_MASK,
	PREFIX_ATTR_VALID,
	PREFIX_ATTR_PREFERRED,
	PREFIX_ATTR_MAX,
};


static const struct blobmsg_policy prefix_attrs[PREFIX_ATTR_MAX] = {
	[PREFIX_ATTR_ADDRESS] = { .name = "address", .type = BLOBMSG_TYPE_STRING },
	[PREFIX_ATTR_MASK] = { .name = "mask", .type = BLOBMSG_TYPE_INT32 },
	[PREFIX_ATTR_PREFERRED] = { .name = "preferred", .type = BLOBMSG_TYPE_INT32 },
	[PREFIX_ATTR_VALID] = { .name = "valid", .type = BLOBMSG_TYPE_INT32 },
};


// Decode and analyze all delegated prefixes and commit them to iface
static void update_delegated(struct iface *c, struct blob_attr *prefixes)
{
	iface_update_delegated(c);

	if (prefixes) {
		hnetd_time_t now = hnetd_time();
		struct blob_attr *k;
		unsigned rem;

		blobmsg_for_each_attr(k, prefixes, rem) {
			struct blob_attr *tb[PREFIX_ATTR_MAX], *a;
			blobmsg_parse(prefix_attrs, PREFIX_ATTR_MAX, tb,
					blobmsg_data(k), blobmsg_data_len(k));

			hnetd_time_t preferred = HNETD_TIME_MAX;
			hnetd_time_t valid = HNETD_TIME_MAX;
			struct prefix p = {IN6ADDR_ANY_INIT, 0};

			if (!(a = tb[PREFIX_ATTR_ADDRESS]) ||
					inet_pton(AF_INET6, blobmsg_get_string(a), &p.prefix) < 1)
				continue;

			if (!(a = tb[PREFIX_ATTR_MASK]))
				continue;

			p.plen = blobmsg_get_u32(a);

			if ((a = tb[PREFIX_ATTR_PREFERRED]))
				preferred = now + (blobmsg_get_u32(a) * HNETD_TIME_PER_SECOND);

			if ((a = tb[PREFIX_ATTR_VALID]))
				valid = now + (blobmsg_get_u32(a) * HNETD_TIME_PER_SECOND);

			iface_add_delegated(c, &p, valid, preferred);
		}
	}

	iface_commit_delegated(c);
}


enum {
	IFACE_ATTR_IFNAME,
	IFACE_ATTR_PROTO,
	IFACE_ATTR_PREFIX,
	IFACE_ATTR_V4ADDR,
	IFACE_ATTR_DELEGATION,
	IFACE_ATTR_MAX,
};

static const struct blobmsg_policy iface_attrs[IFACE_ATTR_MAX] = {
	[IFACE_ATTR_IFNAME] = { .name = "l3_device", .type = BLOBMSG_TYPE_STRING },
	[IFACE_ATTR_PROTO] = { .name = "proto", .type = BLOBMSG_TYPE_STRING },
	[IFACE_ATTR_PREFIX] = { .name = "ipv6-prefix", .type = BLOBMSG_TYPE_ARRAY },
	[IFACE_ATTR_V4ADDR] = { .name = "ipv4-address", .type = BLOBMSG_TYPE_ARRAY },
	[IFACE_ATTR_DELEGATION] = { .name = "delegation", .type = BLOBMSG_TYPE_BOOL },
};


// Decode and analyze netifd interface status blob
static void platform_update(void *data, size_t len)
{
	struct blob_attr *tb[IFACE_ATTR_MAX], *a;
	blobmsg_parse(iface_attrs, IFACE_ATTR_MAX, tb, data, len);

	if (!(a = tb[IFACE_ATTR_IFNAME]))
		return;

	const char *ifname = blobmsg_get_string(a);
	struct iface *c = iface_get(ifname);

	if (c && c->platform) {
		// This is a known managed interface

		const char *proto = NULL;
		if ((a = tb[IFACE_ATTR_PROTO]))
			proto = blobmsg_get_string(a);

		if (!proto)
			return;

		if (!strcmp(proto, "dhcpv6")) {
			// Our nested DHCPv6 client interface
			update_delegated(c, tb[IFACE_ATTR_PREFIX]);
		} else if (!strcmp(proto, "dhcp")) {
			// Our nested DHCP client interface
			bool v4leased = false;

			if ((a = tb[IFACE_ATTR_V4ADDR])) {
				struct blob_attr *c;
				unsigned rem;

				blobmsg_for_each_attr(c, a, rem)
					v4leased = true;
			}

			iface_set_v4leased(c, v4leased);
		}
	} else {
		// We have only unmanaged interfaces at this point

		// If netifd delegates this prefix, ignore it
		if ((a = tb[IFACE_ATTR_DELEGATION]) && blobmsg_get_bool(a))
			tb[IFACE_ATTR_PREFIX] = NULL;

		bool empty = !(a = tb[IFACE_ATTR_PREFIX]) || blobmsg_data_len(a) <= 0;

		// If we don't know this interface yet but it has a PD for us create it
		if (!c && !empty)
			c = iface_create(ifname, NULL);

		update_delegated(c, tb[IFACE_ATTR_PREFIX]);

		// Likewise, if all prefixes are gone, delete the interface
		if (c && empty)
			iface_remove(c);
	}
}


// Handle netifd ubus event for interfaces updates
static int handle_update(__unused struct ubus_context *ctx, __unused struct ubus_object *obj,
		__unused struct ubus_request_data *req, __unused const char *method,
		struct blob_attr *msg)
{
	platform_update(blob_data(msg), blob_len(msg));
	return 0;
}


enum {
	DUMP_ATTR_INTERFACE,
	DUMP_ATTR_MAX
};

static const struct blobmsg_policy dump_attrs[DUMP_ATTR_MAX] = {
	[DUMP_ATTR_INTERFACE] = { .name = "interface", .type = BLOBMSG_TYPE_ARRAY },
};

// Handle netifd ubus reply for interface dump
static void handle_dump(__unused struct ubus_request *req,
		__unused int type, struct blob_attr *msg)
{
	struct blob_attr *tb[DUMP_ATTR_INTERFACE];
	blobmsg_parse(dump_attrs, DUMP_ATTR_MAX, tb, blob_data(msg), blob_len(msg));

	if (!tb[DUMP_ATTR_INTERFACE])
		return;

	struct blob_attr *c;
	unsigned rem;

	blobmsg_for_each_attr(c, tb[DUMP_ATTR_INTERFACE], rem)
		platform_update(blobmsg_data(c), blobmsg_data_len(c));
}

