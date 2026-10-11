/* Exercise the pinned libfabric utility directly, without a NIC/provider. */
#include <config.h>
#include <ofi_util.h>
#include <rdma/providers/fi_prov.h>

#include <stdio.h>
#include <stdlib.h>

struct fi_provider core_prov = {.name = "name-server-test"};

#define CHECK(condition) do { \
	if (!(condition)) { \
		fprintf(stderr, "line %d: %s\n", __LINE__, #condition); \
		abort(); \
	} \
} while (0)

static int compare_services(void *left, void *right)
{
	int a = *(int *) left;
	int b = *(int *) right;
	return (a > b) - (a < b);
}

static void exercise_server(int count)
{
	struct util_ns ns = {
		.hostname = "127.0.0.1",
		.name_len = sizeof(uint64_t),
		.service_len = sizeof(int),
		.service_cmp = compare_services,
	};
	struct sockaddr_in address;
	socklen_t length = sizeof(address);

	ofi_ns_init(&ns);
	CHECK(ofi_ns_start_server(&ns) == 0);
	CHECK(getsockname(ns.listen_sock, (struct sockaddr *) &address, &length) == 0);
	ns.port = ntohs(address.sin_port);
	CHECK(ns.port != 0);
	CHECK(ofi_ns_start_server(&ns) == 0);

	for (int service = 0; service < count; ++service) {
		uint64_t name = service + 100;
		CHECK(ofi_ns_add_local_name(&ns, &service, &name) == 0);
		uint64_t *resolved = ofi_ns_resolve_name(&ns, ns.hostname, &service);
		CHECK(resolved && *resolved == name);
		free(resolved);
	}

	/* A nonfinal stop must preserve every registration and the server. */
	ofi_ns_stop_server(&ns);
	for (int service = 0; service < count; ++service) {
		uint64_t name = service + 100;
		uint64_t *resolved = ofi_ns_resolve_name(&ns, ns.hostname, &service);
		CHECK(resolved && *resolved == name);
		free(resolved);
		if (service % 2 == 0) {
			CHECK(ofi_ns_del_local_name(&ns, &service, &name) == 0);
			CHECK(ofi_ns_resolve_name(&ns, ns.hostname, &service) == NULL);
		}
	}

	/* LeakSanitizer checks entries left by clients that did not unregister. */
	ofi_ns_stop_server(&ns);
	CHECK(ns.listen_sock == INVALID_SOCKET);
	CHECK(ofi_atomic_get32(&ns.ref) == 0);
}

int main(void)
{
	exercise_server(0);
	exercise_server(1);
	exercise_server(2);
	exercise_server(64);
	return 0;
}
