/* SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only */
// clang-format off
#include "opal_config.h"
// clang-format on

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "opal/mca/base/mca_base_var.h"
#include "opal/runtime/opal.h"

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "%s:%d: %s failed\n", __FILE__, __LINE__, #condition);                 \
            exit(EXIT_FAILURE);                                                                    \
        }                                                                                          \
    } while (0)

static int register_value(const char* name, mca_base_var_type_t type, char** storage) {
    return mca_base_var_register("opal",
                                 "loom_registration",
                                 NULL,
                                 name,
                                 NULL,
                                 type,
                                 NULL,
                                 0,
                                 MCA_BASE_VAR_FLAG_SETTABLE,
                                 OPAL_INFO_LVL_1,
                                 MCA_BASE_VAR_SCOPE_LOCAL,
                                 storage);
}

static void check_registration(const char* name, mca_base_var_type_t type) {
    char initial[] = "initial value";
    char* value = initial;
    int index = register_value(name, type, &value);
    CHECK(index >= 0);
    CHECK(value != initial && 0 == strcmp(value, initial));
    initial[0] = 'X';
    for (unsigned i = 0; i < 3; ++i) {
        CHECK(index == register_value(name, type, &value));
        CHECK(0 == strcmp(value, "initial value"));
    }

    CHECK(OPAL_SUCCESS ==
          mca_base_var_set_value(index, "changed value", 0, MCA_BASE_VAR_SOURCE_SET, NULL));
    CHECK(index == register_value(name, type, &value));
    CHECK(0 == strcmp(value, "changed value"));

    /* Framework selection variables survive group deregistration. */
    const mca_base_var_t* variable = NULL;
    CHECK(OPAL_SUCCESS == mca_base_var_get(index, &variable));
    CHECK(OPAL_SUCCESS == mca_base_var_group_deregister(variable->mbv_group_index));
    CHECK(index == register_value(name, type, &value));
    CHECK(0 == strcmp(value, "changed value"));

    CHECK(OPAL_SUCCESS == mca_base_var_deregister(index));
    CHECK(NULL == value);
    char replacement[] = "replacement value";
    value = replacement;
    CHECK(index == register_value(name, type, &value));
    CHECK(value != replacement && 0 == strcmp(value, replacement));
    replacement[0] = 'X';
    CHECK(0 == strcmp(value, "replacement value"));

    /* A new storage location gets its own copy before the old one retires. */
    char* rebound = value;
    CHECK(index == register_value(name, type, &rebound));
    CHECK(NULL == value);
    CHECK(0 == strcmp(rebound, "replacement value"));
    CHECK(OPAL_SUCCESS == mca_base_var_deregister(index));
    CHECK(NULL == rebound);

    /* Empty defaults remain valid across registration and rebinding. */
    CHECK(index == register_value(name, type, &value));
    CHECK(NULL == value);
    CHECK(index == register_value(name, type, &value));
    CHECK(index == register_value(name, type, &rebound));
    CHECK(NULL == value && NULL == rebound);
    CHECK(OPAL_SUCCESS == mca_base_var_deregister(index));
}

static void check_environment(void) {
    const char* name = "environment";
    char* environment = NULL;
    CHECK(OPAL_SUCCESS == mca_base_var_env_name("loom_registration_environment", &environment));
    CHECK(0 == setenv(environment, "first override", 1));
    char* value = NULL;
    int index = register_value(name, MCA_BASE_VAR_TYPE_STRING, &value);
    CHECK(index >= 0 && 0 == strcmp(value, "first override"));
    CHECK(index == register_value(name, MCA_BASE_VAR_TYPE_STRING, &value));
    CHECK(0 == strcmp(value, "first override"));
    CHECK(0 == setenv(environment, "second override", 1));
    CHECK(index == register_value(name, MCA_BASE_VAR_TYPE_STRING, &value));
    CHECK(0 == strcmp(value, "second override"));
    mca_base_var_source_t source;
    CHECK(OPAL_SUCCESS == mca_base_var_get_value(index, NULL, &source, NULL));
    CHECK(MCA_BASE_VAR_SOURCE_ENV == source);
    CHECK(OPAL_SUCCESS == mca_base_var_deregister(index));
    CHECK(NULL == value);
    CHECK(0 == unsetenv(environment));
    free(environment);
}

int main(int argc, char** argv) {
    CHECK(OPAL_SUCCESS == opal_init_util(&argc, &argv));
    check_environment();
    check_registration("string", MCA_BASE_VAR_TYPE_STRING);
    check_registration("version", MCA_BASE_VAR_TYPE_VERSION_STRING);
    CHECK(OPAL_SUCCESS == opal_finalize_util());
    puts("MCA registration ownership checks passed");
    return EXIT_SUCCESS;
}
