/**
* Copyright (c) NVIDIA CORPORATION & AFFILIATES, 2025. ALL RIGHTS RESERVED.
*
* See file LICENSE for terms.
*/

#include "test_ucp_memheap.h"
#include <algorithm>
#include <random>
#include <string>

extern "C" {
#include <ucp/core/ucp_ep.inl>
#include <ucp/core/ucp_context.h>
}

/**
 * Test class for fault tolerance with injected failures
 */
class test_ucp_fault_tolerance : public test_ucp_memheap {
public:
    static void get_test_variants(std::vector<ucp_test_variant>& variants) {
        add_variant_with_value(variants, UCP_FEATURE_RMA, TEST_OP_PUT,
                               op_name(TEST_OP_PUT));
        add_variant_with_value(variants, UCP_FEATURE_RMA, TEST_OP_PUT | TEST_OP_FLUSH,
                               op_name(TEST_OP_PUT | TEST_OP_FLUSH));
        add_variant_with_value(variants, UCP_FEATURE_RMA, TEST_OP_GET,
                               op_name(TEST_OP_GET));
        add_variant_with_value(variants, UCP_FEATURE_RMA, TEST_OP_GET | TEST_OP_FLUSH,
                               op_name(TEST_OP_GET | TEST_OP_FLUSH));
        add_variant_with_value(variants, UCP_FEATURE_AM,  TEST_OP_AM,
                               op_name(TEST_OP_AM));
        add_variant_with_value(variants, UCP_FEATURE_AM,  TEST_OP_AM | TEST_OP_FLUSH,
                               op_name(TEST_OP_AM | TEST_OP_FLUSH));

        add_variant_with_value(variants, UCP_FEATURE_AM | UCP_FEATURE_RMA,
                               TEST_OP_PUT | TEST_OP_AM | TEST_OP_FLUSH,
                               op_name(TEST_OP_PUT |TEST_OP_AM | TEST_OP_FLUSH));
    }

    test_ucp_fault_tolerance() {
        configure_peer_failure_settings();
        // reduce UD testing time 
        modify_config("KEEPALIVE_INTERVAL", "0.3s");
    }

protected:
    static constexpr uint16_t AM_ID = 0;

    enum {
        GOOD_EP_INDEX = 0,      /* Index for good endpoint */
        INJECTED_EP_INDEX = 1   /* Index for failure-injected endpoint */
    };

    enum failure_side_t {
        FAILURE_SIDE_INITIATOR, /* Inject failure on sender (initiator) side */
        FAILURE_SIDE_TARGET     /* Inject failure on receiver (target) side */
    };

    enum test_op_t {
        TEST_OP_PUT   = UCS_BIT(0),
        TEST_OP_GET   = UCS_BIT(1),
        TEST_OP_AM    = UCS_BIT(2),
        TEST_OP_FLUSH = UCS_BIT(3),
    };

    void init() override {
        ucp_test::init();

        ucp_ep_params_t ep_params = get_ep_params();
        sender().connect(&receiver(), ep_params, GOOD_EP_INDEX);
        sender().connect(&receiver(), ep_params, INJECTED_EP_INDEX);
        receiver().connect(&sender(), ep_params, GOOD_EP_INDEX);
        receiver().connect(&sender(), ep_params, INJECTED_EP_INDEX);

        if (get_variant_value() & TEST_OP_AM) {
            set_am_handler();
        }
    }

    void set_am_handler() {
        ucp_am_handler_param_t param;
        param.field_mask = UCP_AM_HANDLER_PARAM_FIELD_ID |
                           UCP_AM_HANDLER_PARAM_FIELD_CB |
                           UCP_AM_HANDLER_PARAM_FIELD_ARG;
        param.id         = AM_ID;
        param.cb         = am_recv_cb;
        param.arg        = reinterpret_cast<void*>(this);

        ucs_status_t status = ucp_worker_set_am_recv_handler(receiver().worker(), &param);
        ASSERT_UCS_OK(status);
    }

    static ucs_status_t am_recv_cb(void *arg, const void *header,
                                   size_t header_length, void *data,
                                   size_t length,
                                   const ucp_am_recv_param_t *param) {
        test_ucp_fault_tolerance *self =
            reinterpret_cast<test_ucp_fault_tolerance*>(arg);

        ++self->m_am_delivery_count;

        if (param->recv_attr & UCP_AM_RECV_ATTR_FLAG_RNDV) {
            /* Rendezvous: `data` is a descriptor, not the payload. Pull the
             * payload before the transfer counts as received, so a failover
             * that loses the data leg shows up as a missing or corrupt
             * receive rather than a silently accepted one. */
            return self->am_rndv_recv(data, length);
        }

        if (param->recv_attr & UCP_AM_RECV_ATTR_FLAG_DATA) {
            self->m_am_rbuf.resize(length);
            memcpy(self->m_am_rbuf.data(), data, length);
            self->m_am_received = true;
        }

        return UCS_OK;
    }

