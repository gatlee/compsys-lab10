// a webserver which serves files
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

static char const* HTTP_200_FORMAT = "HTTP/1.1 200 OK\r\n\
Content-Length: %ld\r\nContent-Type: %s\r\n\r\n";
static char const* HTTP_503_FORMAT =
	"HTTP/1.1 503 Service Unavailable\r\n\r\n503\n";
static char const* HTTP_404_FORMAT =
	"HTTP/1.1 404 Not Found\r\n\r\nFile not found\n";
static char const* HTTP_400_FORMAT = "HTTP/1.1 404 Bad Request\r\n\r\n";

// prototypes
long long get_file_length(const char* filename);
int create_socket(const char* address, const int port);
void* connection_routine(void* args);

// struct for connection thread arguments
struct ConnectionArgs {
	int sockfd;
	int efd;
	time_t* times;
	fd_set* masterfds;
	int* maxfd;
} ConnectionArgs;

// lock for fd_set
pthread_mutex_t fdset_lock;

// handles client requests
void handle_request(int sockfd) {
	char buff[4096];
	int n = 0;

	// read request
	while (1) {
		n += read(sockfd, buff, 4096 - 1);
		if (n <= 0) {
			if (n < 0)
				perror("read");
			return;
		}
		buff[n] = 0;

		if (strstr(buff, "\r\n\r\n")) {
			break;
		}
	}

	// check request line
	if (strncmp(buff, "GET", 3) != 0) {
		return;
	}

	// let's pretend that we need to:
	// check authentication, query database etc.
	sleep(3);

	// extract resource
	char* slash = strstr(buff, "/");
	char* space = strstr(slash, " ");
	char* resource = malloc(sizeof(char) * (space - slash));
	strncpy(resource, slash + 1, space - slash);
	resource[space - slash - 1] = 0;

	// no ../
	if (strstr(resource, "..")) {
		free(resource);
		n = sprintf(buff, HTTP_400_FORMAT);
		printf("[%d] 400 for %s\n", sockfd, resource);
		write(sockfd, buff, n);
		close(sockfd);
		return;
	}
	// assume that index.html is default file
	if (strlen(resource) == 0) {
		free(resource);
		resource = "index.html";
	}

	// non-0 return, 404
	struct stat st;
	if (stat(resource, &st) != 0) {
		n = sprintf(buff, HTTP_404_FORMAT);
		printf("[%d] 404 for %s\n", sockfd, resource);
		write(sockfd, buff, n);
		close(sockfd);
		return;
	}

	// very basic (and often incorrect) content-type matching
	char* content_type;
	if (strstr(resource, ".html")) {
		content_type = "text/html";
	} else if (strstr(resource, ".css")) {
		content_type = "text/css";
	} else if (strstr(resource, ".js")) {
		content_type = "text/javascript";
	} else if (strstr(resource, ".c") || strstr(resource, ".txt")) {
		content_type = "text/plain";
	} else {
		content_type = "application/octet-stream";
	}

	// send response
	printf("[%d] sending response for %s\n", sockfd, resource);
	// write header
	n = sprintf(buff, HTTP_200_FORMAT, st.st_size, content_type);
	write(sockfd, buff, n);
	// write file
	int filefd = open(resource, O_RDONLY);
	do {
		n = sendfile(sockfd, filefd, NULL, 2048);
	} while (n > 0);
	if (n < 0) {
		perror("sendfile");
		close(filefd);
		return;
	}

	close(filefd);
	close(sockfd);
}

