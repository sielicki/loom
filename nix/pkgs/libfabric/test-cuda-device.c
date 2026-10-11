/* Exercise the real HMEM classifier with CUDA's documented attribute widths. */
// clang-format off
#include <config.h>
// clang-format on

#if !HAVE_CUDA || !ENABLE_CUDA_DLOPEN
#    error This fixture requires the CUDA dlopen configuration
#endif

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/hmem_cuda.c"

struct fi_provider core_prov = {.name = "cuda-device-test"};

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "line %d: %s\n", __LINE__, #condition);                                \
            abort();                                                                               \
        }                                                                                          \
    } while (0)

static unsigned int memory_type = CU_MEMORYTYPE_DEVICE;
static unsigned int managed;
static int ordinal;
static CUresult query_result = CUDA_SUCCESS;

static CUresult pointer_attributes(unsigned int count,
                                   CUpointer_attribute* attributes,
                                   void** values,
                                   CUdeviceptr pointer) {
    CHECK(pointer == 0x1000);
    if (query_result != CUDA_SUCCESS)
        return query_result;
    for (unsigned int i = 0; i < count; ++i) {
        switch (attributes[i]) {
            case CU_POINTER_ATTRIBUTE_MEMORY_TYPE:
                memcpy(values[i], &memory_type, sizeof(memory_type));
                break;
            case CU_POINTER_ATTRIBUTE_IS_MANAGED:
                memcpy(values[i], &managed, sizeof(managed));
                break;
            case CU_POINTER_ATTRIBUTE_DEVICE_ORDINAL:
                memcpy(values[i], &ordinal, sizeof(ordinal));
                break;
            default:
                CHECK(0);
        }
    }
    return CUDA_SUCCESS;
}

int main(void) {
    cuda_ops.cuPointerGetAttributes = pointer_attributes;
    const int ordinals[] = {0, 1, 7, INT_MAX};
    for (size_t i = 0; i < sizeof(ordinals) / sizeof(ordinals[0]); ++i) {
        ordinal = ordinals[i];
        for (managed = 0; managed <= 1; ++managed) {
            uint64_t device = UINT64_MAX;
            uint64_t flags = 0;
            CHECK(cuda_is_addr_valid((void*)0x1000, &device, &flags));
            CHECK(device == (uint64_t)ordinal);
            CHECK(flags == (managed ? 0 : FI_HMEM_DEVICE_ONLY));
            CHECK(cuda_is_addr_valid((void*)0x1000, NULL, NULL));
        }
    }
    memory_type = CU_MEMORYTYPE_HOST;
    CHECK(!cuda_is_addr_valid((void*)0x1000, NULL, NULL));
    memory_type = CU_MEMORYTYPE_DEVICE;
    const CUresult errors[] = {CUDA_ERROR_INVALID_VALUE, CUDA_ERROR_NOT_INITIALIZED};
    for (size_t i = 0; i < sizeof(errors) / sizeof(errors[0]); ++i) {
        query_result = errors[i];
        uint64_t device = UINT64_MAX;
        uint64_t flags = 0;
        CHECK(!cuda_is_addr_valid((void*)0x1000, &device, &flags));
        CHECK(device == UINT64_MAX && flags == 0);
    }
    puts("CUDA HMEM classification: device ordinals and allocation flags pass");
    return 0;
}
