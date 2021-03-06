#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <unistd.h>

#include "libhttp.h"

/*
 * Global configuration variables.
 * You need to use these in your implementation of handle_files_request and
 * handle_proxy_request. Their values are set up in main() using the
 * command line arguments (already implemented for you).
 */
int server_port;
char *server_files_directory;
char *server_proxy_hostname;
int server_proxy_port;

void send_file_content(FILE *file, char *type, int fd);

/*
 * Reads an HTTP request from stream (fd), and writes an HTTP response
 * containing:
 *
 *   1) If user requested an existing file, respond with the file
 *   2) If user requested a directory and index.html exists in the directory,
 *      send the index.html file.
 *   3) If user requested a directory and index.html doesn't exist, send a list
 *      of files in the directory with links to each.
 *   4) Send a 404 Not Found response.
 */
void handle_files_request(int fd)
{

  /* YOUR CODE HERE */

  struct http_request *request = http_request_parse(fd);
  struct stat s;
  char *path = strcat(server_files_directory, request->path);
  char *path_backup = malloc(1 + strlen(path));
  strcpy(path_backup, path);
  if (stat(path, &s) == 0) {
    if(s.st_mode & S_IFREG) {
      FILE *file = fopen(path, "r");
      if (file != NULL) {
        char *type = http_get_mime_type(path);
        send_file_content(file, type, fd);
      }
    } else if (s.st_mode & S_IFDIR) {
      char *file_path =  strcat(path, "/index.html");
      FILE *file = fopen(file_path, "r");
      if (file != NULL) {
        char *type = http_get_mime_type(file_path);
        send_file_content(file, type, fd);
      } else {
        http_start_response(fd, 200);
        http_send_header(fd, "Content-type", "text/html");
        http_end_headers(fd);
        DIR *dirp;  
        struct dirent *direntp; 
        dirp = opendir(path_backup);
        while ((direntp = readdir(dirp)) != NULL) {  
          char *file_name = direntp->d_name;
          if (strcmp(file_name, ".") == 0) {
            continue;
          }
          if (strcmp(direntp->d_name, "..") == 0) {
            file_name = "Parent directory";
          }
          char tmp[100];
          memset(tmp, '\0', sizeof(tmp));
          char *head = "<p><a href=\"";
          strcpy(tmp, head);
          strcat(tmp, direntp->d_name);
          if (strcmp(direntp->d_name, "..") == 0) {
            strcat(tmp, "/\">");
          } else {
            strcat(tmp, "\">");
          }
          strcat (tmp, file_name);
          char *tail = "</a></p>";
          strcat (tmp, tail);
          http_send_string(fd, tmp);
        }
        closedir(dirp);
      } 
    } 
  } else {
    http_start_response(fd, 404);
    http_send_header(fd, "Content-type", "text/html");
    http_end_headers(fd);     
    http_send_string(fd, "<html><body>No Page Found</body></html>");
    close(fd);
  }
}

void send_file_content(FILE *file, char *type, int fd) {
  http_start_response(fd, 200);
  http_send_header(fd, "Content-type", type);
  http_end_headers(fd);
  char c[100];
  while (fgets(c, 100, file) != NULL) {
    http_send_string(fd, c);
  }
  fclose (file);
}

/*
 * Opens a connection to the proxy target (hostname=server_proxy_hostname and
 * port=server_proxy_port) and relays traffic to/from the stream fd and the
 * proxy target. HTTP requests from the client (fd) should be sent to the
 * proxy target, and HTTP responses from the proxy target should be sent to
 * the client (fd).
 *
 *   +--------+     +------------+     +--------------+
 *   | client | <-> | httpserver | <-> | proxy target |
 *   +--------+     +------------+     +--------------+
 */
void handle_proxy_request(int fd)
{
  struct hostent *host;
  struct in_addr **addr_list;
  host = gethostbyname(server_proxy_hostname);
  addr_list = (struct in_addr **)host->h_addr_list;
  char *ip_address = inet_ntoa(*addr_list[0]);

  struct sockaddr_in sin;
  bzero(&sin,sizeof(sin));
  sin.sin_family = AF_INET;
  sin.sin_addr.s_addr = inet_addr(ip_address);
  sin.sin_port = htons(server_proxy_port);
  int sd = socket(AF_INET, SOCK_STREAM, 0);
  connect(sd, (struct sockaddr*)&sin, sizeof(sin)); 

  pid_t pid = fork();
  if (pid == 0) {
    char buffer[1024];
    int num_bytes_read;
    int num_bytes_write;
    while (1) {
      num_bytes_read = read(sd, buffer, sizeof(buffer));
      if (num_bytes_read > 0) {
        num_bytes_write = write(fd, buffer, num_bytes_read);
        if (num_bytes_write < 0) {
          break;
        }
      } else {
        break;
      }
      close(sd);
    }
  } else {
    char buffer[1024];
    int num_bytes_read;
    int num_bytes_write;
    while (1) {
      num_bytes_read = read(fd, buffer, sizeof(buffer));
      if (num_bytes_read > 0) {
        num_bytes_write = write(sd, buffer, num_bytes_read);
        if (num_bytes_write < 0) {
          break;
        }
      } else {
        break;
      }
      close(fd);
    }
  }
}

