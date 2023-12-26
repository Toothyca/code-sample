#define _GNU_SOURCE
#include <err.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <ctype.h>
#include <fcntl.h> // open
#include <unistd.h> // close
#include <sys/stat.h>
#include <errno.h>
#include <pthread.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#define BUFFERSIZE 1000

//queue implementation
struct node
{
	struct node *next;
	int *client_socket;	
};
typedef struct node node_t;

node_t *head = NULL;
node_t *tail = NULL;

//create a node and add it to the end of the queue
void enqueue(int *client_socket)
{
	node_t *newnode = malloc(sizeof(node_t));
	newnode->client_socket = client_socket;
	newnode->next = NULL;
	if (tail == NULL)
	{
		head = newnode;
	}
	else
	{
		tail->next = newnode;
	}
	tail = newnode;
}

//return NULL if queue is empty, otherwise return content of first node, destroy node, and update queue
int *dequeue()
{
	if (head == NULL)
	{
		return NULL;
	}
	else
	{
		int *result = head->client_socket;
		node_t *temp = head;
		head = head->next;
		if (head == NULL)
		{
			tail = NULL;
		}
		free(temp);
		return result;
	}
}

int logging = 0;
int logfd;
int logOffset = 0;

//mutex locks
pthread_mutex_t queueLock = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t logLock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t threadSig = PTHREAD_COND_INITIALIZER;

//flock lock
struct flock fileLock;

void enqueue(int *connfd);
int* dequeue();

void simpleResponseMessage(int connfd, int code);
int checkFile(char *fileName);

void *dispatcher_loop(void* args);
void *worker_loop(void* args);
void successLog(char command[], char file_name[], char host_number[], long int length, char content[], int fd);
void errorLog(char request[], int code, int fd);

/**
   Converts a string to an 16 bits unsigned integer.
   Returns 0 if the string is malformed or out of the range.
 */
uint16_t strtouint16(char number[]) {
  char *last;
  long num = strtol(number, &last, 10);
  if (num <= 0 || num > UINT16_MAX || *last != '\0') {
    return 0;
  }
  return num;
}

/**
   Creates a socket for listening for connections.
   Closes the program and prints an error message on error.
 */
int create_listen_socket(uint16_t port) {
  struct sockaddr_in addr;
  int listenfd = socket(AF_INET, SOCK_STREAM, 0);
  if (listenfd < 0) {
    err(EXIT_FAILURE, "socket error");
  }

  memset(&addr, 0, sizeof addr);
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htons(INADDR_ANY);
  addr.sin_port = htons(port);
  if (bind(listenfd, (struct sockaddr*)&addr, sizeof addr) < 0) {
    err(EXIT_FAILURE, "bind error");
  }

  if (listen(listenfd, 500) < 0) {
    err(EXIT_FAILURE, "listen error");
  }

  return listenfd;
}