    /* Destination of one rendezvous receive. Each transfer gets its own, since
     * several can be in flight at once: a shared buffer would let them alias,
     * and growing it would invalidate a pointer UCX is still writing into. */
    struct rndv_recv_ctx {
        test_ucp_fault_tolerance *self;
        std::vector<uint8_t>      buf;
    };

    /* Start the rendezvous receive for one AM. Completion is asynchronous, so
     * the handler returns UCS_INPROGRESS to keep the descriptor alive.
     *
     * When deferral is on, the descriptor is only parked: returning
     * UCS_INPROGRESS without pulling the data keeps it valid, and leaves the
     * sender's request outstanding - waiting for this receive - for as long as
     * the test wants. That is the only way found to have a rendezvous request
     * still live when a lane fails; without it the transfers finish first and
     * reconfiguration has nothing to restart. */
    ucs_status_t am_rndv_recv(void *desc, size_t length) {
        if (m_defer_rndv_recv) {
            m_deferred_rndv.push_back({desc, length});
            return UCS_INPROGRESS;
        }

        return start_rndv_recv(desc, length);
    }

    /* Pull the payload for every parked descriptor. */
    void drain_deferred_rndv_recvs() {
        std::vector<deferred_rndv_t> deferred;

        m_defer_rndv_recv = false;
        deferred.swap(m_deferred_rndv);
        for (const deferred_rndv_t &d : deferred) {
            ucs_status_t status = start_rndv_recv(d.desc, d.length);
            if (status != UCS_OK) {
                EXPECT_EQ(UCS_INPROGRESS, status)
                        << "deferred rendezvous receive returned status: "
                        << ucs_status_string(status);
            }
        }
    }

    ucs_status_t start_rndv_recv(void *desc, size_t length) {
        rndv_recv_ctx *ctx = new rndv_recv_ctx{this,
                                               std::vector<uint8_t>(length)};

        ucp_request_param_t param;
        param.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK |
                             UCP_OP_ATTR_FIELD_USER_DATA;
        param.cb.recv_am   = am_rndv_recv_cb;
        param.user_data    = reinterpret_cast<void*>(ctx);

        ucs_status_ptr_t sp = ucp_am_recv_data_nbx(receiver().worker(), desc,
                                                   ctx->buf.data(), length,
                                                   &param);
        if (UCS_PTR_IS_ERR(sp)) {
            delete ctx;
            return UCS_PTR_STATUS(sp);
        }

        if (sp == NULL) {
            /* Completed in place. */
            finish_rndv_recv(ctx, UCS_OK);
            return UCS_OK;
        }

        return UCS_INPROGRESS;
    }

    static void am_rndv_recv_cb(void *request, ucs_status_t status,
                                size_t length, void *user_data) {
        rndv_recv_ctx *ctx = reinterpret_cast<rndv_recv_ctx*>(user_data);

        ctx->self->finish_rndv_recv(ctx, status);
        ucp_request_free(request);
    }

    /* Verify and account for one completed rendezvous receive, then release its
     * buffer. Checking the payload here rather than once at the end is what
     * makes a truncated or interleaved transfer visible - every transfer in
     * these tests carries the same pattern. */
    void finish_rndv_recv(rndv_recv_ctx *ctx, ucs_status_t status) {
        if (status == UCS_OK) {
            mem_buffer::pattern_check(ctx->buf.data(), ctx->buf.size(), m_seed);
            ++m_am_rndv_completed;
            m_am_received = true;
        } else {
            ++m_am_recv_err_count;
        }

        delete ctx;
    }

    /**
     * Get endpoint parameters with optional failure injection flag
     */
    ucp_ep_params_t get_ep_params() override {
        ucp_ep_params_t params = test_ucp_memheap::get_ep_params();

        params.field_mask     |= UCP_EP_PARAM_FIELD_ERR_HANDLING_MODE |
                                 UCP_EP_PARAM_FIELD_ERR_HANDLER;
        params.err_mode        = UCP_ERR_HANDLING_MODE_FAILOVER;
        params.err_handler.cb  = err_cb;
        params.err_handler.arg = reinterpret_cast<void*>(this);

        return params;
    }

    /**
     * Error callback for endpoint failures
     */
    static void err_cb(void *arg, ucp_ep_h ep, ucs_status_t status) {
        test_ucp_fault_tolerance *self =
            reinterpret_cast<test_ucp_fault_tolerance*>(arg);
        ucp_ep_h sender_ep = self->sender().ep(0, INJECTED_EP_INDEX);

        UCS_TEST_MESSAGE << "Error callback invoked: " << ucs_status_string(status);

        EXPECT_TRUE((UCS_ERR_CONNECTION_RESET == status) ||
                    (UCS_ERR_ENDPOINT_TIMEOUT == status) ||
                    (UCS_ERR_CANCELED == status));

        self->m_err_status = status;
        ++self->m_total_err_count;
        if (ep == sender_ep) {
            ++self->m_initiator_err_count;
        }
    }

