/*
 * $Id: test_hncp_net.c $
 *
 * Author: Markus Stenberg <mstenber@cisco.com>
 *
 * Copyright (c) 2013 cisco Systems, Inc.
 *
 * Created:       Wed Nov 27 10:41:56 2013 mstenber
 * Last modified: Wed Jan 15 15:39:43 2014 mstenber
 * Edit time:     319 min
 *
 */

/*
 * This is N-node version of the testsuite which leverages net_sim.h.
 */

#ifdef L_LEVEL
#undef L_LEVEL
#endif /* L_LEVEL */

#define L_LEVEL 5

/* Test utilities */
#include "net_sim.h"
#include "sput.h"

/********************************************************* Mocked interfaces */

int pa_update_eap(net_node node, const struct prefix *prefix,
                  const struct pa_rid *rid,
                  const char *ifname, bool to_delete)
{
  sput_fail_unless(prefix, "prefix set");
  sput_fail_unless(rid, "rid set");
  node->updated_eap++;
  return 0;
}

int pa_update_edp(net_node node, const struct prefix *prefix,
                  const struct pa_rid *rid,
                  hnetd_time_t valid_until, hnetd_time_t preferred_until,
                  const void *dhcpv6_data, size_t dhcpv6_len)
{
  sput_fail_unless(prefix, "prefix set");
  sput_fail_unless(rid, "rid set");
  node->updated_edp++;
  return 0;
}

int pa_update_eaa(net_node node, const struct in6_addr *addr,
					const struct pa_rid *rid,
					const char *ifname, bool to_delete)
{return 0;}

/**************************************************************** Test cases */

struct prefix p1 = {
  .prefix = { .s6_addr = {
      0x20, 0x01, 0x00, 0x01}},
  .plen = 54 };

struct prefix p2 = {
  .prefix = { .s6_addr = {
      0x20, 0x02, 0x00, 0x01}},
  .plen = 54 };

void hncp_two(void)
{
  net_sim_s s;
  hncp n1;
  hncp n2;
  hncp_link l1;
  hncp_link l2;
  net_node node1, node2;

  net_sim_init(&s);
  n1 = net_sim_find_hncp(&s, "n1");
  n2 = net_sim_find_hncp(&s, "n2");
  l1 = net_sim_hncp_find_link_by_name(n1, "eth0");
  l2 = net_sim_hncp_find_link_by_name(n2, "eth1");
  sput_fail_unless(avl_is_empty(&l1->neighbors.avl), "no l1 neighbors");
  sput_fail_unless(avl_is_empty(&l2->neighbors.avl), "no l2 neighbors");

  /* connect l1+l2 -> should converge at some point */
  net_sim_set_connected(l1, l2, true);
  net_sim_set_connected(l2, l1, true);
  SIM_WHILE(&s, 100, !net_sim_is_converged(&s));

  sput_fail_unless(n1->nodes.avl.count == 2, "n1 nodes == 2");
  sput_fail_unless(n2->nodes.avl.count == 2, "n2 nodes == 2");


  /* Play with the prefix API. Feed in stuff! */
  node1 = container_of(n1, net_node_s, n);
  node2 = container_of(n2, net_node_s, n);

  /* First, fake delegated prefixes */
  pa_update_ldp(&node1->pa_data, &p1, "eth0", s.now + 123, s.now + 1, NULL, 0);
  pa_update_ldp(&node1->pa_data, &p2, NULL, s.now + 123, s.now + 1, NULL, 0);

  SIM_WHILE(&s, 1000,
            node2->updated_edp != 2);

  /* Then fake prefix assignment */
  p1.plen = 64;
  p2.plen = 64;
  pa_update_lap(&node1->pa_data, &p1, "eth0", false);
  pa_update_lap(&node1->pa_data, &p2, NULL, false);
  SIM_WHILE(&s, 1000,
            node2->updated_eap != 2);

  /* disconnect on one side (=> unidirectional traffic) => should at
   * some point disappear. */
  net_sim_set_connected(l1, l2, false);
  SIM_WHILE(&s, 1000,
            !avl_is_empty(&l2->neighbors.avl));

  /* n1 will keep getting stuff from n2, so it's sometimes alive,
   * sometimes not.. However, network hashes should be again
   * different. */
  sput_fail_unless(memcmp(&n1->network_hash, &n2->network_hash, HNCP_HASH_LEN),
                   "hashes different");

  /* Should also have done the necessary purging of nodes due to lack
   * of reachability.. */
  sput_fail_unless(n2->nodes.avl.count == 1, "n2 nodes == 1");

  net_sim_uninit(&s);
}

/* 11 nodes represented, wired according to how they are wired in the
 * test topology. */
char *nodenames[] = {"cpe", "b1", "b2", "b3", "b4", "b5", "b6",
                     "b7", "b8", "b9", "b10", NULL};
typedef struct {
  int src;
  char *srclink;
  int dst;
  char *dstlink;
} nodeconnection_s;

nodeconnection_s nodeconnections[] = {
  {0, "eth1", 1, "eth0"},
  {0, "eth1", 2, "eth0"},
  {1, "eth1", 5, "eth0"},
  {1, "eth2", 2, "eth1"},
  {1, "eth3", 9, "eth0"},
  {2, "eth2", 3, "eth0"},
  {3, "eth1", 4, "eth0"},
  {4, "eth1", 8, "eth0"},
  {4, "eth1", 9, "eth1"},
  {5, "eth1", 6, "eth0"},
  {6, "eth1", 9, "eth2"},
  {6, "eth2", 7, "eth0"},
  {7, "eth1", 10, "eth0"},
  {8, "eth1", 10, "eth1"},
  {9, "eth3", 10, "eth2"},
};

