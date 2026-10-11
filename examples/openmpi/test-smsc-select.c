/* SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only */
// clang-format off
#include "opal_config.h"
// clang-format on

#include <stdio.h>
#include <stdlib.h>

#include "opal/mca/base/base.h"
#include "opal/mca/smsc/base/base.h"

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "%s:%d: %s failed\n", __FILE__, __LINE__, #condition);                 \
            exit(EXIT_FAILURE);                                                                    \
        }                                                                                          \
    } while (0)

static mca_smsc_module_t modules[3];
static unsigned queries, enables, order[2];

static int query(void) {
    ++queries;
    return OPAL_SUCCESS;
}

static mca_smsc_module_t* enable(unsigned index) {
    CHECK(enables < 2);
    if (0 == enables) {
        CHECK(NULL == mca_smsc);
    } else {
        CHECK(&modules[order[0]] == mca_smsc);
    }
    order[enables++] = index;
    return &modules[index];
}

static mca_smsc_module_t* enable_low(void) {
    return enable(0);
}
static mca_smsc_module_t* enable_middle(void) {
    return enable(1);
}
static mca_smsc_module_t* enable_high(void) {
    return enable(2);
}

static mca_smsc_component_t components[] = {
    {.smsc_version = {.mca_component_name = "regression-low"},
     .priority = 10,
     .query = query,
     .enable = enable_low},
    {.smsc_version = {.mca_component_name = "regression-middle"},
     .priority = 20,
     .query = query,
     .enable = enable_middle},
    {.smsc_version = {.mca_component_name = "regression-high"},
     .priority = 30,
     .query = query,
     .enable = enable_high}};

int main(void) {
    opal_smsc_base_framework.framework_output = -1;
    for (unsigned count = 0; count <= 3; ++count) {
        queries = enables = 0;
        mca_smsc = NULL;
        OBJ_CONSTRUCT(&opal_smsc_base_framework.framework_components, opal_list_t);
        for (unsigned i = 0; i < count; ++i) {
            mca_base_component_list_item_t* item = OBJ_NEW(mca_base_component_list_item_t);
            CHECK(NULL != item);
            item->cli_component = &components[i].smsc_version;
            opal_list_append(&opal_smsc_base_framework.framework_components, &item->super);
        }

        CHECK(OPAL_SUCCESS == mca_smsc_base_select());
        CHECK(count == queries);
        CHECK((count < 2 ? count : 2) == enables);
        if (0 == count) {
            CHECK(NULL == mca_smsc);
        } else {
            CHECK(&modules[count - 1] == mca_smsc);
            CHECK(count - 1 == order[enables - 1]);
            if (count > 1) {
                CHECK(count - 2 == order[0]);
            }
        }
        CHECK(count == opal_list_get_size(&opal_smsc_base_framework.framework_components));
        OPAL_LIST_DESTRUCT(&opal_smsc_base_framework.framework_components);
    }
    CHECK(OPAL_SUCCESS == opal_class_finalize());
    puts("SMSC selection ownership checks passed");
    return EXIT_SUCCESS;
}
