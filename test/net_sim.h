/*
 * $Id: net_sim.h $
 *
 * Author: Markus Stenberg <markus stenberg@iki.fi>
 *
 * Copyright (c) 2013 cisco Systems, Inc.
 *
 * Created:       Fri Dec  6 18:48:08 2013 mstenber
 * Last modified: Tue Apr 21 13:27:12 2015 mstenber
 * Edit time:     262 min
 *
 */

#ifndef NET_SIM_H
#define NET_SIM_H

#include "hncp_i.h"
#include "hncp_pa.h"
#include "hncp_sd.h"
#include "hncp_link.h"
#include "hncp_multicast.h"
#include "sput.h"

/* Lots of stubs here, rather not put __unused all over the place. */
#pragma GCC diagnostic ignored "-Wunused-parameter"

/* Use the faked interfaces */
#include "fake_iface.h"

/* We leverage the fake timers and other stuff in fake_uloop. */
#include "fake_uloop.h"

/* iface_* functions from smock queue */
#include "smock.h"

#ifdef L_PREFIX
#undef L_PREFIX
#endif /* L_PREFIX */
#define L_PREFIX ""

/* This is abstraction that can be used to play with multiple HNCP
 * instances; no separate C module provided just due to
 * laziness. Moved from test_hncp_net (as test_hncp_pa needs similar
 * code but it has to be separate test binary due to different
 * interests in stubbed interfaces) */

#ifndef MAXIMUM_PROPAGATION_DELAY
#define MAXIMUM_PROPAGATION_DELAY 100
#endif /* !MAXIMUM_PROPAGATION_DELAY */
#if MAXIMUM_PROPAGATION_DELAY > 0
#define MESSAGE_PROPAGATION_DELAY (random() % MAXIMUM_PROPAGATION_DELAY + 1)
#else
#define MESSAGE_PROPAGATION_DELAY 1
#endif

typedef struct {
  struct list_head h;

  dncp_link l;
  struct sockaddr_in6 src;
  struct in6_addr dst;
  void *buf;
  size_t len;

  /* When is it delivered? */
  struct uloop_timeout deliver_to;
} net_msg_s, *net_msg;

typedef struct {
  struct list_head h;

  dncp_link src;
  dncp_link dst;
} net_neigh_s, *net_neigh;

typedef struct {
  struct list_head h;
  struct net_sim_t *s;
  char *name;
  dncp_s n;
  struct hncp_link *link;
#ifndef DISABLE_HNCP_PA
  hncp_pa pa;
#endif /* !DISABLE_HNCP_PA */
#ifndef DISABLE_HNCP_MULTICAST
  hncp_multicast multicast;
#endif /* !DISABLE_HNCP_MULTICAST */
  hncp_sd sd;

  /* Received messages (timeout has moved them from global list to
   * ours readable list) */
  struct list_head messages;

  /* When is it scheduled to run? */
  struct uloop_timeout run_to;

  /* Debug subscriber we use just to make sure there are no changes
   * when the topology should be stable. */
  dncp_subscriber_s debug_subscriber;

  struct list_head iface_users;
} net_node_s, *net_node;

typedef struct net_sim_t {
  /* Initialized set of nodes. */
  struct list_head nodes;
  struct list_head neighs;
  struct list_head messages;

  bool disable_link_auto_address;
  bool disable_sd;
  bool disable_pa;
  bool disable_multicast;

  int node_count;
  bool should_be_stable_topology;
  hnetd_time_t start;

  int sent_unicast;
  hnetd_time_t last_unicast_sent;
  int sent_multicast;

  int converged_count;
  int not_converged_count;

  bool use_global_iids;
  int next_free_iid;

  bool accept_time_errors;
} net_sim_s, *net_sim;

static struct list_head net_sim_interfaces = LIST_HEAD_INIT(net_sim_interfaces);

