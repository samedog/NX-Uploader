/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Diego Cardenas "The Samedog" 
 */
// net_info.c
#include <switch.h>
#include <arpa/inet.h>
#include <string.h>
#include <unistd.h>

#include "net_info.h"

char g_ip[32] = "0.0.0.0";

void detect_local_ip(void){
    long id = gethostid();
    if (id == 0){
        strcpy(g_ip, "0.0.0.0");
        return;
    }
    struct in_addr a;
    // gethostid() on libnx returns the address in host byte order,
    // but our in_addr wants network byte order. The double swap
    // (bswap + htonl) is intentional: on the little-endian Switch,
    // htonl() is a no-op, so the bswap does the real work, the htonl
    // makes this correct on a hypothetical big-endian host too (maybe?).
    a.s_addr = htonl(__builtin_bswap32((uint32_t)id));
    const char *s = inet_ntoa(a);
    strncpy(g_ip, s, sizeof(g_ip) - 1);
    g_ip[sizeof(g_ip) - 1] = 0;
}   