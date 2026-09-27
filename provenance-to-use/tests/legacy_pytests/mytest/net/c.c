/*
    C ECHO client example using sockets
*/
#include<stdio.h> //printf
#include<string.h>    //strlen
#include<sys/socket.h>    //socket
#include<arpa/inet.h> //inet_addr

int main(int argc , char *argv[])
{
    int sock;
    struct sockaddr_in server;
    char message[1000] , server_reply[2000];
    bzero(server_reply, 2000);

    int testn = 2;
    while (testn-- > 0) {
        //Create socket
        sock = socket(AF_INET , SOCK_STREAM , 0);
        if (sock == -1)
        {
            printf("Could not create socket");
        }
        printf("Socket created %d\n", sock);

        server.sin_addr.s_addr = inet_addr("127.0.0.1");
        server.sin_family = AF_INET;
        server.sin_port = htons( 8888 );

        //Connect to remote server
        if (connect(sock , (struct sockaddr *)&server , sizeof(server)) < 0)
        {
            perror("connect failed. Error");
            return 1;
        }

        puts("Connected");

        //keep communicating with server
        int i=2;
        while (i-- > 0) {
            int res;
            printf("Enter message : ");
            scanf("%s" , message);

            //Send some data
            if( send(sock , message , strlen(message) , 0) < 0)
            {
                puts("Send failed");
                return 1;
            }

            //Receive a reply from the server
            res = recv(sock , server_reply , 2000 , 0);
            if( res < 0)
            {
                puts("recv failed");
                break;
            }

            printf("Server reply [%d]:", res);
            server_reply[res] = '\0';
            puts(server_reply);
            bzero(server_reply, 2000);
        }
        //~ sleep(1000);

        close(sock);
    }
    return 0;
}