void net_sim_init(net_sim s)
{
  memset(s, 0, sizeof(*s));
  INIT_LIST_HEAD(&s->nodes);
  INIT_LIST_HEAD(&s->neighs);
  INIT_LIST_HEAD(&s->messages);
  uloop_init();
  s->start = hnetd_time();
  s->next_free_iid = 100;
}

int net_sim_dncp_tlv_type_count(dncp o, int type)
{
  int c = 0;
  dncp_node n;
  struct tlv_attr *a;

  dncp_for_each_node(o, n)
    dncp_node_for_each_tlv_with_type(n, a, type)
      c++;
  L_DEBUG("net_sim_dncp_tlv_type_count %d -> %d", type, c);
  return c;
}

bool net_sim_is_converged(net_sim s)
{
  net_node n, n2, fn = NULL;
  bool first = true;
  dncp_node hn;
  int acceptable_offset = MAXIMUM_PROPAGATION_DELAY * (s->node_count - 1);
#if L_LEVEL >= 7
  /* Dump # of nodes in each node */
  char *buf = alloca(4 * s->node_count), *c = buf;
  list_for_each_entry(n, &s->nodes, h)
    {
      int count = 0;

      dncp_for_each_node(&n->n, hn)
        count++;
      c += sprintf(c, "%d ", count);
    }
  L_DEBUG("net_sim_is_converged: %s", buf);
#endif /* L_LEVEL >= 7 */

  list_for_each_entry(n, &s->nodes, h)
    {
      if (n->n.network_hash_dirty)
        return false;
      if (first)
        {
          fn = n;
          first = false;
          continue;
        }
      if (memcmp(&fn->n.network_hash, &n->n.network_hash, sizeof(dncp_hash_s)))
        {
          L_DEBUG("network hash mismatch %s<>%s [%llx <> %llx]",
                  fn->name, n->name,
                  dncp_hash64(&fn->n.network_hash),
                  dncp_hash64(&n->n.network_hash));
          s->not_converged_count++;
          return false;
        }
    }
  list_for_each_entry(n, &s->nodes, h)
    {
      list_for_each_entry(n2, &s->nodes, h)
        {
          /* Make sure that the information about other node _is_ valid */
          hn = dncp_find_node_by_node_identifier(&n->n, &n2->n.own_node->node_identifier, false);
          if (!hn)
            {
              L_DEBUG("unable to find other node hash - %s -> %s",
                      n->name, n2->name);
              return false;
            }
          if (memcmp(&n2->n.own_node->node_data_hash,
                     &hn->node_data_hash, DNCP_HASH_LEN))
            {
              L_DEBUG("node data hash mismatch w/ network hash in sync %s @%s",
                      n2->name, n->name);
              return false;
            }
          if (!s->accept_time_errors
              && llabs(n2->n.own_node->origination_time
                       - hn->origination_time) > acceptable_offset)
            {
              L_DEBUG("origination time mismatch at "
                      "%s: %lld !=~ %lld for %s [update number %d]",
                      n->name,
                      (long long) hn->origination_time,
                      (long long) n2->n.own_node->origination_time,
                      n2->name,
                      hn->update_number);
              s->not_converged_count++;
              return false;
            }
        }
    }

  s->converged_count++;
  return true;
}

bool net_sim_is_busy(net_sim s)
{
  net_node n;

  if (!list_empty(&s->messages))
    {
      L_DEBUG("net_sim_is_busy: messages pending");
      return true;
    }
  list_for_each_entry(n, &s->nodes, h)
    {
      if (n->n.immediate_scheduled)
        {
          L_DEBUG("net_sim_is_busy: immediate scheduled");
          return true;
        }
#ifndef DISABLE_HNCP_SD
      if (!s->disable_sd && hncp_sd_busy(n->sd))
        {
          L_DEBUG("net_sim_is_busy: pending sd");
          return true;
        }
#endif /* !DISABLE_HNCP_SD */
#ifndef DISABLE_HNCP_MULTICAST
      if (!s->disable_multicast && hncp_multicast_busy(n->multicast))
        {
          L_DEBUG("net_sim_is_busy: pending multicast");
          return true;
        }
#endif /* !DISABLE_HNCP_MULTICAST */
    }
  return false;
}



