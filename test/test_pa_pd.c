
#include "pa.h"
#include "sput.h"

#define FU_PARANOID_TIMEOUT_CANCEL
#include "fake_uloop.h"

#define FR_MASK_MD5
#include "fake_random.h"
#include "prefixes_library.h"

#include "prefix_utils.c"
#include "pa_data.c"
#include "pa_pd.c"
#include "pa.c"

/* Masking pa_local, pa_core, pa_store and iface dependencies */
void pa_core_init(__unused struct pa_core *c) {}
void pa_core_start(__unused struct pa_core *c) {}
void pa_core_stop(__unused struct pa_core *c) {}
void pa_core_term(__unused struct pa_core *c) {}

void pa_local_conf_defaults(__unused struct pa_local_conf *conf) {}
void pa_local_init(__unused struct pa_local *l, __unused const struct pa_local_conf *conf) {}
void pa_local_start(__unused struct pa_local *l) {}
void pa_local_stop(__unused struct pa_local *l) {}
void pa_local_term(__unused struct pa_local *l) {}

void pa_store_init(__unused struct pa_store *s) {}
void pa_store_start(__unused struct pa_store *store) {}
void pa_store_stop(__unused struct pa_store *store) {}
int pa_store_setfile(__unused struct pa_store *s, __unused const char *filepath) {return 0;}
void pa_store_term(__unused struct pa_store *s) {}

void iface_register_user(__unused struct iface_user *user) {}
void iface_unregister_user(__unused struct iface_user *user) {}



static struct pa pa;
#define pd (&pa.pd)

struct test_lease {
	struct pa_pd_lease lease;
	int update_calls;
} tl1, tl2;

void test_update_cb(struct pa_pd_lease *lease) {
	container_of(lease, struct test_lease, lease)->update_calls++;
}

#define LEASE_ID_1 "lease_id_1"
#define LEASE_ID_2 "lease_id_2"

void test_init_pa()
{
	tl1.lease.update_cb = test_update_cb;
	tl2.lease.update_cb = test_update_cb;

	pa_init(&pa, NULL);
	pa_start(&pa);
}

void test_term_pa()
{
	pa_term(&pa);
}

