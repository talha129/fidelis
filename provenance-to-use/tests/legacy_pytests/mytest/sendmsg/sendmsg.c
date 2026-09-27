#include <string.h>
#include <assert.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>

#define SIZE 512
#define AODV_UDP_PORT 1654

int main(int argc,char *argv[])
{
    int fd=0;
    int fd2=0;
    char conbuffer[SIZE];
    char iobuffer[SIZE]="WASAAAAAAAA";
    struct iovec io;
    struct cmsghdr *c;
    int ttl=8;

    struct sockaddr_in address;
    int broadcast = 1;
    int numbytes;
    struct msghdr msgh;

    //if( (fd = socket(AF_INET,SOCK_STREAM,0)) == -1 )
    if( (fd = socket(AF_INET,SOCK_DGRAM,0)) == -1 )
    {
        perror("error socket");
        return -1;
    }

    //IPv4
    address.sin_family = AF_INET;
    // Set port
    address.sin_port = 0;
    // Listen from any ip
    address.sin_addr.s_addr = INADDR_ANY;

    if( bind(fd, (struct sockaddr *)&address,
        sizeof(address)) == -1 )
    {
        perror("error bind");
        close(fd);
        return -1;
    }

    /*
    if(setsockopt(fd,SOL_SOCKET,SO_BROADCAST,&broadcast,
                sizeof broadcast) == -1)
    {
        perror("error set socket");
        close(fd);
        return -1;
    }
    */

    address.sin_addr.s_addr = inet_addr("127.0.0.1");
    //printf("%s\n",inet_ntoa(address.sin_addr.s_addr));
    address.sin_port = htons(AODV_UDP_PORT);

    memset(&msgh,0,sizeof(msgh));
    memset(&conbuffer,0,SIZE);

    msgh.msg_name=&address;
    msgh.msg_namelen=sizeof(address);

    io.iov_base=iobuffer;
    io.iov_len=SIZE;
    msgh.msg_iovlen=1;
    msgh.msg_iov=&io;

    //~ msgh.msg_control=conbuffer;
    //~ msgh.msg_controllen=sizeof(conbuffer);
//~ 
    //~ c=CMSG_FIRSTHDR(&msgh);
    //~ assert(c);
    //~ /*
    //~ c->cmsg_level=SOL_SOCKET;
    //~ c->cmsg_type=SCM_RIGHTS;
    //~ */
    //~ c->cmsg_level=IPPROTO_IP;
    //~ c->cmsg_type=IP_TTL;
//~ 
//~ 
    //~ c->cmsg_len=CMSG_LEN(sizeof(int));
    //~ *(int*)CMSG_DATA(c)=ttl;
//~ 
    //~ msgh.msg_controllen=c->cmsg_len;

    //if((numbytes = recvmsg(fd,&msgh,0)) == -1)
    int i=2;
    while (i-->0)
    if((numbytes = sendmsg(fd,&msgh,0)) == -1)
    {
        perror("FATAL ERROR: sendmsg");
        return (-1);
    }
    else
        printf("sent %d\n", numbytes);

    close(fd);

    return 0;
}