void handle_connection(int connfd) {
  char requestBuffer[BUFFERSIZE];
  char command[BUFFERSIZE];
  char object[BUFFERSIZE];
  char version[BUFFERSIZE];

  char *header;
  char *host;
  char *contentl;
  char sub[BUFFERSIZE];

  char *saveptr;

  while(1) {
    //create buffers and clear them
    memset(requestBuffer, 0, BUFFERSIZE);
    memset(command, 0, BUFFERSIZE);
    memset(object, 0, BUFFERSIZE);
    memset(version, 0, BUFFERSIZE);

    //receive fd
    int requestBytes = recv(connfd, requestBuffer, BUFFERSIZE, 0);
    if (requestBytes == 0) {
      break;
    }

    if (requestBytes < 0) {
      simpleResponseMessage(connfd, 400);
      errorLog(header, 400, logfd);
      break;
    }

    //read into a buffer and parse request to see if you get, head, or put
    //check if it is in right format
    //printf("Request: |%s|\nRequest Bytes: %i\n", requestBuffer, requestBytes);
    
    //when parsing, care about arg 1(GET, HEAD, or PUT)
    // use sscanf to get command, filename, version
    header = strtok_r(requestBuffer, "\r\n", &saveptr);
    sscanf(header, "%s %s %s", command, object, version);

    int breakit = 1;
    char* token = strtok_r(NULL, "\r\n", &saveptr);
    while (token != NULL) {
      token = strtok_r(NULL, "\r\n", &saveptr);
      if((host = strstr(token, "Host: ")) != NULL) {
        breakit = 0;
        break;
      } 
    }
    if(breakit) {
      simpleResponseMessage(connfd, 400);
      if(logging) {
        errorLog(header, 400, logfd);
      }
      break;
    }

    //check validity of host (no spaces)
    int numspaces = 0;
    for (int i = 0; i < strlen(host); i++) {
      if(isspace(host[i])) {
        numspaces += 1;
      }
      if(numspaces > 1) {
        simpleResponseMessage(connfd, 400);
        if(logging) {
        errorLog(header, 400, logfd);
      }
        break;
      }
    }
    if(numspaces > 1) {
      break;
    }

    //remove / from beginning of file name
    strcpy(object, &object[1]);

    //check if alphanumeric
    if (!checkFile(object)) {
      simpleResponseMessage(connfd, 400);
      if(logging) {
        errorLog(header, 400, logfd);
      }
      break;
    }

    //if requesting healthcheck and its not a get, then return an error
    if(strcmp(object, "healthcheck") == 0) {
      if (strcmp(command, "GET") != 0) {
        simpleResponseMessage(connfd, 403);
        if(logging) {
          errorLog(header, 403, logfd);
        }
        break;
      }
    }

    //GET
    if (strcmp(command, "GET") == 0) {
      
      if(strcmp(object, "healthcheck") == 0) {
        if(logging) {
          //validate log file again
          int bytes_read;
          int numTabs = 0;

          int fails = 0;
          int lines = 0;
          //reuse logFile buffer as a buffer to read in characters from file
          char frogFile[BUFFERSIZE];
          memset(frogFile, 0, BUFFERSIZE);
          lseek(logfd, 0, SEEK_SET);
          int breakit = 0;
          while((bytes_read = read(logfd, frogFile, BUFFERSIZE)) > 0) {
            for(int i = 0; i < bytes_read; i++) {
              if(frogFile[i] == '\t') {
                numTabs += 1;
              }
              if(frogFile[i] == '\n') {
                if (numTabs < 2 || numTabs > 5) {
                  simpleResponseMessage(connfd, 500);
                  breakit = 1;
                  if(logging) {
                    errorLog(header, 500, logfd);
                  }
                  break;
                }
                numTabs = 0;
              }
              if(breakit) {
                break;
              }
              //count fails
              if((frogFile[i] == 'F') && ((i + 5) < bytes_read)) {
                if(frogFile[i+1] == 'A' && frogFile[i+2] == 'I' && frogFile[i+3] == 'L' && frogFile[i+4] == '\t') {
                  fails++;
                }       
              }
              //count entries
              if(frogFile[i] == '\n') {
                lines++;
              }
            }
          }
          if(breakit) {
            break;
          }

          //file validated
          //loop through each line of logfile
          //check if contains FAIL
          //save number of fails and number of lines
          //send to connfd

          //send message containing
          char response[BUFFERSIZE];
          char healthcontent[BUFFERSIZE];
          memset(response, 0, BUFFERSIZE);
          memset(healthcontent, 0, BUFFERSIZE);
          sprintf(healthcontent, "%i\n%i\n", fails, lines);
          sprintf(response, "HTTP/1.1 200 OK\r\nContent-Length: %ld\r\n\r\n", strlen(healthcontent));
          send(connfd, response, strlen(response), 0);
          send(connfd, healthcontent, strlen(healthcontent), 0);
          if(logging) {
            successLog(command, object, host, strlen(healthcontent), healthcontent, logfd);
          }
          

        } else {
          simpleResponseMessage(connfd, 403);
          break;
        }
      //not getting a healthcheck, getting a normal file
      } else {
        breakit = 0;
        int fd = open(object, O_RDONLY);
        if(fd < 0) {
          // how to know if access denied or if file does not exist?
          if(errno == EACCES) {
            simpleResponseMessage(connfd, 403);
            if(logging) {
            errorLog(header, 403, logfd);
            }
          } else if (ENOENT) {
            simpleResponseMessage(connfd, 404);
            if(logging) {
              errorLog(header, 404, logfd); 
            }
          } else {
            simpleResponseMessage(connfd, 500);
            if(logging) {
              errorLog(header, 500, logfd);
            }
          }
          breakit = 1;
          break;
        }
        if(breakit) {
          break;
        }

        memset(&fileLock, 0, sizeof(fileLock));
        fileLock.l_type = F_RDLCK;
        fcntl(fd, F_SETLKW, &fileLock);

        struct stat fileStat;
        fstat(fd, &fileStat);

        char response[BUFFERSIZE];
        memset(response, 0, BUFFERSIZE);
        sprintf(response, "HTTP/1.1 200 OK\r\nContent-Length: %ld\r\n\r\n", fileStat.st_size);
        send(connfd, response, strlen(response), 0);

        // actually send message
        int bytesRead;
        int sentOnce = 0;
        memset(response, 0, BUFFERSIZE);
        while((bytesRead = read(fd, response, BUFFERSIZE)) > 0) {
          send(connfd, response, bytesRead, 0);
          //printf("%s\nlen response: %li\nbytesRead: %i\nsentBytes: %i\n", response, strlen(response), bytesRead, sentBytes);
          if (logging & !(sentOnce)) {
            successLog(command, object, host, fileStat.st_size, response, logfd);
            sentOnce = 1;
          }
          memset(response, 0, BUFFERSIZE);
        }

        fileLock.l_type = F_UNLCK;
        fcntl(fd, F_SETLKW, &fileLock);
        close(fd);
      }
    //HEAD
    } else if (strcmp(command, "HEAD") == 0) {
      
      int fd = open(object, O_RDONLY);
      if(fd < 0) {
        if(errno == EACCES) {
          simpleResponseMessage(connfd, 403);
          if(logging) {
            errorLog(header, 403, logfd);
          }
        } else if (ENOENT) {
          simpleResponseMessage(connfd, 404);
          if(logging) {
            errorLog(header, 404, logfd);
          }
        } else {
          simpleResponseMessage(connfd, 500);
          if(logging) {
            errorLog(header, 500, logfd);
          }
        }
        break;
      }

      memset(&fileLock, 0, sizeof(fileLock));
      fileLock.l_type = F_RDLCK;
      fcntl(fd, F_SETLKW, &fileLock);

      struct stat fileStat;
      fstat(fd, &fileStat);

      char response[BUFFERSIZE];
      memset(response, 0, BUFFERSIZE);
      sprintf(response, "HTTP/1.1 200 OK\r\nContent-Length: %ld\r\n\r\n", fileStat.st_size);
      send(connfd, response, strlen(response), 0);
      successLog(command, object, host, fileStat.st_size, NULL, logfd);

      fileLock.l_type = F_UNLCK;
      fcntl(fd, F_SETLKW, &fileLock);
      close(fd);
    //PUT
    } else if (strcmp(command, "PUT") == 0) {
      strtok_r(NULL, "\r\n", &saveptr);
      contentl = strtok_r(NULL, "\r\n", &saveptr);
      
      sscanf(contentl, "%*s %s", sub);

      breakit = 0;
      for(int digit = 0; digit < strlen(sub); digit++) {
        if(!(isdigit(sub[digit]))) {
          simpleResponseMessage(connfd, 400);
          breakit = 1;
          if(logging) {
            errorLog(header, 400, logfd);
          }
          break;
        }
      }
      if(breakit) {
        break;
      }
      int content_length = atoi(sub);

      //check to see if the file in the header exists and if it is writeable (return error if not)
      //if it doesn't exist, then create it
      int neededCreation = 0;
      if((open(object, O_RDWR | O_TRUNC) < 0 && errno == ENOENT)) {
        neededCreation = 1;
      }
      int fd = open(object, O_RDWR | O_TRUNC | O_CREAT, 0755);

      //read locking file
      memset(&fileLock, 0, sizeof(fileLock));
      fileLock.l_type = F_WRLCK;
      fcntl(fd, F_SETLKW, &fileLock);

      //if just open, it's an OK
      int code = 200;
      if(fd < 0) {
        // how to know if access denied or if file does not exist?
        if(errno == EACCES) {
          simpleResponseMessage(connfd, 403);
          if(logging) {
            errorLog(header, 403, logfd);
          }
          break;
        } else if (ENOENT) {
          //if you need to create, it's a 201
          fd = open(object, O_CREAT, 0755);
          if(fd < 0) {
            simpleResponseMessage(connfd, 500);
            if(logging) {
              errorLog(header, 500, logfd);
            } break;
          }
          code = 201;
        } else {
          simpleResponseMessage(connfd, 500);
          if(logging) {
            errorLog(header, 500, logfd);
          } break;
        }
      }

      if(neededCreation) {
        code = 201;
      }

      // read in body
      int reqBytes;
      char newBuffer[BUFFERSIZE];
      memset(newBuffer, 0, BUFFERSIZE);

      int max;
      int sentOnce = 0;
      while(content_length != 0) {
        if(content_length > BUFFERSIZE) {
          reqBytes = recv(connfd, newBuffer, BUFFERSIZE, 0);
        } else {
          reqBytes = recv(connfd, newBuffer, content_length, 0);
        }

        if (reqBytes >= BUFFERSIZE) {
          max = BUFFERSIZE;
        } else {
          max = reqBytes;
        }
        if(write(fd, newBuffer, reqBytes) == 0){
          break;
        }
        
        if (logging & !(sentOnce)) {
        successLog(command, object, host, content_length, newBuffer, logfd);
        sentOnce = 1;
      }

        content_length -= max;
        memset(newBuffer, 0, BUFFERSIZE);
        if (content_length <= 0 || max <= 0) {
          break;
        }
      }

      fileLock.l_type = F_UNLCK;
      fcntl(fd, F_SETLKW, &fileLock);
      simpleResponseMessage(connfd, code);
      close(fd);

    //UNIMPLEMENTED COMMAND
    } else {
      simpleResponseMessage(connfd, 501);
      if(logging) {
        errorLog(header, 501, logfd);
      }
      break;
    }
  }
  // when done, close socket
  close(connfd);
}