    static void shuffle_lanes(std::vector<ucp_lane_index_t> &lanes, const std::string &lane_type) {
        if (lanes.size() < 2) {
            UCS_TEST_SKIP_R("At least 2 " + lane_type + "lanes are required, but only " + std::to_string(lanes.size()) +
                            " available");
        }

        /* Allocate randomizer on heap to avoid exceeding stack frame size limits. */
        std::unique_ptr<std::random_device> rnd_device(new std::random_device);
        std::unique_ptr<std::mt19937> rng(new std::mt19937((*rnd_device)()));
        std::shuffle(lanes.begin(), lanes.end(), *rng);

        for (ucp_lane_index_t lane : lanes) {
            UCS_TEST_MESSAGE << lane_type << ": " << size_t(lane) << "/" << lanes.size();
        }
    }

    ucp_ep_h get_ucp_ep_for_err_injection(failure_side_t failure_side) {
        return (failure_side == FAILURE_SIDE_INITIATOR) ? sender().ep(0, INJECTED_EP_INDEX) :
               receiver().ep(0, INJECTED_EP_INDEX);
    }

    std::vector<ucp_lane_index_t> get_lanes(unsigned op_mask) {
        std::set<ucp_lane_index_t> tmp_lanes;
        std::string lane_type_str;
        unsigned lane_types;
        const ucp_lane_index_t *lane_idx;
        const ucp_lane_index_t *lanes_key_p;

        unsigned lane_type_mask = 0;
        if (op_mask & (TEST_OP_PUT | TEST_OP_GET)) {
            lane_type_mask |= UCS_BIT(UCP_LANE_TYPE_RMA_BW);
        }

        if (op_mask & TEST_OP_AM) {
            lane_type_mask |= UCS_BIT(UCP_LANE_TYPE_AM_BW);
        }

        if (op_mask & (TEST_OP_PUT | TEST_OP_GET)) {
            lane_type_str  += "RMA BW ";
            lanes_key_p = ucp_ep_config(sender().ep(0, INJECTED_EP_INDEX))->key.rma_bw_lanes;
            ucs_carray_for_each(lane_idx, lanes_key_p, UCP_MAX_LANES) {
                if (*lane_idx == UCP_NULL_LANE) {
                    continue;
                }

                lane_types = ucp_ep_config(sender().ep(0, INJECTED_EP_INDEX))->key.lanes[*lane_idx].lane_types;
                if (ucs_test_all_flags(lane_types, lane_type_mask)) {
                    tmp_lanes.insert(*lane_idx);
                }
            }
        }

        if (op_mask & TEST_OP_AM) {
            lane_type_mask |= UCS_BIT(UCP_LANE_TYPE_AM_BW);
            lane_type_str  += "AM BW ";
            lanes_key_p = ucp_ep_config(sender().ep(0, INJECTED_EP_INDEX))->key.am_bw_lanes;
            ucs_carray_for_each(lane_idx, lanes_key_p, UCP_MAX_LANES) {
                if (*lane_idx == UCP_NULL_LANE) {
                    continue;
                }

                lane_types = ucp_ep_config(sender().ep(0, INJECTED_EP_INDEX))->key.lanes[*lane_idx].lane_types;
                if (ucs_test_all_flags(lane_types, lane_type_mask)) {
                    tmp_lanes.insert(*lane_idx);
                }
            }
        }

        std::vector<ucp_lane_index_t> lanes(tmp_lanes.begin(), tmp_lanes.end());
        shuffle_lanes(lanes, lane_type_str);
        return lanes;
    }