void net_sim_local_tlv_callback(dncp_subscriber sub,
                                struct tlv_attr *tlv, bool add)
{
  net_node n = container_of(sub, net_node_s, debug_subscriber);
  net_sim s = n->s;

  if (s->should_be_stable_topology)
    if (tlv_id(tlv) == DNCP_T_NODE_DATA_NEIGHBOR)
      {
        sput_fail_unless(false, "got change when topology stable");
      }
}

dncp net_sim_find_hncp(net_sim s, const char *name)
{
  net_node n;
  bool r;

  list_for_each_entry(n, &s->nodes, h)
    {
      if (strcmp(n->name, name) == 0)
        return &n->n;
    }

  n = calloc(1, sizeof(*n));
  current_iface_users = &n->iface_users;
  n->name = strdup(name);
  sput_fail_unless(n, "calloc net_node");
  sput_fail_unless(n->name, "strdup name");
  n->s = s;
  r = hncp_init(&n->n, name, strlen(name));
  n->n.io_init_done = true; /* our IO doesn't really need init.. */
  sput_fail_unless(r, "hncp_init");
  if (!r)
    {
    fail:
      current_iface_users = NULL;
      return NULL;
    }
  list_add_tail(&n->h, &s->nodes);
  INIT_LIST_HEAD(&n->messages);
  INIT_LIST_HEAD(&n->iface_users);
  if (!(n->link = hncp_link_create(&n->n, NULL)))
    goto fail;
#ifndef DISABLE_HNCP_PA
  /* Glue it to pa */
  if (!s->disable_pa && !(n->pa = hncp_pa_create(&n->n, n->link)))
    goto fail;
#endif /* !DISABLE_HNCP_PA */
#ifndef DISABLE_HNCP_SD
  static hncp_sd_params_s sd_params = {
    .dnsmasq_script = "s-dnsmasq",
    .dnsmasq_bonus_file = "/tmp/dnsmasq.conf",
    .ohp_script = "s-ohp",
    .pcp_script = "s-pcp",
  };

  /* Add SD support */
  if (!s->disable_sd)
    if (!(n->sd = hncp_sd_create(&n->n, &sd_params, NULL)))
      goto fail;

#endif /* !DISABLE_HNCP_SD */
#ifndef DISABLE_HNCP_MULTICAST
  static hncp_multicast_params_s multicast_params = {
    .multicast_script = "s-mc"
  };
  if (!s->disable_multicast)
    if (!(n->multicast = hncp_multicast_create(&n->n, &multicast_params)))
      return NULL;
#endif /* !DISABLE_HNCP_MULTICAST */
  n->debug_subscriber.local_tlv_change_callback = net_sim_local_tlv_callback;
  s->node_count++;
  dncp_subscribe(&n->n, &n->debug_subscriber);
  L_DEBUG("[%s] %s net_sim_find_hncp added",
          DNCP_NODE_REPR(n->n.own_node), n->name);
  current_iface_users = NULL;
  return &n->n;
}

dncp_link net_sim_dncp_find_link_by_name(dncp o, const char *name)
{
  net_node n = container_of(o, net_node_s, n);
  dncp_link l;

  l = dncp_find_link_by_name(o, name, false);

  if (l)
    return l;

  dncp_if_set_enabled(o, name, true);

  l = dncp_find_link_by_name(o, name, false);

  sput_fail_unless(l, "dncp_find_link_by_name");
  if (l)
    {
      /* Initialize the address - in rather ugly way. We just hash
       * ifname + xor that with our own hash. The result should be
       * highly unique still. */
      dncp_hash_s h1, h2;
      unsigned char buf[16];
      int i;

      dncp_calculate_hash(name, strlen(name), &h1);
      dncp_calculate_hash(n->name, strlen(n->name), &h2);

      int bytes = DNCP_HASH_LEN;
      if (bytes > 8)
        bytes = 8;
      memset(buf, 0, sizeof(buf));
      for (i = 0; i < bytes; i++)
        buf[i+8] = h1.buf[i] ^ h2.buf[i];
      buf[0] = 0xFE;
      buf[1] = 0x80;
      /* 2 .. 7 left 0 always */
      dncp_link_set_ipv6_address(l, (struct in6_addr *)buf);
      l->has_ipv6_address = !n->s->disable_link_auto_address;
      /* Internally we use the ipv6 address even if it is not
       * officially set(!). Beautiful.. */
      /* Override the iid to be unique. */
      if (n->s->use_global_iids)
        l->iid = n->s->next_free_iid++;

      l->ifindex = l->iid;

      /* Give callback about it to iface users. */
      net_sim_node_iface_callback(n, cb_intiface, name, true);
    }
  return l;
}

