#ifndef _HEADER_CONST_H
#define _HEADER_CONST_H

#include "defs.h"

// system includes
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#if defined(HAVE_LINUX_NETLINK_H)
#  include <linux/netlink.h>
#endif
#if defined(HAVE_LINUX_IF_PACKET_H)
#  include <linux/if_packet.h>
#endif

union sockaddr_t {
  char pad[128];
  struct sockaddr sa;
  struct sockaddr_in sin;
  struct sockaddr_un sau;
#ifdef HAVE_INET_NTOP
  struct sockaddr_in6 sa6;
#endif
#if defined(LINUX) && defined(AF_IPX)
  //~ struct sockaddr_ipx sipx;
#endif
#ifdef AF_PACKET
  //~ struct sockaddr_ll ll;
#endif
#ifdef AF_NETLINK
  //~ struct sockaddr_nl nl;
#endif
};

enum provenance_type {
  PRV_RDONLY=1, PRV_WRONLY=2, PRV_RDWR = 3, PRV_UNKNOWNIO=4, 
  PRV_EXECVE=17, PRV_SPAWN=18, PRV_LEXIT=19, 
  STAT_MEM=33,
  PRV_ACTION=65,
  PRV_INVALID=127};

enum sock_action{SOCK_SEND, SOCK_SENDTO, SOCK_SENDMSG,  // 0, 1, 2
  SOCK_RECV, SOCK_RECVFROM, SOCK_RECVMSG,               // 3, 4, 5
  SOCK_BIND, SOCK_CONNECT, SOCK_LISTEN,                 // 6, 7, 8
  SOCK_ACCEPT                                           // 9
  };

#ifndef KEYLEN
#define KEYLEN (1024)
#endif

#ifndef TRUE
#define TRUE (1)
#endif

#ifndef FALSE
#define FALSE (0)
#endif

#ifndef EXITIF
#define EXITIF(x) do { \
  if (x) { \
    fprintf(stderr, "Fatal error in %s [%s:%d]\n", __FUNCTION__, __FILE__, __LINE__); \
    exit(1); \
  } \
} while(0)
#endif

#ifndef vb
#define vb(lvl) do {\
  if (Cde_verbose_mode>=lvl) { \
    fprintf(stderr, "[%s:%d-v%d] %s\n", __FILE__, __LINE__, lvl, __FUNCTION__); \
  } \
} while (0)
#endif
#ifndef vbp
#define vbp(lvl, ...) do {\
  if (Cde_verbose_mode>=lvl) { \
    fprintf(stderr, "[%s:%d-v%d] %s: ", __FILE__, __LINE__, lvl, __FUNCTION__); \
    fprintf(stderr, ##__VA_ARGS__); \
  } \
} while (0)
#endif

#ifndef freeifnn
#define freeifnn(pointer) do {\
  if (pointer != NULL) free(pointer); \
} while (0)
#endif

#endif // _HEADER_CONST_H