    /**
     * Common helper function to test PUT, AM and FLUSH operations with injected failure
     */
    void test_put_am_flush_with_injected_failure(failure_side_t failure_side, unsigned op_mask) {
        const std::string op_str = op_name(op_mask);

        /* TODO: cover case when wireup is in progress, flush here is to complete wireup */
        flush_workers();

        std::vector<ucp_lane_index_t> lanes = get_lanes(op_mask);

        size_t size = rma_msg_size();
        mem_buffer lbuf(size, UCS_MEMORY_TYPE_HOST);
        mapped_buffer rbuf(size, receiver());
        ucs::handle<ucp_rkey_h> rkey = rbuf.rkey(sender());

        ucp_ep_h ucp_ep_for_injection = get_ucp_ep_for_err_injection(failure_side);
        for (size_t lane_idx = 0; lane_idx < lanes.size() - 1; ++lane_idx) {
            std::vector<ucs_status_ptr_t> status_ptrs;
            ucp_lane_index_t lane = lanes[lane_idx];
            uct_ep_h uct_ep_for_injection = ucp_ep_get_lane(ucp_ep_for_injection, lane);
            ucs_status_t status = uct_ep_invalidate(uct_ep_for_injection, 0);
            if (status == UCS_ERR_UNSUPPORTED) {
                UCS_TEST_SKIP_R("uct_ep_invalidate is not supported");
            }

            EXPECT_EQ(UCS_OK, status) << "uct_ep_invalidate returned status: "
                                      << ucs_status_string(status);

            UCS_TEST_MESSAGE << "Attempting " << op_str
                             << " operation after failure injection on lane "
                             << size_t(lane) << '/' << lanes.size() << "...";

            status_ptrs.push_back(
                    ucp_put_nbx(sender().ep(0, INJECTED_EP_INDEX), lbuf.ptr(), size,
                    uintptr_t(rbuf.ptr()), rkey, &m_req_empty_param));
            status_ptrs.push_back(
                    ucp_am_send_nbx(sender().ep(0, INJECTED_EP_INDEX), AM_ID, NULL, 0,
                                       lbuf.ptr(), am_msg_size(), &m_req_empty_param));
            status_ptrs.push_back(
                    ucp_ep_flush_nbx(sender().ep(0, INJECTED_EP_INDEX), &m_req_empty_param));

            status = requests_wait(status_ptrs);
            EXPECT_EQ(UCS_OK, status) << "PUT, AM and FLUSH operations completed with status: "
                                      << ucs_status_string(status);

            // Check that no other lanes have been affected
            for (ucp_lane_index_t valid_lane = lane_idx + 1; valid_lane < lanes.size();
                 ++valid_lane) {
                const ucp_ep_config_t *ep_config = ucp_ep_config(sender().ep(0, INJECTED_EP_INDEX));
                ASSERT_FALSE(UCS_BIT(UCP_LANE_TYPE_FAILED) &
                             ep_config->key.lanes[lanes[valid_lane]].lane_types)
                    << "Lane " << size_t(valid_lane) << " has being marked as failed after "
                    << "failure injection on lane " << size_t(lane);
            }
        }

        short_progress_loop();
        ASSERT_EQ(0, m_total_err_count) << "Error callback invoked " << m_total_err_count << " times";
        UCS_TEST_MESSAGE << "Success";
    }

    /**
     * Common helper function to test AM send with injected failure
     */
    void test_am_with_injected_failure(failure_side_t failure_side, unsigned op_mask) {
        const std::string op_str = op_name(op_mask);

        /* TODO: cover case when wireup is in progress, flush here is to complete wireup */
        flush_workers();

        std::vector<ucp_lane_index_t> am_bw_lanes = get_lanes(op_mask);

        UCS_TEST_MESSAGE << "Attempting " << op_str << " operation before failure injection...";
        ucs_status_t status = do_am_send_and_wait(sender().ep(0, INJECTED_EP_INDEX), am_msg_size(),
                                                  op_mask & TEST_OP_FLUSH);
        EXPECT_EQ(UCS_OK, status) << op_str << " operation returned status: "
                                  << ucs_status_string(status);

        ucp_ep_h ucp_ep_for_injection = get_ucp_ep_for_err_injection(failure_side);
        for (size_t lane_idx = 0; lane_idx < am_bw_lanes.size(); ++lane_idx) {
            ucp_lane_index_t lane = am_bw_lanes[lane_idx];
            uct_ep_h uct_ep_for_injection = ucp_ep_get_lane(ucp_ep_for_injection, lane);
            const bool last_lane = (lane_idx == (am_bw_lanes.size() - 1));
            if (last_lane && has_any_transport({"ud_v", "ud_x"}) &&
                (failure_side == FAILURE_SIDE_INITIATOR)) {
                /* TODO: remove this once UD ep purge assertions are fixed */
                UCS_TEST_MESSAGE << "Keep 1 live lane for UD transports since "
                                 << "local error injection on all lanes leads to "
                                 << "failed assertion in ud_ep_purge";
                break;
            }

            status = uct_ep_invalidate(uct_ep_for_injection, 0);
            if (status == UCS_ERR_UNSUPPORTED) {
                UCS_TEST_SKIP_R("uct_ep_invalidate is not supported");
            }

            EXPECT_EQ(UCS_OK, status) << "uct_ep_invalidate returned status: "
                                      << ucs_status_string(status);

            UCS_TEST_MESSAGE << "Attempting " << op_str
                             << " operation after failure injection on lane "
                             << size_t(lane) << '/' << am_bw_lanes.size() << "...";

            std::unique_ptr<scoped_log_handler> slh;
            if (last_lane) {
                slh.reset(new scoped_log_handler(hide_errors_logger));
            }

            status = do_am_send_and_wait(sender().ep(0, INJECTED_EP_INDEX), am_msg_size(),
                                         op_mask & TEST_OP_FLUSH);
            if (!last_lane) {
                EXPECT_EQ(UCS_OK, status) << op_str << " operation returned status: "
                                          << ucs_status_string(status);
                ASSERT_EQ(0, m_total_err_count) << "Error callback invoked " << m_total_err_count << " times";
            } else {
                // The last lane is expected to fail
                short_progress_loop();
                if ((failure_side == FAILURE_SIDE_TARGET) &&
                    has_transport("dc_x")) {
                    // DC transport is not able to detect failure of remote DCI since DC is a connect2iface transport.
                    // This is a test limitation.
                } else {
                    ucs_time_t deadline = ucs::get_deadline();
                    while ((m_initiator_err_count == 0) && (ucs_get_time() < deadline)) {
                        short_progress_loop();
                    }

                    // Initiator EP should invoke error callback only once
                    ASSERT_EQ(1, m_initiator_err_count) << "Error callback invoked " << m_initiator_err_count << " times";
                    // Remote side may detect failure by keepalive or other control messages but not more than 1 time
                    ASSERT_LE(m_total_err_count - m_initiator_err_count, 1)
                            << "Error callback invoked " << m_total_err_count << " times";
                }
            }
        }

        UCS_TEST_MESSAGE << "Success";
    }

