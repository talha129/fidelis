/*
    C socket server example
*/

#include<stdio.h>
#include<string.h>    //strlen
#include<sys/socket.h>
#include<arpa/inet.h> //inet_addr
#include<unistd.h>    //write

int main(int argc , char *argv[])
{
    int socket_desc , client_sock , c , read_size;
    struct sockaddr_in server , client;
    char client_message[2000];
    char reply_message[2000];
    bzero(client_message, 2000);

    //Create socket
    socket_desc = socket(AF_INET , SOCK_STREAM , 0);
    int true=1;
    setsockopt(socket_desc, SOL_SOCKET, SO_REUSEADDR, &true, sizeof(int));
    if (socket_desc == -1)
    {
        printf("Could not create socket");
    }
    printf("Socket created %d\n", socket_desc);

    //Prepare the sockaddr_in structure
    server.sin_family = AF_INET;
    server.sin_addr.s_addr = INADDR_ANY;
    server.sin_port = htons( 8888 );

    //Bind
    if( bind(socket_desc,(struct sockaddr *)&server , sizeof(server)) < 0)
    {
        //print the error message
        perror("bind failed. Error");
        return 1;
    }
    printf("bind done\n");

    //Listen
    listen(socket_desc , 3);

    //Accept and incoming connection
    printf("Waiting for incoming connections...\n");
    c = sizeof(struct sockaddr_in);

    char exitnow = 0;
    while (!exitnow) {
        //accept connection from an incoming client
        client_sock = accept(socket_desc, (struct sockaddr *)&client, (socklen_t*)&c);
        if (client_sock < 0)
        {
            perror("accept failed");
            return 1;
        }
        printf("Connection accepted %d\n", client_sock);


        //Receive a message from client
        while( (read_size = recv(client_sock , client_message , 2000 , 0)) > 0 )
        {
            //Send the message back to client
            sprintf(reply_message, "I got: %s", client_message);
            //write(client_sock , reply_message , strlen(reply_message));
            send(client_sock , reply_message , strlen(reply_message), 0);
            exitnow = strcmp(client_message, "_exit") == 0 ? 1 : 0;
            printf("%d %s\n", read_size, client_message);
            
            bzero(client_message, 2000);
            bzero(reply_message, 2000);
        }

        if(read_size == 0)
        {
            printf("Client disconnected\n");
            fflush(stdout);
        }
        else if(read_size == -1)
        {
            perror("recv failed");
        }
        printf("close %d\n", close(client_sock));
    }

    return 0;
}
