/*
	SOCK_DGRAM example program (multiple clients, recvmsg/sendmsg)
	AUP2, Sec. 8.06.3

	Copyright 2003 by Marc J. Rochkind. All rights reserved.
	May be copied only for purposes and under conditions described
	on the Web page www.basepath.com/aup/copyright.htm.

	The Example Files are provided "as is," without any warranty;
	without even the implied warranty of merchantability or fitness
	for a particular purpose. The author and his publisher are not
	responsible for any damages, direct or incidental, resulting
	from the use or non-use of these Example Files.

	The Example Files may contain defects, and some contain deliberate
	coding mistakes that were included for educational reasons.
	You are responsible for determining if and how the Example Files
	are to be used.

*/
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/uio.h>
#include <sys/socket.h>
#include <sys/un.h>

#define SOCKETNAME_SERVER "SktOne"
#define SOCKETNAME_CLIENT "SktTwo"

static struct sockaddr_un sa_server;

#define MSG_SIZE 30
#define NC 2 // number of client

static void run_client(int nclient)
{
	struct sockaddr_un sa_client;
	int fd_skt;
	ssize_t nrecv;
	char msg[2][MSG_SIZE];
	int i;

	//~ if (fork() == 0) { /* client */
		//~ sleep(1); /* let server startup first */
		//~ ec_neg1( fd_skt = socket(AF_UNIX, SOCK_DGRAM, 0) )
		fd_skt = socket(AF_UNIX, SOCK_DGRAM, 0);
		snprintf(sa_client.sun_path, sizeof(sa_client.sun_path),
		  "%s-%d", SOCKETNAME_CLIENT, nclient);
		(void)unlink(sa_client.sun_path);
		sa_client.sun_family = AF_UNIX;
		//~ ec_neg1( bind(fd_skt, (struct sockaddr *)&sa_client,
		  //~ sizeof(sa_client)) )
		bind(fd_skt, (struct sockaddr *)&sa_client,
		  sizeof(sa_client));
		for (i = 1; i <= 4; i++) {
			snprintf(msg[0], sizeof(msg[0]), "Message! (#%d of client #%d)", i, nclient);
			snprintf(msg[1], sizeof(msg[1]), "what else? (#%d of client #%d)", i, nclient);
			printf("Client sending %lu %lu %lu\n", sizeof(msg), sizeof(msg[0]), sizeof(msg[1]));
			sendto(fd_skt, msg, sizeof(msg), 0,
			  (struct sockaddr *)&sa_server, sizeof(sa_server));
			nrecv = read(fd_skt, msg, sizeof(msg));
			if (nrecv != sizeof(msg)) {
				//~ printf("Client got short message\n");
				//~ break;
			}
			printf("Client %d got \"%s\" & \"%s\" back [%zu]\n", nclient, msg[0], msg[1], nrecv);
		}
		sleep(1);
		close(fd_skt);
	//~ }
	return;

}
/*[run_server]*/
static void run_server(void)
{
	#define N 3
	int fd_skt, i;
	ssize_t nrecv;
	char msg[N][MSG_SIZE];
	struct sockaddr_storage sa;
	struct msghdr m;
	struct iovec v[N];

	fd_skt = socket(AF_UNIX, SOCK_DGRAM, 0);
	bind(fd_skt, (struct sockaddr *)&sa_server, sizeof(sa_server));
	int counter = 4*NC;
	while (counter-->0) {
		memset(msg, 0, sizeof(msg));
		memset(&m, 0, sizeof(m));
		m.msg_name = &sa;
		m.msg_namelen = sizeof(sa);
		for (i=0; i<N; i++) {
			v[i].iov_base = msg[i];
			v[i].iov_len = sizeof(msg[i]);
		}
		m.msg_iov = &v;
		m.msg_iovlen = 2;
		//~ printf("Server addr %p %p %p %p\n", msg[0], msg[1], m.msg_iov[0].iov_base, m.msg_iov[1].iov_base);
		nrecv = recvmsg(fd_skt, &m, 0);
		if (nrecv < sizeof(msg)) {
			printf("Server got %zd != %lu\n", nrecv, sizeof(msg));
			//~ break;
		} else {
			printf("Server ok: %zd + %zd\n", v[0].iov_len, v[1].iov_len);
		}
		//~ printf("Server new addr %p %p %p %p\n", msg[0], msg[1], m.msg_iov[0].iov_base, m.msg_iov[1].iov_base);
		printf("Server got: \"%s\" & \"%s\" [%zu]\n", msg[0], msg[1], nrecv);
		((char *)m.msg_iov[0].iov_base)[0] = 'x';
		((char *)m.msg_iov[1].iov_base)[0] = 'y';
		//~ printf("Server modified: \"%s\" & \"%s\" [%zu]\n", msg[0], msg[1], nrecv);
		sendmsg(fd_skt, &m, 0);
	}
	close(fd_skt);
	sleep(1);
	exit(0);

}
/*[]*/
int main(int argc, char* argv[])
{
	int nclient;
	if (argc == 1) return 0;

	strcpy(sa_server.sun_path, SOCKETNAME_SERVER);
	sa_server.sun_family = AF_UNIX;
	//~ (void)unlink(SOCKETNAME_SERVER);
	if (argv[1][0]=='c') {// run client
		for (nclient = 1; nclient <= NC; nclient++) {
			printf("client %d\n", nclient);
			run_client(nclient);
		}
		exit(0);
	}
	else if (argv[1][0]=='s'){ // run server
		(void)unlink(SOCKETNAME_SERVER);
		run_server();
	}
	else if (argv[1][0]=='a'){ // run both
		(void)unlink(SOCKETNAME_SERVER);
		for (nclient = 1; nclient <= NC; nclient++)
			if (fork() == 0) {
				sleep(1);
				run_client(nclient);
				exit(0);
			}
		run_server();
	}
	exit(0);
}