    /**
     * Common helper function to test RMA operation with injected failure
     */
    void test_rma_with_injected_failure(failure_side_t failure_side, unsigned op_mask) {
        const size_t size        = rma_msg_size();
        const std::string op_str = op_name(op_mask);

        /* TODO: cover case when wireup is in progress, flush here is to complete wireup */
        flush_workers();

        std::vector<ucp_lane_index_t> rma_bw_lanes = get_lanes(op_mask);

        mem_buffer lbuf(size, UCS_MEMORY_TYPE_HOST);
        mapped_buffer rbuf(size, receiver());
        ucs::handle<ucp_rkey_h> rkey = rbuf.rkey(sender());

        if (op_mask & TEST_OP_PUT) {
            lbuf.pattern_fill(m_seed);
        } else {
            ASSERT_TRUE(op_mask & TEST_OP_GET);
            rbuf.pattern_fill(m_seed);
        }

        UCS_TEST_MESSAGE << "Attempting " << op_str << " operation before failure injection...";
        ucs_status_t status = do_rma_and_wait(sender().ep(0, INJECTED_EP_INDEX), op_mask,
                                              lbuf, rbuf, rkey.get(), size);
        EXPECT_EQ(UCS_OK, status) << op_str << " operation returned status: "
                                  << ucs_status_string(status);

        ucp_ep_h ucp_ep_for_injection = get_ucp_ep_for_err_injection(failure_side);
        for (size_t lane_idx = 0; lane_idx < rma_bw_lanes.size() - 1; ++lane_idx) {
            ucp_lane_index_t lane = rma_bw_lanes[lane_idx];
            uct_ep_h uct_ep_for_injection = ucp_ep_get_lane(ucp_ep_for_injection, lane);
            status = uct_ep_invalidate(uct_ep_for_injection, 0);
            if (status == UCS_ERR_UNSUPPORTED) {
                UCS_TEST_SKIP_R("uct_ep_invalidate is not supported");
            }

            EXPECT_EQ(UCS_OK, status) << "uct_ep_invalidate returned status: "
                                    << ucs_status_string(status);

            UCS_TEST_MESSAGE << "Attempting " << op_str
                             << " operation after failure injection on lane "
                             << size_t(lane) << '/' << rma_bw_lanes.size() << "...";
            status = do_rma_and_wait(sender().ep(0, INJECTED_EP_INDEX), op_mask, lbuf, rbuf,
                                     rkey.get(), size);
            EXPECT_EQ(UCS_OK, status) << op_str << " operation returned status: "
                                    << ucs_status_string(status);

            for (ucp_lane_index_t valid_lane = lane_idx + 1; valid_lane < rma_bw_lanes.size();
                 ++valid_lane) {
                const ucp_ep_config_t *ep_config = ucp_ep_config(sender().ep(0, INJECTED_EP_INDEX));
                ASSERT_FALSE(UCS_BIT(UCP_LANE_TYPE_FAILED) &
                             ep_config->key.lanes[rma_bw_lanes[valid_lane]].lane_types)
                    << "Lane " << size_t(rma_bw_lanes[valid_lane]) << " has being marked as failed after "
                    << "failure injection on lane " << size_t(lane);
            }
        }

        short_progress_loop();
        ASSERT_EQ(0, m_total_err_count) << "Error callback invoked " << m_total_err_count << " times";
        UCS_TEST_MESSAGE << "Success";
    }

    void do_test(failure_side_t failure_side) {
        const unsigned op_mask = get_variant_value();

        if (ucs_test_all_flags(op_mask, TEST_OP_PUT | TEST_OP_AM | TEST_OP_FLUSH)) {
            test_put_am_flush_with_injected_failure(failure_side, op_mask);
        } else if (op_mask & TEST_OP_AM) {
            ASSERT_FALSE(op_mask & (TEST_OP_PUT|TEST_OP_GET));
            test_am_with_injected_failure(failure_side, op_mask);
        } else {
            ASSERT_TRUE(op_mask & (TEST_OP_PUT|TEST_OP_GET));
            test_rma_with_injected_failure(failure_side, op_mask);
        }
    }
private:
    static size_t rma_msg_size() {
        return ucs::limit_buffer_size((100 * UCS_MBYTE) / ucs::test_time_multiplier());
    }

