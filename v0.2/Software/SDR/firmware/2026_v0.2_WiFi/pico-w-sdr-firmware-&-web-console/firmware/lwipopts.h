// Save this as: firmware/lwipopts.h
#ifndef _LWIPOPTS_H
#define _LWIPOPTS_H

#define NO_SYS                          1
#define LWIP_SOCKET                     0
#define LWIP_NETCONN                    0
#define MEM_ALIGNMENT                   4

#define MEM_SIZE                        16384
#define MEMP_NUM_PBUF                   32
#define MEMP_NUM_UDP_PCB                6
#define MEMP_NUM_TCP_PCB                6
#define MEMP_NUM_TCP_PCB_LISTEN         2
#define MEMP_NUM_TCP_SEG                32
#define MEMP_NUM_SYS_TIMEOUT            10

#define PBUF_POOL_SIZE                  24
#define PBUF_POOL_BUFSIZE               1536

#define LWIP_ARP                        1
#define LWIP_ETHERNET                   1
#define LWIP_ICMP                       1
#define LWIP_RAW                        0
#define LWIP_DHCP                       1
#define LWIP_AUTOIP                     1
#define LWIP_DNS                        0
#define LWIP_UDP                        1
#define LWIP_TCP                        1

#define TCP_MSS                         1460
#define TCP_WND                         (8 * TCP_MSS)
#define TCP_SND_BUF                     (8 * TCP_MSS)
#define TCP_SND_QUEUELEN                ((4 * (TCP_SND_BUF) + (TCP_MSS - 1)) / (TCP_MSS))

#define CHECKSUM_GEN_IP                 1
#define CHECKSUM_GEN_UDP                1
#define CHECKSUM_GEN_TCP                1
#define CHECKSUM_CHECK_IP               1
#define CHECKSUM_CHECK_UDP              1
#define CHECKSUM_CHECK_TCP              1

#define LWIP_STATS                      0
#define LWIP_DEBUG                      0

#endif /* _LWIPOPTS_H */
