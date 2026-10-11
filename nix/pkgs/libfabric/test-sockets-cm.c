// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <rdma/fabric.h>
#include <rdma/fi_cm.h>
#include <rdma/fi_endpoint.h>
#include <rdma/fi_eq.h>

#include <assert.h>
#include <netinet/in.h>
#include <sched.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static struct fi_eq_cm_entry next_event(struct fid_eq* eq, struct fid_cq* cq, uint32_t expected) {
    struct timespec start, now;
    struct fi_eq_cm_entry entry;
    uint32_t event = 0;
    ssize_t ret;

    assert(!clock_gettime(CLOCK_MONOTONIC, &start));
    for (;;) {
        ret = fi_eq_read(eq, &event, &entry, sizeof(entry), 0);
        if (ret != -FI_EAGAIN)
            break;
        ret = fi_cq_read(cq, NULL, 0);
        assert(ret == 0 || ret == -FI_EAGAIN);
        assert(!clock_gettime(CLOCK_MONOTONIC, &now));
        assert(now.tv_sec - start.tv_sec < 10);
        sched_yield();
    }
    assert(ret >= (ssize_t)sizeof(entry));
    assert(event == expected);
    return entry;
}

int main(void) {
    struct fi_info *hints = fi_allocinfo(), *info = NULL;
    struct fid_fabric* fabric;
    struct fid_domain* domain;
    struct fid_eq *client_eq, *server_eq;
    struct fid_cq* cq;
    struct fid_pep* listener;
    struct fid_ep *client, *server;
    struct fi_eq_attr eq_attr = {.wait_obj = FI_WAIT_NONE};
    struct fi_cq_attr cq_attr = {.size = 16, .format = FI_CQ_FORMAT_MSG, .wait_obj = FI_WAIT_NONE};
    struct sockaddr_in address;
    size_t address_size = sizeof(address);
    struct fi_eq_cm_entry request;

    assert(hints);
    hints->ep_attr->type = FI_EP_MSG;
    hints->caps = FI_MSG;
    hints->mode = FI_CONTEXT | FI_CONTEXT2;
    hints->addr_format = FI_SOCKADDR_IN;
    hints->fabric_attr->prov_name = strdup("sockets");
    assert(hints->fabric_attr->prov_name);
    assert(!fi_getinfo(FI_VERSION(2, 7), "127.0.0.1", "0", FI_SOURCE, hints, &info));
    assert(!fi_fabric(info->fabric_attr, &fabric, NULL));
    assert(!fi_domain(fabric, info, &domain, NULL));
    assert(!fi_eq_open(fabric, &eq_attr, &client_eq, NULL));
    assert(!fi_eq_open(fabric, &eq_attr, &server_eq, NULL));
    assert(!fi_cq_open(domain, &cq_attr, &cq, NULL));
    assert(!fi_passive_ep(fabric, info, &listener, NULL));
    assert(!fi_pep_bind(listener, &server_eq->fid, 0));
    assert(!fi_listen(listener));
    assert(!fi_getname(&listener->fid, &address, &address_size));
    assert(!fi_endpoint(domain, info, &client, NULL));
    assert(!fi_ep_bind(client, &client_eq->fid, 0));
    assert(!fi_ep_bind(client, &cq->fid, FI_TRANSMIT | FI_RECV));
    assert(!fi_enable(client));
    assert(!fi_connect(client, &address, NULL, 0));
    request = next_event(server_eq, cq, FI_CONNREQ);
    assert(request.info);
    assert(request.info->domain_attr->name);
    assert(request.info->fabric_attr->name);
    assert(request.info->fabric_attr->prov_name);
    assert(!fi_endpoint(domain, request.info, &server, NULL));
    assert(!fi_ep_bind(server, &server_eq->fid, 0));
    assert(!fi_ep_bind(server, &cq->fid, FI_TRANSMIT | FI_RECV));
    assert(!fi_enable(server));
    assert(!fi_accept(server, NULL, 0));
    (void)next_event(client_eq, cq, FI_CONNECTED);
    (void)next_event(server_eq, cq, FI_CONNECTED);
    assert(!fi_close(&server->fid));
    assert(!fi_close(&client->fid));
    assert(!fi_close(&listener->fid));
    assert(!fi_close(&cq->fid));
    assert(!fi_close(&server_eq->fid));
    assert(!fi_close(&client_eq->fid));
    assert(!fi_close(&domain->fid));
    assert(!fi_close(&fabric->fid));
    fi_freeinfo(request.info);
    fi_freeinfo(info);
    fi_freeinfo(hints);
    return 0;
}