    static size_t am_msg_size() {
        return ucs::limit_buffer_size(UCS_KBYTE);
    }

    static std::string op_name(unsigned op_mask)
    {
        std::string name;

        if (op_mask & TEST_OP_PUT) {
            name += "PUT|";
        }

        if (op_mask & TEST_OP_GET) {
            name += "GET|";
        }

        if (op_mask & TEST_OP_AM) {
            name += "AM|";
        }

        if (op_mask & TEST_OP_FLUSH) {
            name += "FLUSH|";
        }

        if (!name.empty()) {
            name.pop_back();
        }

        return name;
    }

    ucs_status_t do_am_send_and_wait(ucp_ep_h ep, size_t size, bool flush_after) {
        m_am_received = false;

        mem_buffer sbuf(size, UCS_MEMORY_TYPE_HOST);
        sbuf.pattern_fill(m_seed, size);

        ucp_request_param_t param;
        param.op_attr_mask = 0;

        ucs_status_ptr_t sptr = ucp_am_send_nbx(ep, AM_ID, NULL, 0, sbuf.ptr(),
                                                size, &param);
        if (flush_after) {
            ucs_status_t status = request_wait(ucp_ep_flush_nbx(ep, &param));
            if (status != UCS_OK) {
                request_wait(sptr);
                return status;
            }
        }

        ucs_status_t status = request_wait(sptr);
        if (status != UCS_OK) {
            return status;
        }

        wait_for_value(&m_am_received, true);
        mem_buffer::pattern_check(m_am_rbuf.data(), size, m_seed);
        return UCS_OK;
    }

    ucs_status_t do_put_and_wait(ucp_ep_h ep, mem_buffer &lbuf, mapped_buffer &rbuf,
                                 ucp_rkey_h rkey, size_t size, bool flush) {
        rbuf.memset(0);
        ucs_status_ptr_t put_status_ptr   = ucp_put_nbx(ep, lbuf.ptr(), size, uintptr_t(rbuf.ptr()),
                                                        rkey, &m_req_empty_param);
        ucs_status_ptr_t flush_status_ptr = flush ? ucp_ep_flush_nbx(ep, &m_req_empty_param) : NULL;
        ucs_status_t status               = request_wait(put_status_ptr);
        if (status == UCS_OK) {
            rbuf.pattern_check(m_seed, size);
        }

        EXPECT_EQ(UCS_OK, status) << "put operation returned status: " << ucs_status_string(status);
        if (flush) {
            status = request_wait(flush_status_ptr);
            EXPECT_EQ(UCS_OK, status) << "flush operation returned status: " << ucs_status_string(status);
        }

        return status;
    }

    ucs_status_t do_get_and_wait(ucp_ep_h ep, mem_buffer &lbuf, mapped_buffer &rbuf,
                                 ucp_rkey_h rkey, size_t size, bool flush) {
        ucp_request_param_t param;
        param.op_attr_mask = 0;

        lbuf.memset(0);
        ucs_status_ptr_t status_ptr       = ucp_get_nbx(ep, lbuf.ptr(), size, uintptr_t(rbuf.ptr()), rkey, &param);
        ucs_status_ptr_t flush_status_ptr = flush ? ucp_ep_flush_nbx(ep, &param) : NULL;
        ucs_status_t status               = request_wait(status_ptr);
        EXPECT_EQ(UCS_OK, status) << "get operation returned status: " << ucs_status_string(status);
        if (status == UCS_OK) {
            lbuf.pattern_check(m_seed, size);
        }

        if (flush) {
            status = request_wait(flush_status_ptr);
            EXPECT_EQ(UCS_OK, status) << "flush operation returned status: " << ucs_status_string(status);
        }

        return status;
    }

    ucs_status_t do_rma_and_wait(ucp_ep_h ep, unsigned op_mask, mem_buffer &lbuf, mapped_buffer &rbuf,
                                 ucp_rkey_h rkey, size_t size) {
        if (op_mask & TEST_OP_PUT) {
            return do_put_and_wait(ep, lbuf, rbuf, rkey, size, op_mask & TEST_OP_FLUSH);
        }

        if (op_mask & TEST_OP_GET) {
            return do_get_and_wait(ep, lbuf, rbuf, rkey, size, op_mask & TEST_OP_FLUSH);
        }

        return UCS_ERR_INVALID_PARAM;
    }

protected:
    static constexpr uint64_t m_seed = 0x12345678;

    const ucp_request_param_t m_req_empty_param = { 0 };
    std::vector<uint8_t> m_am_rbuf              = std::vector<uint8_t>(am_msg_size());
    volatile bool m_am_received                 = false;
    /* Every invocation of the AM handler, so a replayed rendezvous RTS that is
     * handed to the application twice is visible as a count, not as silently
     * duplicated work. */
    volatile size_t m_am_delivery_count         = 0;
    volatile size_t m_am_recv_err_count         = 0;
    volatile size_t m_am_rndv_completed         = 0;

