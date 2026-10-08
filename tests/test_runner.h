#ifndef TEST_RUNNER_H
#define TEST_RUNNER_H

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

#define ANSI_GREEN  "\033[0;32m"
#define ANSI_RED    "\033[0;31m"
#define ANSI_YELLOW "\033[0;33m"
#define ANSI_CYAN   "\033[0;36m"
#define ANSI_BOLD   "\033[1m"
#define ANSI_RESET  "\033[0m"

static int g_tests_run = 0;
static int g_tests_passed = 0;
static int g_tests_failed = 0;
static const char *g_current_suite = "";

#define TEST_SUITE(name) \
    do { \
        g_current_suite = name; \
        printf("\n" ANSI_CYAN ANSI_BOLD "=== %s ===" ANSI_RESET "\n", name); \
    } while(0)

#define ASSERT_TRUE(condition) \
    do { \
        g_tests_run++; \
        if (condition) { \
            g_tests_passed++; \
            printf("  " ANSI_GREEN "[PASS]" ANSI_RESET " %s\n", #condition); \
        } else { \
            g_tests_failed++; \
            printf("  " ANSI_RED "[FAIL]" ANSI_RESET " %s (%s:%d)\n", #condition, __FILE__, __LINE__); \
        } \
    } while(0)

#define ASSERT_FALSE(condition) \
    do { \
        g_tests_run++; \
        if (!(condition)) { \
            g_tests_passed++; \
            printf("  " ANSI_GREEN "[PASS]" ANSI_RESET " !(%s)\n", #condition); \
        } else { \
            g_tests_failed++; \
            printf("  " ANSI_RED "[FAIL]" ANSI_RESET " !(%s) was true! (%s:%d)\n", #condition, __FILE__, __LINE__); \
        } \
    } while(0)

#define ASSERT_EQ(actual, expected) \
    do { \
        g_tests_run++; \
        long long _act = (long long)(actual); \
        long long _exp = (long long)(expected); \
        if (_act == _exp) { \
            g_tests_passed++; \
            printf("  " ANSI_GREEN "[PASS]" ANSI_RESET " %s == %s (%lld)\n", #actual, #expected, _act); \
        } else { \
            g_tests_failed++; \
            printf("  " ANSI_RED "[FAIL]" ANSI_RESET " %s == %s (actual: %lld, expected: %lld) (%s:%d)\n", #actual, #expected, _act, _exp, __FILE__, __LINE__); \
        } \
    } while(0)

#define ASSERT_STR_EQ(actual, expected) \
    do { \
        g_tests_run++; \
        const char *_a = (actual); \
        const char *_e = (expected); \
        if (_a && _e && strcmp(_a, _e) == 0) { \
            g_tests_passed++; \
            printf("  " ANSI_GREEN "[PASS]" ANSI_RESET " %s == \"%s\"\n", #actual, _e); \
        } else { \
            g_tests_failed++; \
            printf("  " ANSI_RED "[FAIL]" ANSI_RESET " %s == \"%s\" (actual: \"%s\") (%s:%d)\n", #actual, _e ? _e : "NULL", _a ? _a : "NULL", __FILE__, __LINE__); \
        } \
    } while(0)

#define PRINT_TEST_SUMMARY() \
    do { \
        printf("\n--------------------------------------------------\n"); \
        if (g_tests_failed == 0) { \
            printf(ANSI_GREEN ANSI_BOLD "ALL %d TESTS PASSED!" ANSI_RESET "\n", g_tests_run); \
        } else { \
            printf(ANSI_RED ANSI_BOLD "TEST RESULTS: %d Passed, %d Failed (Total %d)" ANSI_RESET "\n", \
                   g_tests_passed, g_tests_failed, g_tests_run); \
        } \
        printf("--------------------------------------------------\n\n"); \
    } while(0)

#endif // TEST_RUNNER_H
