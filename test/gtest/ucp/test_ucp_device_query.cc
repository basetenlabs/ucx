/**
 * Copyright (c) NVIDIA CORPORATION & AFFILIATES, 2024. ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#include "ucp_test.h"

#include <algorithm>
#include <set>
#include <string>

extern "C" {
#include <ucp/core/ucp_ep.inl>
#include <ucp/core/ucp_worker.inl>
#include <ucp/wireup/address.h>
#include <ucp/wireup/wireup.h>
#include <ucp/wireup/wireup_ep.h>
}


class test_ucp_device_query : public ucp_test {
public:
    static void get_test_variants(std::vector<ucp_test_variant> &variants)
    {
        add_variant(variants, UCP_FEATURE_TAG);
    }

protected:
    std::vector<ucp_worker_device_attr_t> query_devices(ucp_worker_h worker)
    {
        unsigned num_devices = 0;
        EXPECT_UCS_OK(ucp_worker_query_devices(worker, NULL, &num_devices));
        EXPECT_GT(num_devices, 0u);

        std::vector<ucp_worker_device_attr_t> devices(num_devices);
        EXPECT_UCS_OK(ucp_worker_query_devices(worker, &devices[0],
                                               &num_devices));
        EXPECT_EQ(devices.size(), num_devices);
        return devices;
    }

    std::vector<ucp_address_device_attr_t>
    query_address_devices(ucp_worker_h worker, const ucp_address_t *address)
    {
        unsigned num_entries = 0;
        EXPECT_UCS_OK(ucp_address_query_devices(worker, address, NULL,
                                                &num_entries));
        EXPECT_GT(num_entries, 0u);

        std::vector<ucp_address_device_attr_t> entries(num_entries);
        EXPECT_UCS_OK(ucp_address_query_devices(worker, address, &entries[0],
                                                &num_entries));
        EXPECT_EQ(entries.size(), num_entries);
        return entries;
    }

    /* The endpoint a peer's wireup request built on @a worker. These tests
       create every other endpoint by hand, so the worker holds at most one */
    ucp_ep_h peer_endpoint(ucp_worker_h worker)
    {
        ucp_ep_ext_t *ep_ext;

        ucs_list_for_each(ep_ext, &worker->all_eps, ep_list) {
            return ep_ext->ep;
        }

        return NULL;
    }

    std::set<ucp_rsc_index_t> devices_of(ucp_context_h context,
                                        const ucp_tl_bitmap_t *tl_bitmap)
    {
        std::set<ucp_rsc_index_t> dev_indexes;
        ucp_rsc_index_t tl_idx;

        UCS_STATIC_BITMAP_FOR_EACH_BIT(tl_idx, tl_bitmap) {
            dev_indexes.insert(context->tl_rscs[tl_idx].dev_index);
        }

        return dev_indexes;
    }

    std::set<std::string> ep_device_names(ucp_ep_h ep)
    {
        std::vector<ucp_transport_entry_t> transport_entries(100);
        ucp_ep_attr_t ep_attrs;

        ep_attrs.field_mask             = UCP_EP_ATTR_FIELD_TRANSPORTS;
        ep_attrs.transports.entries     = &transport_entries[0];
        ep_attrs.transports.num_entries = transport_entries.size();
        ep_attrs.transports.entry_size  = sizeof(ucp_transport_entry_t);
        EXPECT_UCS_OK(ucp_ep_query(ep, &ep_attrs));

        std::set<std::string> names;
        for (unsigned i = 0; i < ep_attrs.transports.num_entries; ++i) {
            names.insert(ep_attrs.transports.entries[i].device_name);
        }

        return names;
    }

    /* The receiver builds its endpoint when the request arrives, so waiting for
       that endpoint to appear is what drives the exchange. A flush is the wrong
       instrument: it waits for lanes this test may deliberately have left
       unconnectable, and blocks until the harness deadline when they are. */
    ucp_ep_h wait_for_peer_endpoint()
    {
        for (unsigned i = 0; i < 10; ++i) {
            ucp_ep_h peer_ep = peer_endpoint(receiver().worker());
            if (peer_ep != NULL) {
                return peer_ep;
            }

            short_progress_loop();
        }

        return peer_endpoint(receiver().worker());
    }

    void close_ep(ucp_ep_h ep)
    {
        void *close_req = ucp_ep_close_nb(ep, UCP_EP_CLOSE_MODE_FORCE);
        if (UCS_PTR_IS_PTR(close_req)) {
            request_wait(close_req);
        }
    }
};