void net_sim_set_connected(dncp_link l1, dncp_link l2, bool enabled)
{
  dncp o = l1->dncp;
  net_node node = container_of(o, net_node_s, n);
  net_sim s = node->s;
  net_neigh n;


  L_DEBUG("connection %p/%d -> %p/%d %s",
          l1, l1->iid, l2, l2->iid, enabled ? "on" : "off");
  if (enabled)
    {
      /* Make sure it's not there already */
      list_for_each_entry(n, &s->neighs, h)
        if (n->src == l1 && n->dst == l2)
          return;

      /* Add node */
      n = calloc(1, sizeof(*n));

      sput_fail_unless(n, "calloc net_neigh");
      n->src = l1;
      n->dst = l2;
      list_add(&n->h, &s->neighs);
    }
  else
    {
      /* Remove node */
      list_for_each_entry(n, &s->neighs, h)
        {
          if (n->src == l1 && n->dst == l2)
            {
              list_del(&n->h);
              free(n);
              return;
            }
        }
    }
}

void net_sim_remove_node(net_sim s, net_node node)
{
  struct list_head *p, *pn;
  dncp o = &node->n;
  net_neigh n, nn;

  /* Remove from neighbors */
  list_for_each_entry_safe(n, nn, &s->neighs, h)
    {
      if (n->src->dncp == o || n->dst->dncp == o)
        {
          list_del(&n->h);
          free(n);
        }
    }

  /* Remove from messages */
  list_for_each_safe(p, pn, &s->messages)
    {
      net_msg m = container_of(p, net_msg_s, h);
      if (m->l->dncp == o)
        {
          uloop_timeout_cancel(&m->deliver_to);
          list_del(&m->h);
          free(m->buf);
          free(m);
        }
    }

  uloop_timeout_cancel(&node->run_to);

  /* Remove from list of nodes */
  list_del(&node->h);
  free(node->name);
  hncp_uninit(&node->n);

#ifndef DISABLE_HNCP_SD
  /* Get rid of sd data structure */
  if (!s->disable_sd)
    hncp_sd_destroy(node->sd);
#endif /* !DISABLE_HNCP_SD */

#ifndef DISABLE_HNCP_PA
  /* Kill glue (has to be done _after_ hncp_uninit). */
  if (!s->disable_pa)
    hncp_pa_destroy(node->pa);
#endif /* !DISABLE_HNCP_PA */
#ifndef DISABLE_HNCP_MULTICAST
  if (!s->disable_multicast)
    hncp_multicast_destroy(node->multicast);
#endif /* !DISABLE_HNCP_MULTICAST */
  free(node);
}

void net_sim_remove_node_by_name(net_sim s, const char *name)
{
  dncp o = net_sim_find_hncp(s, name);
  net_node node = container_of(o, net_node_s, n);
  sput_fail_unless(o, "net_sim_find_hncp");
  net_sim_remove_node(s, node);
}

