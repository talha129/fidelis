#include <string.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>

#define BUF_SIZE 2024
#define SIZE 1024
#define AODV_UDP_PORT 1654

int main(int argc,char *argv[])
{
    int fd=0;

    char buffer[SIZE]="morte";
    char crap[SIZE]="morte";
    struct iovec io;
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
    address.sin_port = htons(AODV_UDP_PORT);
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
    if( listen(fd,1) < 0)
    {
        close(fd);
        perror("error listening");
        return -1;
    }

    if((fd2= accept(fd,NULL,0)) < 0 )
    {
        close(fd);
        perror("error accept");
        return -1;
    }

    */
    if(setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&broadcast,
                sizeof broadcast) == -1)
    {
        perror("error set socket");
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
    int i = 2;
    while(i-->0)
    {
        io.iov_base=buffer;
        io.iov_len=SIZE;
        memset(&msgh,0,sizeof(msgh));
    //    msgh.msg_control=(struct iovec*)malloc(10000*sizeof(struct iovec));
    //   msgh.msg_controllen= 10000;
        msgh.msg_iov=&io;
        msgh.msg_iovlen=1;
        msgh.msg_control=&crap;
        msgh.msg_controllen=sizeof(crap);
        //Receive the packet
        if((numbytes = recvmsg(fd,&msgh,0)) == -1)
        {
            perror("FATAL ERROR: recvmsg");
            return (-1);
        }

        printf("recibido: %d\n",numbytes);
        printf("name:%d, iov:%d, control:%d flags:%d\n",
                msgh.msg_namelen,msgh.msg_iovlen,
                msgh.msg_controllen,msgh.msg_flags);
        printf("Buffer: %s\n",buffer);
        printf("Daemon: Packet has been received with ttl %d\n",
                aodv_get_ttl(&msgh));
        switch(msgh.msg_flags)
        {
            case MSG_EOR:
                puts("MSG_EOR");
                break;
            case MSG_TRUNC:
                puts("MSG_TRUNC");
                break;
            case MSG_CTRUNC:
                puts("MSG_CTRUNC");
                break;
            case MSG_OOB:
                puts("MSG_OOB");
                break;
            case MSG_ERRQUEUE:
                puts("MSG_ERRQUEUE");
                break;
            case MSG_DONTWAIT:
                puts("MSG_DONTWAIT");
                break;
        }
    }
    close(fd);

    return 0;
}

int aodv_get_ttl(struct msghdr* msgh)
{
    struct cmsghdr *cmsg;

    /* Recibir los datos auxiliares en msgh */
    for (cmsg = CMSG_FIRSTHDR(msgh); cmsg != NULL;
            cmsg = CMSG_NXTHDR(msgh,cmsg))
    {
        puts("megabucl3");
        if (cmsg->cmsg_level == SOL_IP
                && cmsg->cmsg_type == IP_TTL)
            return (int)CMSG_DATA(cmsg);
    }
    /* 
     * FIXME TTL no encontrado
     */
    return -1;
}