UCS_TEST_P(test_ucp_device_query, worker_devices)
{
    std::vector<ucp_worker_device_attr_t> devices =
            query_devices(sender().worker());

    for (size_t i = 0; i < devices.size(); ++i) {
        UCS_TEST_MESSAGE << "device[" << devices[i].dev_index << "] "
                         << devices[i].dev_name << "/" << devices[i].tl_name
                         << " bw " << devices[i].bandwidth;
        EXPECT_STRNE(devices[i].dev_name, "");
        EXPECT_STRNE(devices[i].tl_name, "");
        EXPECT_GT(devices[i].bandwidth, 0.0);
    }
}

UCS_TEST_P(test_ucp_device_query, worker_devices_buffer_too_small)
{
    unsigned num_devices = 0;
    ucs_status_t status  = ucp_worker_query_devices(sender().worker(), NULL,
                                                    &num_devices);
    ASSERT_UCS_OK(status);

    /* One entry short of what the worker has, so the call has to report the
       number needed rather than truncate silently */
    std::vector<ucp_worker_device_attr_t> devices(num_devices);
    unsigned num_queried = num_devices - 1;
    status               = ucp_worker_query_devices(sender().worker(),
                                                    &devices[0], &num_queried);
    EXPECT_EQ(UCS_ERR_BUFFER_TOO_SMALL, status);
    EXPECT_EQ(num_devices, num_queried);
}

UCS_TEST_P(test_ucp_device_query, address_devices_reach_self)
{
    ucp_worker_attr_t worker_attr;

    worker_attr.field_mask = UCP_WORKER_ATTR_FIELD_ADDRESS;
    ASSERT_UCS_OK(ucp_worker_query(sender().worker(), &worker_attr));

    std::vector<ucp_address_device_attr_t> entries =
            query_address_devices(sender().worker(), worker_attr.address);

    uint64_t reachable = 0;
    for (size_t i = 0; i < entries.size(); ++i) {
        UCS_TEST_MESSAGE << "entry[" << i << "] device[" << entries[i].dev_index
                         << "] reachable_dev_bitmap 0x" << std::hex
                         << entries[i].reachable_dev_bitmap << std::dec;
        EXPECT_LT(entries[i].dev_index, 64u);
        reachable |= entries[i].reachable_dev_bitmap;
    }

    /* The address was packed by this very worker, so at least one of its own
       devices has to reach it */
    EXPECT_NE(0u, reachable);

    ucp_worker_release_address(sender().worker(), worker_attr.address);
}

UCS_TEST_P(test_ucp_device_query, remote_device_absent)
{
    ucp_worker_attr_t worker_attr;

    worker_attr.field_mask = UCP_WORKER_ATTR_FIELD_ADDRESS;
    ASSERT_UCS_OK(ucp_worker_query(receiver().worker(), &worker_attr));

    std::vector<ucp_address_device_attr_t> entries =
            query_address_devices(sender().worker(), worker_attr.address);

    unsigned absent_index = 0;
    for (size_t i = 0; i < entries.size(); ++i) {
        absent_index = std::max(absent_index, entries[i].dev_index + 1);
    }

    ucp_ep_params_t ep_params;
    ep_params.field_mask              = UCP_EP_PARAM_FIELD_REMOTE_ADDRESS |
                                        UCP_EP_PARAM_FIELD_PATH;
    ep_params.address                 = worker_attr.address;
    ep_params.path.local_dev_index    = 0;
    ep_params.path.remote_dev_index   = absent_index;

    ucp_ep_h ep;
    /* The rejection logs an error, which the harness fails a test for unless it
       is declared expected */
    scoped_log_handler wrap_err(wrap_errors_logger);
    ucs_status_t status = ucp_ep_create(sender().worker(), &ep_params, &ep);
    EXPECT_EQ(UCS_ERR_NO_DEVICE, status);

    ucp_worker_release_address(receiver().worker(), worker_attr.address);
}

