/*
 * ============================================================================
 * NETWORK TEST MODULE HEADER
 * ============================================================================
 * Exposes agnostic internet test functions.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void test_network_http_get(void);
void test_network_google_search(void);
void test_network_task(void *pvParameters);

#ifdef __cplusplus
}
#endif
