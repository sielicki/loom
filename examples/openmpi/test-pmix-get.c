/* SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only */
#include <pmix.h>
#include <pthread.h>
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

static const char payload_key[] = "loom.get.payload";
static const char scalar_key[] = "loom.get.scalar";

static void publish(const pmix_proc_t* self) {
    char bytes[] = {0, (char)(self->rank + 1), 0x7f, (char)0xff, 0};
    pmix_byte_object_t payload = {.bytes = bytes, .size = sizeof(bytes)};
    uint64_t scalar = UINT64_C(123) + self->rank;
    pmix_value_t value;

    PMIX_VALUE_CONSTRUCT(&value);
    CHECK(PMIX_SUCCESS == PMIx_Value_load(&value, &payload, PMIX_BYTE_OBJECT));
    CHECK(PMIX_SUCCESS == PMIx_Put(PMIX_GLOBAL, payload_key, &value));
    PMIX_VALUE_DESTRUCT(&value);
    PMIX_VALUE_CONSTRUCT(&value);
    CHECK(PMIX_SUCCESS == PMIx_Value_load(&value, &scalar, PMIX_UINT64));
    CHECK(PMIX_SUCCESS == PMIx_Put(PMIX_GLOBAL, scalar_key, &value));
    PMIX_VALUE_DESTRUCT(&value);
    CHECK(PMIX_SUCCESS == PMIx_Commit());
}

static pmix_value_t* get_payload(const pmix_proc_t* proc) {
    const char expected[] = {0, (char)(proc->rank + 1), 0x7f, (char)0xff, 0};
    pmix_value_t* value = NULL;

    CHECK(PMIX_SUCCESS == PMIx_Get(proc, payload_key, NULL, 0, &value));
    CHECK(NULL != value && PMIX_BYTE_OBJECT == value->type);
    CHECK(sizeof(expected) == value->data.bo.size);
    CHECK(NULL != value->data.bo.bytes);
    CHECK(0 == memcmp(expected, value->data.bo.bytes, sizeof(expected)));
    return value;
}

static void check_values(const pmix_proc_t* proc) {
    pmix_value_t* first = get_payload(proc);
    first->data.bo.bytes[1] = 0;

    /* Returned storage belongs to the caller, independently of the cache
     * and subsequent queries. The first peer query also fetches server data. */
    pmix_value_t* second = get_payload(proc);
    CHECK(first != second && first->data.bo.bytes != second->data.bo.bytes);
    CHECK(0 == first->data.bo.bytes[1]);
    PMIX_VALUE_RELEASE(first);
    PMIX_VALUE_RELEASE(second);

    pmix_value_t* scalar = NULL;
    CHECK(PMIX_SUCCESS == PMIx_Get(proc, scalar_key, NULL, 0, &scalar));
    CHECK(NULL != scalar && PMIX_UINT64 == scalar->type);
    CHECK(UINT64_C(123) + proc->rank == scalar->data.uint64);
    PMIX_VALUE_RELEASE(scalar);
}

struct async_value {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    bool ready;
    pmix_status_t status;
    pmix_value_t* value;
};

static void value_ready(pmix_status_t status, pmix_value_t* value, void* data) {
    struct async_value* result = data;
    CHECK(0 == pthread_mutex_lock(&result->mutex));
    result->status = status;
    result->value = value;
    result->ready = true;
    CHECK(0 == pthread_cond_signal(&result->condition));
    CHECK(0 == pthread_mutex_unlock(&result->mutex));
}

static pmix_value_t*
get_hostname(const pmix_proc_t* proc, const pmix_info_t* info, size_t ninfo, bool nonblocking) {
    pmix_value_t* value = NULL;
    if (nonblocking) {
        struct async_value result = {.mutex = PTHREAD_MUTEX_INITIALIZER,
                                     .condition = PTHREAD_COND_INITIALIZER};
        CHECK(PMIX_SUCCESS == PMIx_Get_nb(proc, PMIX_HOSTNAME, info, ninfo, value_ready, &result));
        CHECK(0 == pthread_mutex_lock(&result.mutex));
        while (!result.ready) {
            CHECK(0 == pthread_cond_wait(&result.condition, &result.mutex));
        }
        CHECK(0 == pthread_mutex_unlock(&result.mutex));
        CHECK(PMIX_SUCCESS == result.status);
        value = result.value;
        CHECK(0 == pthread_cond_destroy(&result.condition));
        CHECK(0 == pthread_mutex_destroy(&result.mutex));
    } else {
        CHECK(PMIX_SUCCESS == PMIx_Get(proc, PMIX_HOSTNAME, info, ninfo, &value));
    }
    CHECK(NULL != value && PMIX_STRING == value->type);
    CHECK(NULL != value->data.string && '\0' != value->data.string[0]);
    return value;
}

static void check_hostnames(const pmix_proc_t* proc) {
    pmix_value_t* first = get_hostname(proc, NULL, 0, false);
    pmix_value_t* second = get_hostname(proc, NULL, 0, true);
    CHECK(0 == strcmp(first->data.string, second->data.string));
    PMIX_VALUE_RELEASE(second);

    /* An explicit hostname belongs to the caller's directive. Query teardown
     * must release derived names without freeing this borrowed allocation. */
    pmix_info_t directive;
    PMIX_INFO_LOAD(&directive, PMIX_HOSTNAME, first->data.string, PMIX_STRING);
    for (unsigned nonblocking = 0; nonblocking < 2; ++nonblocking) {
        pmix_value_t* value = get_hostname(proc, &directive, 1, nonblocking);
        CHECK(0 == strcmp(first->data.string, value->data.string));
        CHECK(0 == strcmp(first->data.string, directive.value.data.string));
        CHECK(value->data.string != directive.value.data.string);
        PMIX_VALUE_RELEASE(value);
    }
    PMIX_INFO_DESTRUCT(&directive);
    PMIX_VALUE_RELEASE(first);
}

int main(void) {
    pmix_proc_t self, peer, job;
    pmix_value_t* value = NULL;
    pmix_info_t optional;

    CHECK(PMIX_SUCCESS == PMIx_Init(&self, NULL, 0));
    PMIX_LOAD_PROCID(&job, self.nspace, PMIX_RANK_WILDCARD);
    CHECK(PMIX_SUCCESS == PMIx_Get(&job, PMIX_JOB_SIZE, NULL, 0, &value));
    CHECK(NULL != value && PMIX_UINT32 == value->type && 2 == value->data.uint32);
    CHECK(self.rank < 2);
    PMIX_VALUE_RELEASE(value);

    publish(&self);
    /* The default fence synchronizes without collecting peers' values. */
    CHECK(PMIX_SUCCESS == PMIx_Fence(&job, 1, NULL, 0));
    PMIX_LOAD_PROCID(&peer, self.nspace, 1 - self.rank);

    PMIX_INFO_LOAD(&optional, PMIX_OPTIONAL, NULL, PMIX_BOOL);
    value = NULL;
    CHECK(PMIX_ERR_NOT_FOUND == PMIx_Get(&peer, payload_key, &optional, 1, &value));
    CHECK(NULL == value);
    CHECK(PMIX_ERR_NOT_FOUND == PMIx_Get(&self, "loom.get.missing", &optional, 1, &value));
    CHECK(NULL == value);
    PMIX_INFO_DESTRUCT(&optional);

    check_values(&self);
    check_values(&peer);
    check_hostnames(&self);
    check_hostnames(&peer);
    CHECK(PMIX_SUCCESS == PMIx_Finalize(NULL, 0));
    puts("PMIx Get ownership checks passed");
    return EXIT_SUCCESS;
}
