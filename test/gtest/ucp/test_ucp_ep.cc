/**
 * Copyright (c) NVIDIA CORPORATION & AFFILIATES, 2001-2026. ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#include "ucp_test.h"
#include <common/test_helpers.h>
#include <ucp/core/ucp_context.h>

extern "C" {
#include <ucp/core/ucp_ep.inl>
#include <ucp/wireup/wireup.h>
}

class test_ucp_ep : public ucp_test {
public:
    static void get_test_variants(std::vector<ucp_test_variant> &variants)
    {
        add_variant(variants, UCP_FEATURE_TAG);
    }

    /// @override
    virtual void init()
    {
        ucp_test::init();
        sender().connect(&receiver(), get_ep_params());
    }
};

UCS_TEST_P(test_ucp_ep, ucp_query_ep)
{
    ucp_ep_h ep;
    ucs_status_t status;
    ucp_ep_evaluate_perf_param_t param;
    ucp_ep_evaluate_perf_attr_t attr;
    double estimated_time_0, estimated_time_1000;

    param.field_mask   = UCP_EP_PERF_PARAM_FIELD_MESSAGE_SIZE;
    attr.field_mask    = UCP_EP_PERF_ATTR_FIELD_ESTIMATED_TIME;
    param.message_size = 0;
    create_entity();

    ep     = sender().ep();
    status = ucp_ep_evaluate_perf(ep, &param, &attr);

    EXPECT_EQ(status, UCS_OK);
    EXPECT_GE(attr.estimated_time, 0);
    estimated_time_0 = attr.estimated_time;

    param.message_size = 1000;
    status             = ucp_ep_evaluate_perf(ep, &param, &attr);
    EXPECT_EQ(status, UCS_OK);
    EXPECT_GT(attr.estimated_time, 0);
    EXPECT_LT(attr.estimated_time, 10);
    estimated_time_1000 = attr.estimated_time;

    param.message_size = 2000;
    status             = ucp_ep_evaluate_perf(ep, &param, &attr);
    EXPECT_EQ(status, UCS_OK);
    EXPECT_GT(attr.estimated_time, 0);
    EXPECT_LT(attr.estimated_time, 10);

    /* Test time estimation sanity, by verifying constant increase per message
       size (which represents current calculation model) */
    EXPECT_FLOAT_EQ(attr.estimated_time - estimated_time_1000,
                    estimated_time_1000 - estimated_time_0);
}

UCS_TEST_P(test_ucp_ep, ucp_query_transport)
{
    ucs_status_t status;
    int i;
    ucp_ep_attr_t ep_attrs;
    std::vector<ucp_transport_entry_t> transport_entries(100);

    ep_attrs.field_mask             = UCP_EP_ATTR_FIELD_TRANSPORTS;
    ep_attrs.transports.entries     = &transport_entries[0];
    ep_attrs.transports.num_entries = transport_entries.size();
    ep_attrs.transports.entry_size  = sizeof(ucp_transport_entry_t);
    status                          = ucp_ep_query(sender().ep(), &ep_attrs);

    // Verify ucp_ep_query completed successfully, that the number of
    // transports is updated, since no system should reasonably have
    // 100 transport/device name pairs, and verify returned strings are
    // not empty.
    ASSERT_UCS_OK(status);
    EXPECT_LT(ep_attrs.transports.num_entries, 100);
    UCS_TEST_MESSAGE << "Number of transport/device name pairs: "
                     << ep_attrs.transports.num_entries;
    for (i = 0; i < ep_attrs.transports.num_entries; i++) {
        UCS_TEST_MESSAGE << "Transport[" << i << "] transport="
                         << ep_attrs.transports.entries[i].transport_name
                         << " device="
                         << ep_attrs.transports.entries[i].device_name;
        EXPECT_STRNE(ep_attrs.transports.entries[i].transport_name, "");
        EXPECT_STRNE(ep_attrs.transports.entries[i].device_name, "");
    }
}

UCP_INSTANTIATE_TEST_CASE(test_ucp_ep);

class test_ucp_ep_recovery : public test_ucp_ep {
protected:
    ucp_ep_params_t get_ep_params() override
    {
        ucp_ep_params_t params = test_ucp_ep::get_ep_params();

        params.field_mask    |= UCP_EP_PARAM_FIELD_ERR_HANDLING_MODE |
                                UCP_EP_PARAM_FIELD_ERR_HANDLER;
        params.err_mode       = UCP_ERR_HANDLING_MODE_FAILOVER;
        params.err_handler.cb = [](void *, ucp_ep_h, ucs_status_t) {};
        return params;
    }

    void wait_for_failure(ucp_ep_h ep)
    {
        wait_for_cond([ep]() {
            return ep->flags & UCP_EP_FLAG_FAILED;
        }, [this]() {
            short_progress_loop();
        });
        EXPECT_TRUE(ep->flags & UCP_EP_FLAG_FAILED);
    }
};

UCS_TEST_P(test_ucp_ep_recovery, recovery_during_close,
           "PROTO_INDIRECT_ID=n")
{
    ucp_ep_h closed_ep = sender().ep();
    ucp_ep_h peer_ep;
    ucp_lane_map_t lane_map;

    EXPECT_TRUE(closed_ep->flags & UCP_EP_FLAG_INDIRECT_ID);
    receiver().connect(&sender(), get_ep_params());
    flush_workers();
    peer_ep  = receiver().ep();
    lane_map = UCS_BIT(ucp_ep_config(peer_ep)->key.am_lane);

    {
        ucs::scoped_async_lock lock(closed_ep->worker->async);
        ASSERT_UCS_OK(ucp_ep_recovery_arm(closed_ep));
        ucp_ep_update_flags(closed_ep, UCP_EP_FLAG_CLOSED, 0);
        EXPECT_EQ(1, ucp_ep_recovery_progress(closed_ep));
    }

    ucp_wireup_send_lanes_addr_msg(peer_ep,
                                   UCP_WIREUP_MSG_LANES_ADDR_REQUEST, lane_map,
                                   lane_map);
    wait_for_failure(peer_ep);

    ucp_ep_set_lanes_failed_schedule(closed_ep, lane_map,
                                     UCS_ERR_CONNECTION_RESET);
    wait_for_failure(closed_ep);

    ucs::scoped_async_lock lock(closed_ep->worker->async);
    ucp_ep_update_flags(closed_ep, 0, UCP_EP_FLAG_CLOSED);
}

UCS_TEST_P(test_ucp_ep_recovery, exhausted_stays_idle)
{
    ucp_ep_h ep              = sender().ep();
    ucp_ep_config_key_t *key = &ucp_ep_config(ep)->key;
    ucp_lane_index_t lane    = key->am_lane;
    ucp_lane_type_mask_t lane_types;

    ASSERT_NE(UCP_NULL_LANE, lane);
    ASSERT_EQ(NULL, ep->ext->recovery_arg);

    lane_types = key->lanes[lane].lane_types;
    key->lanes[lane].lane_types |= UCS_BIT(UCP_LANE_TYPE_FAILED);
    EXPECT_EQ(0, ucp_ep_recovery_progress(ep));
    key->lanes[lane].lane_types = lane_types;
}

UCP_INSTANTIATE_TEST_CASE_TLS(test_ucp_ep_recovery, tcp, "tcp")