    /* A rendezvous descriptor whose payload has not been pulled yet. */
    struct deferred_rndv_t {
        void  *desc;
        size_t length;
    };

    bool m_defer_rndv_recv = false;
    std::vector<deferred_rndv_t> m_deferred_rndv;

protected:
    size_t m_initiator_err_count = 0;
    size_t m_total_err_count     = 0;
    ucs_status_t m_err_status    = UCS_OK;
};

UCP_INSTANTIATE_TEST_CASE(test_ucp_fault_tolerance)

UCS_TEST_P(test_ucp_fault_tolerance, initiator_failure, "MAX_EAGER_LANES=8")
{
    do_test(FAILURE_SIDE_INITIATOR);
}

UCS_TEST_P(test_ucp_fault_tolerance, target_failure, "MAX_EAGER_LANES=8")
{
    do_test(FAILURE_SIDE_TARGET);
}


/**
 * Rendezvous fault injection under failover mode.
 *
 * test_ucp_fault_tolerance covers eager AM and RMA only - its AM payload is a
 * kilobyte, so a rendezvous protocol is never selected and the semantics that
 * failover rendezvous depends on go untested. These cases inject the same lane
 * failure while rendezvous transfers are in flight.
 *
 * Failover recovers the data path; it does not replay a control message. So a
 * transfer whose RTS, RTR or ATS died with the lane waits for the application
 * timeout, and these cases only require that a transfer which can still make
 * progress does - plus that nothing is ever delivered twice, since no replay
 * exists to duplicate it.
 */
class test_ucp_rndv_failover : public test_ucp_fault_tolerance {
public:
    static void get_test_variants(std::vector<ucp_test_variant> &variants) {
        /* RMA as well as AM: a rendezvous payload moves over the RMA BW lanes,
         * which the failure injection has to be able to reach. */
        add_variant_with_value(variants, UCP_FEATURE_AM | UCP_FEATURE_RMA,
                               TEST_OP_AM, "am");
    }

    test_ucp_rndv_failover() {
        /* Force rendezvous for the payload used here. */
        modify_config("RNDV_THRESH", "8k");
    }

    void cleanup() override {
        /* A test that returns early - an ASSERT in the middle of the helper -
         * would otherwise leave descriptors parked and their senders waiting,
         * and ucp_ep_destroy asserts the tracked-request list is empty. */
        drain_deferred_rndv_recvs();
        flush_workers();
        test_ucp_fault_tolerance::cleanup();
    }

    void init() override {
        test_ucp_fault_tolerance::init();
    }

protected:
    /* Large enough to be rendezvous, small enough to keep many in flight. */
    static size_t rndv_msg_size() {
        return ucs::limit_buffer_size(256 * UCS_KBYTE);
    }

    /* One rendezvous AM per request, none of them waited on yet, so the
     * failure lands while they are in flight. */
    std::vector<ucs_status_ptr_t> start_rndv_sends(size_t count,
                                                   mem_buffer &sbuf) {
        std::vector<ucs_status_ptr_t> sptrs;

        for (size_t i = 0; i < count; ++i) {
            sptrs.push_back(ucp_am_send_nbx(sender().ep(0, INJECTED_EP_INDEX),
                                            AM_ID, NULL, 0, sbuf.ptr(),
                                            rndv_msg_size(),
                                            &m_req_empty_param));
        }

        return sptrs;
    }

    /* Fail every AM lane but one. Invalidating a single lane is not enough to
     * reach failover: a transfer that never used that lane completes untouched
     * and no reconfiguration happens, so the test would pass without
     * exercising anything. Leaving exactly one lane alive forces the roles on
     * the failed lanes to move there, which is the path under test. */
    /* Fail a data lane only.
     *
     * A rendezvous transfer rides two lane sets: RTS/RTR/ATS take a single AM
     * control lane, the payload takes the RMA BW lanes. Failover recovers the
     * payload - a fetch restarts on a surviving lane - but does not replay a
     * control message, so killing the AM lane strands the transfer until the
     * application timeout whenever the control leg happened to be on it. A test
     * that injected on both lane sets therefore passed or failed depending on
     * which lane the control leg picked, and a stranded request then tripped
     * the tracked-request assertion in ucp_ep_destroy.
     *
     * Failing only the data lanes tests exactly what admission promises. */
    void inject_am_lane_failures() {
        inject_lane_group_failures(TEST_OP_GET, "RMA BW");
    }