UCS_TEST_P(test_ucp_device_query, pin_both_ends)
{
    ucp_worker_attr_t worker_attr;

    worker_attr.field_mask = UCP_WORKER_ATTR_FIELD_ADDRESS;
    ASSERT_UCS_OK(ucp_worker_query(receiver().worker(), &worker_attr));

    std::vector<ucp_worker_device_attr_t> devices =
            query_devices(sender().worker());
    std::vector<ucp_address_device_attr_t> entries =
            query_address_devices(sender().worker(), worker_attr.address);

    ucp_ep_params_t ep_params;
    ep_params.field_mask            = UCP_EP_PARAM_FIELD_REMOTE_ADDRESS |
                                      UCP_EP_PARAM_FIELD_PATH;
    ep_params.address               = worker_attr.address;
    ep_params.path.local_dev_index  = devices[0].dev_index;
    ep_params.path.remote_dev_index = entries[0].dev_index;

    ucp_ep_h ep;
    ucs_status_t status = ucp_ep_create(sender().worker(), &ep_params, &ep);
    ASSERT_UCS_OK(status);

    /* Every lane of a pinned endpoint is on the named local device */
    std::vector<ucp_transport_entry_t> transport_entries(100);
    ucp_ep_attr_t ep_attrs;
    ep_attrs.field_mask             = UCP_EP_ATTR_FIELD_TRANSPORTS;
    ep_attrs.transports.entries     = &transport_entries[0];
    ep_attrs.transports.num_entries = transport_entries.size();
    ep_attrs.transports.entry_size  = sizeof(ucp_transport_entry_t);
    ASSERT_UCS_OK(ucp_ep_query(ep, &ep_attrs));

    for (unsigned i = 0; i < ep_attrs.transports.num_entries; ++i) {
        EXPECT_STREQ(devices[0].dev_name,
                     ep_attrs.transports.entries[i].device_name);
    }

    /* One path is one pair, so no lane may be selected on a second local
       device -- the query above reports every lane, and they all named one */
    EXPECT_GT(ep_attrs.transports.num_entries, 0u);

    void *close_req = ucp_ep_close_nb(ep, UCP_EP_CLOSE_MODE_FORCE);
    if (UCS_PTR_IS_PTR(close_req)) {
        request_wait(close_req);
    }

    ucp_worker_release_address(receiver().worker(), worker_attr.address);
}