/*
 * Opens a TCP stream socket on all interfaces with port number PORTNO. Saves
 * the fd number of the server socket in *socket_number. For each accepted
 * connection, calls request_handler with the accepted fd number.
 */
void serve_forever(int* socket_number, void (*request_handler)(int)) {

  struct sockaddr_in server_address, client_address;
  size_t client_address_length = sizeof(client_address);
  int client_socket_number;
  pid_t pid;

  *socket_number = socket(PF_INET, SOCK_STREAM, 0);
  if (*socket_number == -1) {
    fprintf(stderr, "Failed to create a new socket: error %d: %s\n", errno, strerror(errno));
    exit(errno);
  }

  int socket_option = 1;
  if (setsockopt(*socket_number, SOL_SOCKET, SO_REUSEADDR, &socket_option,
        sizeof(socket_option)) == -1) {
    fprintf(stderr, "Failed to set socket options: error %d: %s\n", errno, strerror(errno));
    exit(errno);
  }

  memset(&server_address, 0, sizeof(server_address));
  server_address.sin_family = AF_INET;
  server_address.sin_addr.s_addr = INADDR_ANY;
  server_address.sin_port = htons(server_port);

  if (bind(*socket_number, (struct sockaddr*) &server_address,
        sizeof(server_address)) == -1) {
    fprintf(stderr, "Failed to bind on socket: error %d: %s\n", errno, strerror(errno));
    exit(errno);
  }

  if (listen(*socket_number, 1024) == -1) {
    fprintf(stderr, "Failed to listen on socket: error %d: %s\n", errno, strerror(errno));
    exit(errno);
  }

  printf("Listening on port %d...\n", server_port);

  while (1) {

    client_socket_number = accept(*socket_number, (struct sockaddr*) &client_address,
        (socklen_t*) &client_address_length);
    if (client_socket_number < 0) {
      fprintf(stderr, "Error accepting socket: error %d: %s\n", errno, strerror(errno));
      continue;
    }

    printf("Accepted connection from %s on port %d\n", inet_ntoa(client_address.sin_addr),
        client_address.sin_port);

    pid = fork();
    if (pid > 0) {
      close(client_socket_number);
    } else if (pid == 0) {
      signal(SIGINT, SIG_DFL); // Un-register signal handler (only parent should have it)
      close(*socket_number);
      request_handler(client_socket_number);
      close(client_socket_number);
      exit(EXIT_SUCCESS);
    } else {
      fprintf(stderr, "Failed to fork child: error %d: %s\n", errno, strerror(errno));
      exit(errno);
    }
  }

  close(*socket_number);

}

int server_fd;
void signal_callback_handler(int signum) {
  printf("Caught signal %d: %s\n", signum, strsignal(signum));
  printf("Closing socket %d\n", server_fd);
  if (close(server_fd) < 0) perror("Failed to close server_fd (ignoring)\n");
  exit(signum);
}

char *USAGE = "Usage: ./httpserver --files www_directory/ --port 8000\n"
              "       ./httpserver --proxy inst.eecs.berkeley.edu:80 --port 8000\n";

void exit_with_usage() {
  fprintf(stderr, "%s", USAGE);
  exit(EXIT_SUCCESS);
}

int main(int argc, char **argv) {
  signal(SIGINT, signal_callback_handler);

  /* Default settings */
  server_port = 8000;
  server_files_directory = malloc(1024);
  getcwd(server_files_directory, 1024);
  server_proxy_hostname = "inst.eecs.berkeley.edu";
  server_proxy_port = 80;

  void (*request_handler)(int) = handle_files_request;

  int i;
  for (i = 1; i < argc; i++)
  {
    if (strcmp("--files", argv[i]) == 0) {
      request_handler = handle_files_request;
      free(server_files_directory);
      server_files_directory = argv[++i];
      if (!server_files_directory) {
        fprintf(stderr, "Expected argument after --files\n");
        exit_with_usage();
      }
    } else if (strcmp("--proxy", argv[i]) == 0) {
      request_handler = handle_proxy_request;

      char *proxy_target = argv[++i];
      if (!proxy_target) {
        fprintf(stderr, "Expected argument after --proxy\n");
        exit_with_usage();
      }

      char *colon_pointer = strchr(proxy_target, ':');
      if (colon_pointer != NULL) {
        *colon_pointer = '\0';
        server_proxy_hostname = proxy_target;
        server_proxy_port = atoi(colon_pointer + 1);
      } else {
        server_proxy_hostname = proxy_target;
        server_proxy_port = 80;
      }
    } else if (strcmp("--port", argv[i]) == 0) {
      char *server_port_string = argv[++i];
      if (!server_port_string) {
        fprintf(stderr, "Expected argument after --port\n");
        exit_with_usage();
      }
      server_port = atoi(server_port_string);
    } else if (strcmp("--help", argv[i]) == 0) {
      exit_with_usage();
    } else {
      fprintf(stderr, "Unrecognized option: %s\n", argv[i]);
      exit_with_usage();
    }
  }

  serve_forever(&server_fd, request_handler);

  return EXIT_SUCCESS;
}