// helper function to send out code
void simpleResponseMessage(int connfd, int code) {
  switch(code) {
    case 200:
      send(connfd, "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nOK\n", strlen("HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nOK\n"), 0);
      break;
    case 201:
      send(connfd, "HTTP/1.1 201 Created\r\nContent-Length: 8\r\n\r\nCreated\n", strlen("HTTP/1.1 201 Created\r\nContent-Length: 8\r\n\r\nCreated\n"), 0);
      break;
    case 400:
      send(connfd, "HTTP/1.1 400 Bad Request\r\nContent-Length: 12\r\n\r\nBad Request\n", strlen("HTTP/1.1 400 Bad Request\r\nContent-Length: 12\r\n\r\nBad Request\n"), 0);
      break;
    case 403:
      send(connfd, "HTTP/1.1 403 Forbidden\r\nContent-Length: 10\r\n\r\nForbidden\n", strlen("HTTP/1.1 403 Forbidden\r\nContent-Length: 10\r\n\r\nForbidden\n"), 0);
      break;
    case 404:
      send(connfd, "HTTP/1.1 404 File Not Found\r\nContent-Length: 15\r\n\r\nFile Not Found\n", strlen("HTTP/1.1 404 File Not Found\r\nContent-Length: 15\r\n\r\nFile Not Found\n"), 0);
      break;
    case 500:
      send(connfd, "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 22\r\n\r\nInternal Server Error\n", strlen("HTTP/1.1 500 Internal Server Error\r\nContent-Length: 22\r\n\r\nInternal Server Error\n"), 0);
      break;
    case 501:
      send(connfd, "HTTP/1.1 501 Not Implemented\r\nContent-Length: 16\r\n\r\nNot Implemented\n", strlen("HTTP/1.1 501 Not Implemented\r\nContent-Length: 16\r\n\r\nNot Implemented"), 0);
      break;
  }    
}