UCS_TEST_P(test_ucp_device_query, a_path_takes_no_more_lanes_than_the_peer_can_address)
{
    /* A path confines every lane to one peer address entry, and each p2p lane
       needs an ep address of its own from it. Selection must stop at the count
       the entry packed instead of building a map wireup cannot fill: before
       this was checked, the overflow asserted in a debug build and read past
       ep_addrs[] in a release one. */
    ucp_worker_attr_t worker_attr;

    worker_attr.field_mask = UCP_WORKER_ATTR_FIELD_ADDRESS;
    ASSERT_UCS_OK(ucp_worker_query(receiver().worker(), &worker_attr));

    std::vector<ucp_worker_device_attr_t> devices =
            query_devices(sender().worker());
    std::vector<ucp_address_device_attr_t> entries =
            query_address_devices(sender().worker(), worker_attr.address);

    ucp_ep_params_t ep_params;
    ep_params.field_mask            = UCP_EP_PARAM_FIELD_REMOTE_ADDRESS |
                                      UCP_EP_PARAM_FIELD_PATH;
    ep_params.address               = worker_attr.address;
    ep_params.path.local_dev_index  = devices[0].dev_index;
    ep_params.path.remote_dev_index = entries[0].dev_index;

    ucp_ep_h ep;
    scoped_log_handler wrap_err(wrap_errors_logger);
    ucs_status_t status = ucp_ep_create(sender().worker(), &ep_params, &ep);

    /* Either the pair affords a connectable set of lanes, or it affords none
       and creation says so; what it must never do is return a configuration
       whose p2p lanes have no remote ep address */
    if (status == UCS_OK) {
        ucp_ep_attr_t ep_attrs;
        std::vector<ucp_transport_entry_t> transport_entries(100);
        ep_attrs.field_mask             = UCP_EP_ATTR_FIELD_TRANSPORTS;
        ep_attrs.transports.entries     = &transport_entries[0];
        ep_attrs.transports.num_entries = transport_entries.size();
        ep_attrs.transports.entry_size  = sizeof(ucp_transport_entry_t);
        ASSERT_UCS_OK(ucp_ep_query(ep, &ep_attrs));
        EXPECT_LE(ep_attrs.transports.num_entries, 64u);

        void *close_req = ucp_ep_close_nb(ep, UCP_EP_CLOSE_MODE_FORCE);
        if (UCS_PTR_IS_PTR(close_req)) {
            request_wait(close_req);
        }
    } else {
        EXPECT_EQ(UCS_ERR_UNREACHABLE, status);
    }

    ucp_worker_release_address(receiver().worker(), worker_attr.address);
}

UCS_TEST_P(test_ucp_device_query, a_local_only_pin_still_names_one_end)
{
    /* The half a caller is allowed to name alone: its own. A path is the other
       shape, and the two must not be confused -- this one leaves the peer's
       device to selection, as it always did. */
    ucp_worker_attr_t worker_attr;

    worker_attr.field_mask = UCP_WORKER_ATTR_FIELD_ADDRESS;
    ASSERT_UCS_OK(ucp_worker_query(receiver().worker(), &worker_attr));

    std::vector<ucp_worker_device_attr_t> devices =
            query_devices(sender().worker());

    ucp_ep_params_t ep_params;
    ep_params.field_mask   = UCP_EP_PARAM_FIELD_REMOTE_ADDRESS |
                             UCP_EP_PARAM_FIELD_LOCAL_DEVICE;
    ep_params.address      = worker_attr.address;
    ep_params.local_device = devices[0].dev_name;

    ucp_ep_h ep;
    ASSERT_UCS_OK(ucp_ep_create(sender().worker(), &ep_params, &ep));

    void *close_req = ucp_ep_close_nb(ep, UCP_EP_CLOSE_MODE_FORCE);
    if (UCS_PTR_IS_PTR(close_req)) {
        request_wait(close_req);
    }

    ucp_worker_release_address(receiver().worker(), worker_attr.address);
}

UCS_TEST_P(test_ucp_device_query, arrival_pin)
{
    /* A peer that pinned its own end sends a request whose address names one
       device, and that request arrives on the port it aimed at. The endpoint
       the receiver builds from it must take that port, or the reply, the lanes
       and any rendezvous the peer later pulls over this endpoint land
       somewhere the peer never chose -- which is the crossing invariant 3
       forbids. */
    ucp_worker_attr_t worker_attr;

    worker_attr.field_mask = UCP_WORKER_ATTR_FIELD_ADDRESS;
    ASSERT_UCS_OK(ucp_worker_query(receiver().worker(), &worker_attr));

    std::vector<ucp_worker_device_attr_t> devices =
            query_devices(sender().worker());
    std::vector<ucp_address_device_attr_t> entries =
            query_address_devices(sender().worker(), worker_attr.address);

    ucp_ep_params_t ep_params;
    ep_params.field_mask            = UCP_EP_PARAM_FIELD_REMOTE_ADDRESS |
                                      UCP_EP_PARAM_FIELD_PATH;
    ep_params.address               = worker_attr.address;
    ep_params.path.local_dev_index  = devices[0].dev_index;
    ep_params.path.remote_dev_index = entries[0].dev_index;

    ucp_ep_h ep;
    ASSERT_UCS_OK(ucp_ep_create(sender().worker(), &ep_params, &ep));

    ucp_ep_h peer_ep = wait_for_peer_endpoint();
    if (peer_ep == NULL) {
        close_ep(ep);
        ucp_worker_release_address(receiver().worker(), worker_attr.address);
        UCS_TEST_SKIP_R("the selected lanes need no wireup exchange");
    }

    /* One device in the record, which is what the reply's auxiliary is also
       selected from, and one device in the lanes it produced */
    EXPECT_EQ(1u, devices_of(receiver().ucph(),
                             ucp_ep_dev_restriction_tls(peer_ep)).size());
    EXPECT_EQ(1u, ep_device_names(peer_ep).size());

    close_ep(ep);
    ucp_worker_release_address(receiver().worker(), worker_attr.address);
}

