/* Run with PSM3_DEVICES=self and FI_PSM3_NAME_SERVER=1. */
#include <rdma/fabric.h>
#include <rdma/fi_domain.h>
#include <rdma/fi_endpoint.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(condition) do { \
	if (!(condition)) { \
		fprintf(stderr, "line %d: %s\n", __LINE__, #condition); \
		abort(); \
	} \
} while (0)

static int resolve(struct fi_info *hints, const char *service)
{
	struct fi_info *info = NULL;
	int status = fi_getinfo(FI_VERSION(2, 7), "127.0.0.1", service,
			       0, hints, &info);
	fi_freeinfo(info);
	return status;
}

static void exercise(uint64_t caps, bool scalable)
{
	struct fi_info *hints = fi_allocinfo();
	struct fi_info *info;
	struct fid_fabric *fabric;
	struct fid_domain *domain;
	char service[32];
	fprintf(stderr, "PSM3 caps=%llu scalable=%d\n",
		(unsigned long long) caps, scalable);

	CHECK(hints != NULL);
	hints->ep_attr->type = FI_EP_RDM;
	hints->ep_attr->tx_ctx_cnt = scalable ? 2 : 1;
	hints->ep_attr->rx_ctx_cnt = scalable ? 2 : 1;
	hints->caps = caps;
	hints->mode = FI_CONTEXT;
	hints->fabric_attr->prov_name = strdup("psm3");
	CHECK(hints->fabric_attr->prov_name != NULL);
	CHECK(snprintf(service, sizeof(service), "%u", (unsigned) getpid()) > 0);
	CHECK(fi_getinfo(FI_VERSION(2, 7), NULL, service, FI_SOURCE, hints, &info) == 0);
	CHECK(fi_fabric(info->fabric_attr, &fabric, NULL) == 0);
	CHECK(fi_domain(fabric, info, &domain, NULL) == 0);

	/* Reuse a service while its fabric remains open, within the context budget. */
	for (int iteration = 0; iteration < 8; ++iteration) {
		struct fid_ep *endpoint;
		int status;
		CHECK(resolve(hints, service) == -FI_ENODATA);
		if (scalable)
			status = fi_scalable_ep(domain, info, &endpoint, NULL);
		else
			status = fi_endpoint(domain, info, &endpoint, NULL);
		if (status)
			fprintf(stderr, "open iteration %d: %d (%s)\n",
				iteration, status, fi_strerror(-status));
		CHECK(status == 0);
		CHECK(resolve(hints, service) == 0);
		CHECK(fi_close(&endpoint->fid) == 0);
		CHECK(resolve(hints, service) == -FI_ENODATA);
	}

	CHECK(fi_close(&domain->fid) == 0);
	CHECK(fi_close(&fabric->fid) == 0);
	fi_freeinfo(info);
	fi_freeinfo(hints);
}

int main(void)
{
	exercise(FI_MSG, false);
	exercise(FI_MSG, true);
	exercise(FI_MSG | FI_RECV, false);
	exercise(FI_MSG | FI_SEND, false);
	return 0;
}