// checks validity of file name. returns 1 is so, 0 if not
int checkFile(char *fileName) {
  if (strlen(fileName) > 19) {
    return 0;
  }
  for(int i = 0; i < strlen(fileName); i++) {
    if (!(isalnum(fileName[i])) && fileName[i] != '.' && fileName[i] != '_') {
      return 0;
    }
  }
  return 1;
}

//ASGN2 FUNCTIONS
void *worker_loop(void* args) {
  while(1) {
    pthread_mutex_lock(&queueLock);
    int *clientfd;
    if((clientfd = dequeue()) == NULL) {
      pthread_cond_wait(&threadSig, &queueLock);
      clientfd = dequeue();
    }
    pthread_mutex_unlock(&queueLock);
    if (clientfd != NULL) {
      int passfd = *clientfd;
      free(clientfd);
      handle_connection(passfd);
    }
  }
}

void successLog(char command[], char file_name[], char host_number[], long int content_length, char content[], int fd) {
  char log[2000];
  char byte[5];
  memset(log, 0, 2000);
  memset(byte, 0, 5);

  sprintf(log, "%s\t%s\t%s\t%ld", command, file_name, host_number, content_length);
  if ((strcmp(command, "GET") == 0) || (strcmp(command, "PUT") == 0)) {
    int min;
    if (content_length > 1000) {
      min = 1000;
    } else {
      min = content_length;
    }
    strcat(log, "\t");
    for (int i = 0; i < min; i++) {
      
      if(content[i] == '\r' && ((i+1 < min) && content[i+1] == '\n')) {
        break;
      }
      
      sprintf(byte, "%02hhx", content[i]);
      strcat(log, byte);
    }
  }
  
  strcat(log, "\n");
  pthread_mutex_lock(&logLock);
  
  int written;
  if((written = pwrite(fd, log, strlen(log), logOffset)) < 0) {
    printf("errno: %i\n", errno);
  } else {
    logOffset += written;
  }  
  pthread_mutex_unlock(&logLock);
}