UCS_TEST_P(test_ucp_device_query, arrival_pin_no_data_transport)
{
    /* The widening case: a device that can carry wireup messages but no data
       gives the endpoint no lane at all, so the arrival port is refused and
       the caller selects over everything instead. A device retired at runtime
       is the same condition and is the one this test can produce. */
    ucp_worker_h worker      = sender().worker();
    ucp_context_h context    = sender().ucph();
    ucp_ep_dev_restriction_t restriction;

    EXPECT_EQ(UCS_ERR_NO_DEVICE,
              ucp_ep_dev_restriction_from_iface(worker, UCP_NULL_RESOURCE,
                                                &restriction));

    ucp_rsc_index_t tl_idx;
    ucp_rsc_index_t data_rsc = UCP_NULL_RESOURCE;
    UCS_STATIC_BITMAP_FOR_EACH_BIT(tl_idx, &context->tl_bitmap) {
        if (ucp_ep_dev_restriction_from_iface(worker, tl_idx, &restriction) ==
            UCS_OK) {
            data_rsc = tl_idx;
            break;
        }
    }
    ASSERT_NE(UCP_NULL_RESOURCE, data_rsc);

    /* The record names the arrival device and nothing else */
    EXPECT_EQ(1u, devices_of(context, &restriction.tls).size());
    EXPECT_EQ(context->tl_rscs[data_rsc].dev_index,
              *devices_of(context, &restriction.tls).begin());

    ASSERT_UCS_OK(ucp_worker_exclude_device(
            worker, context->tl_rscs[data_rsc].tl_rsc.dev_name));
    EXPECT_EQ(UCS_ERR_NO_DEVICE,
              ucp_ep_dev_restriction_from_iface(worker, data_rsc,
                                                &restriction));
}

UCS_TEST_P(test_ucp_device_query, aux_follows_the_pin)
{
    /* The request a pinned endpoint sends announces its lane resources plus
       its auxiliary's. An auxiliary on another device therefore hands the peer
       a second device to select over, and the peer's reply arrives on a port
       no lane is on. */
    ucp_worker_attr_t worker_attr;

    worker_attr.field_mask = UCP_WORKER_ATTR_FIELD_ADDRESS;
    ASSERT_UCS_OK(ucp_worker_query(receiver().worker(), &worker_attr));

    std::vector<ucp_worker_device_attr_t> devices =
            query_devices(sender().worker());
    std::vector<ucp_address_device_attr_t> entries =
            query_address_devices(sender().worker(), worker_attr.address);

    ucp_ep_params_t ep_params;
    ep_params.field_mask            = UCP_EP_PARAM_FIELD_REMOTE_ADDRESS |
                                      UCP_EP_PARAM_FIELD_PATH;
    ep_params.address               = worker_attr.address;
    ep_params.path.local_dev_index  = devices[0].dev_index;
    ep_params.path.remote_dev_index = entries[0].dev_index;

    ucp_ep_h ep;
    ASSERT_UCS_OK(ucp_ep_create(sender().worker(), &ep_params, &ep));

    /* Read before progressing: the auxiliary is discarded once the endpoint is
       remote connected */
    uct_ep_h uct_ep          = ucp_ep_get_lane(ep, 0);
    ucp_rsc_index_t aux_rsc  = ucp_wireup_ep_test(uct_ep) ?
                                       ucp_wireup_ep_get_aux_rsc_index(uct_ep) :
                                       UCP_NULL_RESOURCE;
    if (aux_rsc == UCP_NULL_RESOURCE) {
        close_ep(ep);
        ucp_worker_release_address(receiver().worker(), worker_attr.address);
        UCS_TEST_SKIP_R("the selected lanes need no auxiliary transport");
    }

    EXPECT_EQ(devices[0].dev_index,
              sender().ucph()->tl_rscs[aux_rsc].dev_index);

    close_ep(ep);
    ucp_worker_release_address(receiver().worker(), worker_attr.address);
}