void net_sim_uninit(net_sim s)
{
  struct list_head *p, *pn;
  int c = 0;

  list_for_each_safe(p, pn, &s->nodes)
    {
      net_node node = container_of(p, net_node_s, h);
      net_sim_remove_node(s, node);
      c++;
    }
  L_NOTICE("#nodes:%d elapsed:%.2fs unicasts:%d multicasts:%d",
           c,
           (float)(hnetd_time() - s->start) / HNETD_TIME_PER_SECOND,
           s->sent_unicast, s->sent_multicast);
  sput_fail_unless(list_empty(&s->neighs), "no neighs");
  sput_fail_unless(list_empty(&s->messages), "no messages");
}

void net_sim_advance(net_sim s, hnetd_time_t t)
{
  set_hnetd_time(t);
  L_DEBUG("time = %lld", (long long int) (t - s->start));
}

#define SIM_WHILE(s, maxiter, criteria)                 \
  do {                                                  \
    int iter = 0;                                       \
                                                        \
    sput_fail_unless((criteria), "criteria at start");  \
    while (iter < maxiter && fu_loop(1) == 0)           \
      {                                                 \
        while (fu_poll());                              \
        if (!(criteria))                                \
          break;                                        \
        iter++;                                         \
      }                                                 \
    sput_fail_unless(!(criteria), "!criteria at end");  \
  } while(0)

void net_sim_populate_iface_next(net_node n)
{
  static char dummybuf[12345];
  struct iface *i = (struct iface *)dummybuf;
  dncp_link l;

  vlist_for_each_element(&n->n.links, l, in_links)
    {
      *i = default_iface;
      strcpy(i->ifname, l->ifname);
      smock_push("iface_next", i);
      i = (void *)i + sizeof(struct iface) + strlen(l->ifname) + 1;
    }
  smock_push("iface_next", NULL);
}

/************************************************* Mocked interface - dncp_io */

bool dncp_io_init(dncp o)
{
  return true;
}

void dncp_io_uninit(dncp o)
{
}

bool dncp_io_set_ifname_enabled(dncp o, const char *ifname, bool enabled)
{
  return true;
}

int dncp_io_get_hwaddrs(unsigned char *buf, int buf_left)
{
  return 0;
}

bool dncp_io_get_ipv6(struct in6_addr *addr, char *prefer_ifname)
{
  memset(addr, 0, sizeof(*addr));
  ((uint8_t *)addr)[0] = prefer_ifname ? 1 : 0;
  return true;
}

static void _node_run_cb(struct uloop_timeout *t)
{
  net_node node = container_of(t, net_node_s, run_to);
  L_DEBUG("%s: dncp_run", node->name);
  dncp_run(&node->n);
}

void dncp_io_schedule(dncp o, int msecs)
{
  net_node node = container_of(o, net_node_s, n);
  sput_fail_unless(msecs >= 0, "should be present or future");
  node->run_to.cb = _node_run_cb;
  uloop_timeout_set(&node->run_to, msecs);
}

ssize_t dncp_io_recvfrom(dncp o, void *buf, size_t len,
                         char *ifname,
                         struct sockaddr_in6 *src,
                         struct in6_addr *dst)
{
  net_node node = container_of(o, net_node_s, n);
  net_msg m;

  list_for_each_entry(m, &node->messages, h)
    {
      int s = m->len > len ? len : m->len;
      strcpy(ifname, m->l->ifname);
      *src = m->src;
      *dst = m->dst;
      memcpy(buf, m->buf, s);
      list_del(&m->h);
      free(m->buf);
      free(m);
      L_DEBUG("%s/%s: dncp_io_recvfrom %d bytes", node->name, ifname, s);
      return s;
    }
  return - 1;
}

void
sanity_check_buf(void *buf, size_t len)
{
  struct tlv_attr *a, *last = NULL;
  int a_len;
  int last_len;
  bool ok = true;
  size_t dhs = sizeof(dncp_t_node_data_header_s);

  tlv_for_each_in_buf(a, buf, len)
    {
      a_len = tlv_pad_len(a);
      if (last)
        {
          if (memcmp(last, a, last_len < a_len ? last_len : a_len) >= 0)
            {
              ok = false;
              L_ERR("ordering error - %s >= %s",
                    TLV_REPR(last), TLV_REPR(a));
            }
        }
      last = a;
      last_len = a_len;
      /* XXX - some better way to determine recursion? */
      switch (tlv_id(a))
        {
        case DNCP_T_NODE_DATA:
          sanity_check_buf(tlv_data(a)+dhs, tlv_len(a)-dhs);
          break;
        }
    }
  sput_fail_unless(ok, "tlv ordering valid");

}