static void handle_connections(net_sim s,
                               nodeconnection_s *c,
                               int n_conns)
{
  int i;

  for (i = 0 ; i < n_conns ; i++)
    {
      hncp n1 = net_sim_find_hncp(s, nodenames[c->src]);
      hncp_link l1 = net_sim_hncp_find_link_by_name(n1, c->srclink);
      hncp n2 = net_sim_find_hncp(s, nodenames[c->dst]);
      hncp_link l2 = net_sim_hncp_find_link_by_name(n2, c->dstlink);

      net_sim_set_connected(l1, l2, true);
      net_sim_set_connected(l2, l1, true);
      c++;
    }
}

static void raw_bird14(net_sim s)
{
  int num_connections = sizeof(nodeconnections) / sizeof(nodeconnections[0]);

  handle_connections(s, &nodeconnections[0], num_connections);

  SIM_WHILE(s, 10000, !net_sim_is_converged(s));

  sput_fail_unless(net_sim_find_hncp(s, "b10")->nodes.avl.count == 11,
                   "b10 enough nodes");

  sput_fail_unless(s->now - s->start < 10 * HNETD_TIME_PER_SECOND,
                   "should converge in 10 seconds");

  sput_fail_unless(s->sent_multicast < 1000, "with 'few' multicast");

  sput_fail_unless(s->sent_unicast < 2000, "with 'few' unicast");

  net_sim_remove_node_by_name(s, nodenames[0]);

  /* Re-add the node */
  (void)net_sim_find_hncp(s, nodenames[0]);

  handle_connections(s, &nodeconnections[0], 2); /* Two first ones are needed */

  SIM_WHILE(s, 1000, !net_sim_is_converged(s));

  /* Then, simulate network for a while, keeping eye on how often it's
   * NOT converged. */
  int converged_count = s->converged_count;
  int not_converged_count = s->not_converged_count;
  int sent_unicast = s->sent_unicast;
  hnetd_time_t convergence_time = s->now;

  SIM_WHILE(s, 1000, !net_sim_is_converged(s) || iter < 900);
  L_NOTICE("unicasts sent:%d after convergence, last %lld ms after convergence",
           s->sent_unicast - sent_unicast, (long long)(s->last_unicast_sent - convergence_time));
#if 0
  /* As we do reachability checking, this isn't valid.. unfortunately. */
  sput_fail_unless((s->sent_unicast - sent_unicast) < 50,
                   "did not send (many) unicasts");
#endif /* 0 */
  sput_fail_unless(s->not_converged_count == not_converged_count,
                   "should stay converged");
  sput_fail_unless(s->converged_count >= 900 + converged_count,
                   "converged count rising");
}

void hncp_bird14()
{
  net_sim_s s;

  net_sim_init(&s);
  raw_bird14(&s);
  net_sim_uninit(&s);
}

void hncp_bird14_bidir()
{
  net_sim_s s;

  net_sim_init(&s);
  s.assume_bidirectional_reachability = true;
  raw_bird14(&s);
  net_sim_uninit(&s);
}

static void raw_hncp_tube(unsigned int num_nodes)
{
  /* A LOT of routers connected in a tube (R1 R2 R3 .. RN). */
  unsigned int i;
  net_sim_s s;

  net_sim_init(&s);
  s.disable_sd = true;
  for (i = 0 ; i < num_nodes-1 ; i++)
    {
      char buf[128];

      sprintf(buf, "node%d", i);
      hncp n1 = net_sim_find_hncp(&s, buf);

      sprintf(buf, "node%d", i+1);
      hncp n2 = net_sim_find_hncp(&s, buf);

      hncp_link l1 = net_sim_hncp_find_link_by_name(n1, "down");
      hncp_link l2 = net_sim_hncp_find_link_by_name(n2, "up");
      net_sim_set_connected(l1, l2, true);
      net_sim_set_connected(l2, l1, true);
    }
  SIM_WHILE(&s, 10000, !net_sim_is_converged(&s));

  sput_fail_unless(net_sim_find_hncp(&s, "node0")->nodes.avl.count == num_nodes,
                   "enough nodes");

  net_sim_uninit(&s);
}

void hncp_tube_small(void)
{
  raw_hncp_tube(5);
}

void hncp_tube_beyond_multicast(void)
{
  raw_hncp_tube(1400 / (HNCP_HASH_LEN * 2 + TLV_SIZE));
}

int main(__unused int argc, __unused char **argv)
{
  setbuf(stdout, NULL); /* so that it's in sync with stderr when redirected */
  openlog("test_hncp_net", LOG_CONS | LOG_PERROR, LOG_DAEMON);
  sput_start_testing();
  sput_enter_suite("hncp_net"); /* optional */
  sput_run_test(hncp_two);
  sput_run_test(hncp_bird14);
  sput_run_test(hncp_bird14_bidir);
  sput_run_test(hncp_tube_small);
  sput_run_test(hncp_tube_beyond_multicast);
  sput_leave_suite(); /* optional */
  sput_finish_testing();
  return sput_get_return_value();
}