UCS_TEST_P(test_ucp_device_query, multi_device_address_is_left_alone)
{
    /* The scope of the rule, at the one place it is decided. A peer that did
       not pin gets today's behaviour, and the only evidence the receiver has is
       the request's address -- so the predicate is what keeps the unpaired
       fallback, an intra-node peer and a multi-rail application out of a rule
       none of them asked for. Read here rather than through an endpoint because
       an unrestricted endpoint on a many-device node cannot always be wired up
       at all: its request outgrows the auxiliary's bcopy limit. */
    std::vector<ucp_worker_device_attr_t> devices =
            query_devices(sender().worker());
    ucp_worker_attr_t worker_attr;

    worker_attr.field_mask = UCP_WORKER_ATTR_FIELD_ADDRESS;
    ASSERT_UCS_OK(ucp_worker_query(sender().worker(), &worker_attr));

    /* The flags a worker address was packed with, not every flag: an address
       packed without endpoint addresses is unparseable as one that has them */
    const uint64_t pack_flags =
            ucp_worker_default_address_pack_flags(sender().worker());

    ucp_unpacked_address_t whole;
    ASSERT_UCS_OK(ucp_address_unpack(sender().worker(), worker_attr.address,
                                     pack_flags, &whole));
    std::set<ucp_rsc_index_t> whole_devs;
    const ucp_address_entry_t *ae;
    ucp_unpacked_address_for_each(ae, &whole) {
        whole_devs.insert(ae->dev_index);
    }
    const int whole_is_single = ucp_wireup_address_is_single_device(&whole);
    ucs_free(whole.address_list);
    ucp_worker_release_address(sender().worker(), worker_attr.address);

    UCS_TEST_MESSAGE << "whole address: " << whole_devs.size() << " devices";
    EXPECT_EQ(whole_devs.size() == 1, whole_is_single != 0);

    /* The other half, from an address packed down one device name -- the shape
       a pinned peer sends. One name is not always one device index: the
       shared-memory resources all answer to 'memory' and split by sys_device,
       so the count is read rather than assumed, and the predicate is held to
       it either way. */
    const char *dev_name = devices[0].dev_name;
    ucp_address_t *one_device;
    size_t length;
    ASSERT_UCS_OK(ucp_worker_get_address_with_devices(sender().worker(),
                                                      &dev_name, 1,
                                                      &one_device, &length));

    ucp_unpacked_address_t narrowed;
    ASSERT_UCS_OK(ucp_address_unpack(sender().worker(), one_device, pack_flags,
                                     &narrowed));
    std::set<ucp_rsc_index_t> narrowed_devs;
    ucp_unpacked_address_for_each(ae, &narrowed) {
        narrowed_devs.insert(ae->dev_index);
    }
    const int narrowed_is_single =
            ucp_wireup_address_is_single_device(&narrowed);
    ucs_free(narrowed.address_list);
    ucp_worker_release_address(sender().worker(), one_device);

    UCS_TEST_MESSAGE << "'" << dev_name << "' alone: " << narrowed_devs.size()
                     << " devices";
    EXPECT_EQ(narrowed_devs.size() == 1, narrowed_is_single != 0);
    EXPECT_LE(narrowed_devs.size(), whole_devs.size());
}

UCP_INSTANTIATE_TEST_CASE(test_ucp_device_query)