void _message_deliver_cb(struct uloop_timeout *t)
{
  net_msg m = container_of(t, net_msg_s, deliver_to);
  dncp o = m->l->dncp;
  net_node node = container_of(o, net_node_s, n);

  list_del(&m->h);
  list_add(&m->h, &node->messages);
  dncp_poll(&node->n);
}

void _sendto(net_sim s, void *buf, size_t len, dncp_link sl, dncp_link dl,
             const struct in6_addr *dst)
{
  net_msg m = calloc(1, sizeof(*m));

  sput_fail_unless(m, "calloc neigh");
  m->l = dl;
  m->buf = malloc(len);
  sput_fail_unless(m->buf, "malloc buf");
  memcpy(m->buf, buf, len);
  m->len = len;
  memset(&m->src, 0, sizeof(m->src));
  m->src.sin6_family = AF_INET6;
  m->src.sin6_addr = sl->ipv6_address;
  m->src.sin6_scope_id = dl->ifindex;
  m->dst = *dst;
  list_add(&m->h, &s->messages);
  m->deliver_to.cb = _message_deliver_cb;
  uloop_timeout_set(&m->deliver_to, MESSAGE_PROPAGATION_DELAY);

#if L_LEVEL >= 7
  dncp o = dl->dncp;
  net_node node1 = container_of(sl->dncp, net_node_s, n);
  net_node node2 = container_of(dl->dncp, net_node_s, n);
  bool is_multicast = memcmp(dst, &o->profile_data.multicast_address,
                             sizeof(*dst)) == 0;
  L_DEBUG("sendto: %s/%s -> %s/%s (%d bytes %s)",
          node1->name, sl->ifname, node2->name, dl->ifname, (int)len,
          is_multicast ? "multicast" : "unicast");
#endif /* L_LEVEL >= 7 */
}

ssize_t dncp_io_sendto(dncp o, void *buf, size_t len,
                       const struct sockaddr_in6 *dst)
{
  net_node node = container_of(o, net_node_s, n);
  net_sim s = node->s;
  sput_fail_unless(dst->sin6_scope_id, "scope id must be set");
  dncp_link l = dncp_find_link_by_id(o, dst->sin6_scope_id);
  bool is_multicast = memcmp(&dst->sin6_addr, &o->profile_data.multicast_address, sizeof(o->profile_data.multicast_address)) == 0;
  net_neigh n;

  if (!l)
    return -1;

  L_DEBUG("dncp_io_sendto: %s -> " SA6_F,
          is_multicast ? "multicast" : "unicast", SA6_D(dst));
  sanity_check_buf(buf, len);
  if (is_multicast)
    {
      s->sent_multicast++;
      sput_fail_unless(len <= HNCP_MAXIMUM_MULTICAST_SIZE, "not too long multicast");
    }
  else
    {
      s->sent_unicast++;
      s->last_unicast_sent = hnetd_time();
    }
  int sent = 0;
  list_for_each_entry(n, &s->neighs, h)
    {
      if (n->src == l
          && (is_multicast
              || (memcmp(&n->dst->ipv6_address, &dst->sin6_addr,
                         sizeof(dst->sin6_addr)) == 0)))
        {
          _sendto(s, buf, len, n->src, n->dst, &dst->sin6_addr);
          sent++;
        }
    }
  /* Loop at self too, just for fun. */
  if (is_multicast)
    _sendto(s, buf, len, l, l, &dst->sin6_addr);
  else
    sput_fail_unless(sent <= 1, "unicast must hit only one target");

  return 1;
}

hnetd_time_t dncp_io_time(dncp o)
{
  return hnetd_time();
}

#endif /* NET_SIM_H */