void test_1()
{
	struct pa_cpl *cpl1;
	struct pa_cpd *cpd;
	struct pa_pd_dp_req *req;
	struct pa_ldp *ldp1, *ldp2;
	struct pa_iface *iface1;
	struct prefix p1 = PL_P1;
	struct prefix p1_01 = PL_P1_01;
	struct prefix p1_04 = PL_P1_04;
	struct prefix p2 = PL_P2;
	struct prefix p2_01 = PL_P2_01;
	struct prefix delegated;

	//hnetd_time_t start = hnetd_time();
	test_init_pa();

	/* A lease with no dp is immediatly called with empty list */
	tl1.update_calls = 0;
	pa_pd_lease_init(pd, &tl1.lease, LEASE_ID_1, 0, 64);
	sput_fail_unless(pd->update.pending, "pd algo is pending");
	sput_fail_unless(uloop_timeout_remaining(&pd->update) == PA_PD_UPDATE_DELAY, "Correct timeout value");
	fu_loop(1); //Execute pd algorithm
	sput_fail_unless(tl1.lease.cb_to.pending, "Lease timeout is pending");
	sput_fail_unless(uloop_timeout_remaining(&tl1.lease.cb_to) == PA_PD_LEASE_CB_DELAY, "Correct timeout value");
	fu_loop(1); //Calling lease
	sput_fail_unless(tl1.update_calls == 1, "One lease update call");
	sput_fail_unless(list_empty(&tl1.lease.cpds), "No cpds");
	sput_fail_unless(list_empty(&tl1.lease.dp_reqs), "No requests");
	sput_fail_unless(fu_next() == NULL, "No next schedule");
	pa_pd_lease_term(pd, &tl1.lease);

	/* Create an iface */
	iface1 = pa_iface_get(&pa.data, PL_IFNAME1, true);
	pa_iface_notify(&pa.data, iface1);

	/* Create a dp */
	sput_fail_unless((ldp1 = pa_ldp_get(&pa.data, &p1, true)), "Created new ldp");
	pa_ldp_set_iface(ldp1, iface1);
	pa_dp_notify(&pa.data, &ldp1->dp);
	sput_fail_unless(fu_next() == NULL, "No next schedule");

	/* Create a cp that takes all the space */
	cpl1 = _pa_cpl(pa_cp_get(&pa.data, &p1, PA_CPT_L, true));
	sput_fail_unless(cpl1, "Create a cpl");
	pa_cp_notify(&cpl1->cp);

	/* Create a request */
	tl1.update_calls = 0;
	pa_pd_lease_init(pd, &tl1.lease, LEASE_ID_1, 0, 64);
	sput_fail_unless(pd->update.pending, "pd algo is pending");
	sput_fail_unless(uloop_timeout_remaining(&pd->update) == PA_PD_UPDATE_DELAY, "Correct timeout value");
	fr_md5_push(&p1_01); //Will be used when trying to get a md5 for the lease
	fu_loop(1);
	sput_fail_unless(tl1.lease.cb_to.pending, "Lease timeout is pending");
	sput_fail_unless(uloop_timeout_remaining(&tl1.lease.cb_to) == PA_PD_LEASE_CB_DELAY, "Correct timeout value");
	fu_loop(1);
	/* Checking if tl1 call is correct */
	sput_fail_unless(tl1.update_calls == 1, "One lease update call");
	sput_fail_unless(list_empty(&tl1.lease.cpds), "No cpds");
	sput_fail_unless((req = list_first_entry(&tl1.lease.dp_reqs, struct pa_pd_dp_req, lease_le)), "One remaining request");
	sput_fail_unless(fu_next() == NULL, "No next schedule");
	/* Let's remove the blocking cp : that should trigger a schedule*/
	pa_cp_todelete(&cpl1->cp);
	pa_cp_notify(&cpl1->cp);
	sput_fail_unless(pd->update.pending, "pd algo is pending");
	sput_fail_unless(uloop_timeout_remaining(&pd->update) ==
			ldp1->dp.compute_leases_last + PA_PD_UPDATE_RATE_DELAY - hnetd_time(), "Timeout value is bounded");
	fr_md5_push(&p1_01); //It will be used to give a /62
	fu_loop(1);
	/* A cpds should have been created, but it is not applied for now */
	sput_fail_unless(list_empty(&tl1.lease.dp_reqs), "No requests in lease");
	sput_fail_unless(list_empty(&ldp1->dp.lease_reqs), "No requests in dp");
	sput_fail_unless(!list_empty(&tl1.lease.cpds), "There is a cpd");
	cpd = list_first_entry(&tl1.lease.cpds, struct pa_cpd, lease_le);
	sput_fail_unless(cpd->cp.dp == &ldp1->dp, "Correct associated dp");
	delegated = p1_01;
	delegated.plen = PA_PD_DFLT_MIN_LEN;
	sput_fail_unless(!prefix_cmp(&delegated, &cpd->cp.prefix), "Correct delegated prefix");

	/* Now let's apply the prefix */
	fu_loop(1); //This is the apply callback
	sput_fail_unless(tl1.lease.cb_to.pending, "Lease timeout is pending");
	sput_fail_unless(uloop_timeout_remaining(&tl1.lease.cb_to) == PA_PD_LEASE_CB_DELAY, "Correct timeout value");
	fu_loop(1);
	sput_fail_unless(tl1.update_calls == 2, "Second lease update call");
	sput_fail_unless(fu_next() == NULL, "No next schedule");

	/* Now let's add a new lease */
	tl2.update_calls = 0;
	pa_pd_lease_init(pd, &tl2.lease, LEASE_ID_2, 63, 64);
	sput_fail_unless(pd->update.pending, "pd algo is pending");
	sput_fail_unless(uloop_timeout_remaining(&pd->update) == PA_PD_UPDATE_DELAY, "Correct timeout value");
	fr_md5_push(&p1_01); //Will be used when trying to get a md5 for the lease: That will make a collision and p1_08 should be used
	fu_loop(1);
	sput_fail_unless(list_empty(&tl2.lease.dp_reqs), "No requests in lease");
	sput_fail_unless(list_empty(&ldp1->dp.lease_reqs), "No requests in dp");
	sput_fail_unless(!list_empty(&tl2.lease.cpds), "There is a cpd");
	cpd = list_first_entry(&tl2.lease.cpds, struct pa_cpd, lease_le);
	sput_fail_unless(cpd->cp.dp == &ldp1->dp, "Correct associated dp");
	delegated = p1_04;
	delegated.plen = 63;
	sput_fail_unless(!prefix_cmp(&delegated, &cpd->cp.prefix), "Correct delegated prefix");
	fu_loop(2); //This is the apply callback and lease cb
	sput_fail_unless(tl2.update_calls == 1, "Second lease update call");
	sput_fail_unless(fu_next() == NULL, "No next schedule");

	/* Now let's delete the second lease */
	pa_pd_lease_term(pd, &tl2.lease);
	sput_fail_unless(fu_next() == NULL, "No next schedule");

	/* And add a new dp */
	sput_fail_unless((ldp2 = pa_ldp_get(&pa.data, &p2, true)), "Created new ldp");
	pa_ldp_set_iface(ldp2, iface1);
	pa_dp_notify(&pa.data, &ldp2->dp);
	sput_fail_unless(pd->update.pending, "pd algo is pending");
	sput_fail_unless(uloop_timeout_remaining(&pd->update) == PA_PD_UPDATE_DELAY, "Correct timeout value");
	fr_md5_push(&p2_01);
	fu_loop(1);
	sput_fail_unless(list_empty(&tl1.lease.dp_reqs), "No requests in lease");
	sput_fail_unless(list_empty(&ldp2->dp.lease_reqs), "No requests in dp");
	sput_fail_unless(!list_empty(&tl1.lease.cpds), "There is a cpd");
	cpd = list_first_entry(&tl1.lease.cpds, struct pa_cpd, lease_le);
	fu_loop(2); //Apply callback and lease cb
	sput_fail_unless(tl1.update_calls == 3, "Second lease update call");

	//todo: Dynamicaly unvalidate the used cp

	test_term_pa();
}


int main(__attribute__((unused)) int argc, __attribute__((unused))char **argv)
{
	openlog("hnetd_test_pa", LOG_PERROR | LOG_PID, LOG_DAEMON);
	fu_init();
	sput_start_testing();
	sput_enter_suite("Prefix assignment prefix delegation (pa_pd.c)"); /* optional */
	sput_run_test(test_1);
	sput_leave_suite(); /* optional */
	sput_finish_testing();
	return sput_get_return_value();
}