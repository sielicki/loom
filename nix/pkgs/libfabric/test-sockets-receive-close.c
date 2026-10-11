/* SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only */
#include <rdma/fabric.h>
#include <rdma/fi_domain.h>
#include <rdma/fi_endpoint.h>
#include <rdma/fi_tagged.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "%s:%d: %s failed\n", __FILE__, __LINE__, #condition);                 \
            exit(EXIT_FAILURE);                                                                    \
        }                                                                                          \
    } while (0)

static void post(struct fid_ep* ep, bool tagged, char* buffer, struct fi_context2* context) {
    if (tagged) {
        CHECK(0 == fi_trecv(ep, buffer, 1, NULL, FI_ADDR_UNSPEC, 7, 0, context));
    } else {
        CHECK(0 == fi_recv(ep, buffer, 1, NULL, FI_ADDR_UNSPEC, context));
    }
}

static void check_close(bool tagged, size_t count, bool cancel_and_repost) {
    struct fi_info *hints = fi_allocinfo(), *info = NULL;
    struct fid_fabric* fabric;
    struct fid_domain* domain;
    struct fid_ep* ep;
    struct fid_cq* cq;
    struct fid_av* av;
    struct fi_cq_attr cqa = {.format = FI_CQ_FORMAT_TAGGED, .size = 32};
    struct fi_av_attr ava = {.type = FI_AV_MAP};
    struct fi_context2 contexts[12] = {0};
    char buffers[12] = {0};

    CHECK(NULL != hints);
    hints->fabric_attr->prov_name = strdup("sockets");
    CHECK(NULL != hints->fabric_attr->prov_name);
    hints->caps = tagged ? FI_TAGGED : FI_MSG;
    hints->ep_attr->type = FI_EP_RDM;
    hints->domain_attr->control_progress = FI_PROGRESS_MANUAL;
    hints->domain_attr->data_progress = FI_PROGRESS_MANUAL;
    hints->rx_attr->size = 2;
    CHECK(0 == fi_getinfo(FI_VERSION(2, 7), "127.0.0.1", "0", FI_SOURCE, hints, &info));
    fi_freeinfo(hints);
    CHECK(2 == info->rx_attr->size);
    CHECK(0 == fi_fabric(info->fabric_attr, &fabric, NULL));
    CHECK(0 == fi_domain(fabric, info, &domain, NULL));
    CHECK(0 == fi_cq_open(domain, &cqa, &cq, NULL));
    CHECK(0 == fi_av_open(domain, &ava, &av, NULL));
    CHECK(0 == fi_endpoint(domain, info, &ep, NULL));
    CHECK(0 == fi_ep_bind(ep, &cq->fid, FI_TRANSMIT | FI_RECV));
    CHECK(0 == fi_ep_bind(ep, &av->fid, 0));
    CHECK(0 == fi_enable(ep));

    for (size_t i = 0; i < count; ++i) {
        post(ep, tagged, &buffers[i], &contexts[i]);
    }
    if (cancel_and_repost) {
        /* Retire one pooled entry and one overflow entry before reusing
         * their contexts, leaving both kinds of allocation posted at close. */
        const size_t indices[] = {0, count - 1};
        for (size_t i = 0; i < sizeof(indices) / sizeof(indices[0]); ++i) {
            const size_t index = indices[i];
            struct fi_cq_err_entry error = {0};
            CHECK(0 == fi_cancel(&ep->fid, &contexts[index]));
            CHECK(1 == fi_cq_readerr(cq, &error, 0));
            CHECK(FI_ECANCELED == error.err && &contexts[index] == error.op_context);
            post(ep, tagged, &buffers[index], &contexts[index]);
        }
    }

    CHECK(0 == fi_close(&ep->fid));
    CHECK(0 == fi_close(&av->fid));
    CHECK(0 == fi_close(&cq->fid));
    CHECK(0 == fi_close(&domain->fid));
    CHECK(0 == fi_close(&fabric->fid));
    fi_freeinfo(info);
}

int main(void) {
    const size_t counts[] = {0, 1, 2, 12};
    for (unsigned tagged = 0; tagged < 2; ++tagged) {
        for (size_t i = 0; i < sizeof(counts) / sizeof(counts[0]); ++i) {
            check_close(tagged, counts[i], false);
        }
        check_close(tagged, 12, true);
    }
    puts("Sockets receive-close checks passed");
    return EXIT_SUCCESS;
}