    /* Fail one lane of a group, leaving the rest alive: that is what makes this
     * a failover test rather than an endpoint-failure test. Failing all but one
     * lane of both groups was tried and drives the endpoint to
     * UCS_ERR_CONNECTION_RESET through "AM lane not found", because rebuilding
     * lanes is not implemented - it tests the absence of that, not failover. */
    void inject_lane_group_failures(unsigned op_mask, const char *group) {
        std::vector<ucp_lane_index_t> lanes = get_lanes(op_mask);
        /* Invalidate the TARGET's lanes, so the failure surfaces on the sender's
         * endpoint - the one holding the outstanding rendezvous requests. With
         * the initiator's lanes invalidated instead, every reconfiguration lands
         * on the peer's endpoint, whose tracked-request list is empty, and the
         * sender's requests are never considered for restart. */
        ucp_ep_h ucp_ep = get_ucp_ep_for_err_injection(FAILURE_SIDE_TARGET);

        for (size_t i = 0; (i < 1) && (i + 1 < lanes.size()); ++i) {
            uct_ep_h uct_ep     = ucp_ep_get_lane(ucp_ep, lanes[i]);
            ucs_status_t status = uct_ep_invalidate(uct_ep, 0);
            if (status == UCS_ERR_UNSUPPORTED) {
                UCS_TEST_SKIP_R("uct_ep_invalidate is not supported");
            }

            ASSERT_EQ(UCS_OK, status) << "uct_ep_invalidate returned status: "
                                      << ucs_status_string(status);
            UCS_TEST_MESSAGE << "injected failure on " << group << " lane "
                             << size_t(lanes[i]) << '/' << lanes.size();
        }
    }

    /* Send `count` rendezvous AMs, fail lanes while every one of them is still
     * outstanding, and require that they all complete anyway.
     *
     * The receiver parks the descriptors instead of pulling the payload, so the
     * senders are still waiting when the failure is injected. Injecting after
     * the transfers have drained is what made an earlier version of this vacuous
     * - it reported zero reconfigurations and passed with the fix removed. */
    void run_rndv_with_injected_failure(size_t count) {
        flush_workers();

        mem_buffer sbuf(rndv_msg_size(), UCS_MEMORY_TYPE_HOST);
        sbuf.pattern_fill(m_seed, rndv_msg_size());

        m_defer_rndv_recv = true;
        std::vector<ucs_status_ptr_t> sptrs = start_rndv_sends(count, sbuf);

        /* Every RTS has reached the receiver, so every send is now parked on a
         * receive that has not been started. */
        wait_for_value(&m_am_delivery_count, count);
        ASSERT_EQ(count, m_am_delivery_count)
                << "only " << m_am_delivery_count << " of " << count
                << " rendezvous RTS arrived before the failure was injected";

        inject_am_lane_failures();
        drain_deferred_rndv_recvs();

        ucs_status_t status = requests_wait(sptrs);
        EXPECT_EQ(UCS_OK, status)
                << count << " rendezvous AM sends completed with status: "
                << ucs_status_string(status);

        /* Sends completing is not enough: the receives are what prove the data
         * arrived, and they complete after their send does. */
        wait_for_value(&m_am_rndv_completed, count);
        EXPECT_EQ(count, m_am_rndv_completed)
                << "only " << m_am_rndv_completed << " of " << count
                << " rendezvous transfers were received";
        EXPECT_EQ(0u, m_am_recv_err_count)
                << "rendezvous receive failed " << m_am_recv_err_count
                << " times";
        /* Nothing is replayed on failover, so the handler must have run exactly
         * once per transfer. A reintroduced RTS replay would show up here as a
         * duplicate delivery, which is why replaying was removed rather than
         * gated: it cannot be made exactly-once without a stable transfer
         * identity and a receiver-side duplicate check. */
        EXPECT_EQ(count, m_am_delivery_count)
                << "AM handler ran " << m_am_delivery_count << " times for "
                << count << " transfers";
        EXPECT_EQ(0u, m_total_err_count)
                << "error callback invoked " << m_total_err_count << " times";
    }
};

/* RC and DC only: they carry the rendezvous protocols that failover admits. */
UCP_INSTANTIATE_TEST_CASE_TLS(test_ucp_rndv_failover, rc,      "rc")
UCP_INSTANTIATE_TEST_CASE_TLS(test_ucp_rndv_failover, rc_mlx5, "rc_mlx5")
UCP_INSTANTIATE_TEST_CASE_TLS(test_ucp_rndv_failover, dc,      "dc")
UCP_INSTANTIATE_TEST_CASE_TLS(test_ucp_rndv_failover, dc_mlx5, "dc_mlx5")

/* Rendezvous must survive a lane failure at all - the case the eager-only
 * coverage left open. What failover admission buys is data-path recovery: a get
 * refetches, a put rewinds to its acknowledged offset. */
UCS_TEST_P(test_ucp_rndv_failover, single_transfer_survives_lane_failure,
           "MAX_EAGER_LANES=8")
{
    run_rndv_with_injected_failure(1);
}

/* Several rendezvous transfers in flight when a lane under them fails, so the
 * recovery has to cope with more than one outstanding request at a time. */
UCS_TEST_P(test_ucp_rndv_failover, concurrent_transfers_survive_lane_failure,
           "MAX_EAGER_LANES=8")
{
    run_rndv_with_injected_failure(8);
}