int main(int argc, char* argv[]) {
	if (argc < 3) {
		fprintf(stderr, "usage: %s interface port\n", argv[0]);
		return EXIT_FAILURE;
	}

	// init lock
	if (pthread_mutex_init(&fdset_lock, NULL) != 0) {
		fprintf(stderr, "mutex init failed\n");
		return EXIT_FAILURE;
	}

	// create socket
	int sockfd = create_socket(argv[1], atoi(argv[2]));

	// create event file descriptor, to wake up select after accept
	int efd = eventfd(0, 0);
	if (efd == -1) {
		perror("eventfd");
		return 1;
	}

	// init set, add event fd
	fd_set masterfds;
	FD_ZERO(&masterfds);
	// add efd
	FD_SET(efd, &masterfds);
	int maxfd = efd;

	// set up welcome thread, pass to it set of fds, times and socketfd
	pthread_t welcome_thread;
	time_t* times = malloc(sizeof(time_t) * FD_SETSIZE);
	struct ConnectionArgs connection_args;
	connection_args.sockfd = sockfd;
	connection_args.times = times;
	connection_args.masterfds = &masterfds;
	connection_args.maxfd = &maxfd;
	connection_args.efd = efd;
	int r = pthread_create(&welcome_thread, NULL, connection_routine,
						   (void*)&connection_args);
	if (r != 0) {
		fprintf(stderr, "Cannot create thread\n");
	}

	while (1) {
		// monitor
		pthread_mutex_lock(&fdset_lock);
		fd_set readfds = masterfds;
		pthread_mutex_unlock(&fdset_lock);
		if (select(FD_SETSIZE, &readfds, NULL, NULL, NULL) < 0) {
			perror("select");
			exit(EXIT_FAILURE);
		}

		for (int i = 0; i <= maxfd; i++) {
			// if available for reading
			if (FD_ISSET(i, &readfds)) {
				// need to clear this read
				if (i == efd) {
					uint64_t x;
					read(efd, &x, sizeof(uint64_t));
					continue;
				}

				// non-persistent
				pthread_mutex_lock(&fdset_lock);
				FD_CLR(i, &masterfds);
				pthread_mutex_unlock(&fdset_lock);

				// check time since connection
				unsigned long time_used = time(NULL) - times[i];

				// too long, server is 'under load', return 503
				if (time_used > 1) {
					fprintf(stderr, "[%d] sending 503\n", i);
					char buff[2048];
					read(i, buff, 2048);
					write(i, HTTP_503_FORMAT, strlen(HTTP_503_FORMAT));
					close(i);
				} else {
					// handle request
					fprintf(stderr, "[%d] passing request to handler\n", i);
					handle_request(i);
				}
			}
		}
	}

	return 0;
}

// accepts new connections
void* connection_routine(void* args) {
	// extract args
	struct ConnectionArgs* connection_args = (struct ConnectionArgs*)args;
	int sockfd = connection_args->sockfd;
	int efd = connection_args->efd;
	time_t* times = connection_args->times;
	fd_set* masterfds = connection_args->masterfds;
	int* maxfd = connection_args->maxfd;

	while (1) {
		int newsockfd = accept(sockfd, NULL, NULL);

		if (newsockfd < 0) {
			perror("accept");
			close(newsockfd);
		} else {
			pthread_mutex_lock(&fdset_lock);

			// add new socket
			FD_SET(newsockfd, masterfds);
			// update maxfd when applicable
			if (newsockfd > *maxfd)
				*maxfd = newsockfd;
			// set time
			times[newsockfd] = time(NULL);

			fprintf(stderr, "[%d] accepted\n", newsockfd);

			// write to efd
			uint64_t x = 1;
			int n = write(efd, &x, sizeof(uint64_t));
			if (n < 0)
				perror("write");
			pthread_mutex_unlock(&fdset_lock);
		}
	}
}

// create, bind and listen
int create_socket(const char* address, const int port) {
	// create TCP socket which only accept IPv4
	int sockfd = socket(AF_INET, SOCK_STREAM, 0);
	if (sockfd < 0) {
		perror("socket");
		exit(EXIT_FAILURE);
	}

	// reuse the socket if possible
	int const reuse = 1;
	if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(int)) <
		0) {
		perror("setsockopt");
		exit(EXIT_FAILURE);
	}

	// create and initialise address we will listen on
	struct sockaddr_in serv_addr;
	bzero(&serv_addr, sizeof(serv_addr));
	serv_addr.sin_family = AF_INET;
	// if ip parameter is not specified
	serv_addr.sin_addr.s_addr = inet_addr(address);
	serv_addr.sin_port = htons(port);

	// bind address to socket
	if (bind(sockfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
		perror("bind");
		exit(EXIT_FAILURE);
	}

	// listen on the socket
	listen(sockfd, 1024);
	return sockfd;
}