void errorLog(char request[], int code, int fd) {
  char log[2000];
  char byte[5];
  memset(log, 0, 2000);
  memset(byte, 0, 5);
  
  sprintf(log, "FAIL\t");
  int min;
  if (strlen(request) > 1000) {
    min = 1000;
  } else {
    min = strlen(request);
  }
  for (int i = 0; i < min; i++) {
    if(request[i] == '\r' && ((i+1 < min) && request[i+1] == '\n')) {
      break;
    }
    sprintf(byte, "%c", request[i]);
    strcat(log, byte);
  }
  strcat(log, "\t");
  memset(byte, 0, 5);
  sprintf(byte, "%i\n", code);
  strcat(log, byte);
  
  pthread_mutex_lock(&logLock);
  int written = pwrite(fd, log, strlen(log), logOffset);
  logOffset += written;
  
  pthread_mutex_unlock(&logLock);
}

int main(int argc, char *argv[]) {
  int listenfd;
  uint16_t port;

  if (argc < 2 || argc > 6) {
    errx(EXIT_FAILURE, "invalid number of arguments");
  }

  //default port number to check if port was specified in arguments
  listenfd = 0;
  port = 0;

  //default number of worker threads is 5, will be changed if user specifies otherwise
  int numThreads = 5;

  char logFile[BUFFERSIZE];
  memset(logFile, 0, BUFFERSIZE);

  //check for l and N flags
  int opt;
  while((opt = getopt(argc, argv, ":l:N:")) != -1) {
    switch(opt) {
      case 'l':
        strcpy(logFile, optarg);
        logging = 1;
        break;
      case 'N':
        if (!(strspn(optarg, "012345789") == strlen(optarg)) || atoi(optarg) == 0) {
          errx(EXIT_FAILURE, "invalid thread number: %s", optarg);
        } else {
          numThreads = atoi(optarg);
        }
        break;
      case ':':
        if (opt == 'l') {
          errx(EXIT_FAILURE, "no file log specified");
        }
        else if (opt == 'N') {
          errx(EXIT_FAILURE, "no thread number specified");
        }
        break;
      case '?':
        errx(EXIT_FAILURE, "invalid flag: %d", opt);
        break;
    }
  }

  
  if((argc - optind) != 1) {
    errx(EXIT_FAILURE, "invalid number of arguments");
  }

  //check and connect to port
  port = strtouint16(argv[optind]);
  if (port == 0) {
    errx(EXIT_FAILURE, "invalid port number: %s", argv[optind]);
  }
  listenfd = create_listen_socket(port);

  //if file logging, check if file exists
  //if file exists, check validity of file
  //otherwise create log file
  if(logging) {
    logfd = open(logFile, O_RDWR | O_APPEND | O_CREAT, 0755);
    if(logfd < 0) {
      // how to know if access denied or if file does not exist?
      if(errno == EACCES) {
        errx(EXIT_FAILURE, "invalid permissions for preexisting log file");
      } else if (ENOENT) {
        //if you need to create, it's a 201
        logfd = open(logFile, O_CREAT | O_APPEND, 0755);
        if(logfd < 0) {
          errx(EXIT_FAILURE, "unable to create log file");
        }
      }
    } else {
      //check validity of log file
      //load in a buffer of SIZE until full file has been read
      //iterate through buffer a character at a time, counting how many tabs there are per new line. if too many tabs, then ???
      int bytes_read;
      int numTabs = 0;
      //reuse logFile buffer as a buffer to read in characters from file
      memset(logFile, 0, BUFFERSIZE);
      while((bytes_read = read(logfd, logFile, BUFFERSIZE)) > 0) {
        for(int i = 0; i < bytes_read; i++) {
          if(logFile[i] == '\t') {
            numTabs += 1;
          }
          if(logFile[i] == '\n') {
            if (numTabs < 2 || numTabs > 5) {
              errx(EXIT_FAILURE, "specified log file doesn't follow format");
            }
            numTabs = 0;
          }
        }
      }
    }
  }

  //create array of worker threads
  pthread_t threads[numThreads];
  for (int i = 0; i < numThreads; i++) {
    pthread_create(&threads[i], NULL, &worker_loop, NULL);
  }
  
  if(pthread_mutex_init(&queueLock, NULL) != 0) {
    errx(EXIT_FAILURE, "failed mutex");
  }
  if(pthread_mutex_init(&logLock, NULL) != 0) {
    errx(EXIT_FAILURE, "failed mutex");
  }

  while(1) {
    int connfd = accept(listenfd, NULL, NULL);
    if (connfd < 0) {
      warn("accept error");
      continue;
    }
    int *clientfd = malloc(sizeof(int));
    *clientfd = connfd;
    pthread_mutex_lock(&queueLock);
    enqueue(clientfd);
    pthread_cond_signal(&threadSig);
    pthread_mutex_unlock(&queueLock);
  }
  
  return EXIT_SUCCESS;
}