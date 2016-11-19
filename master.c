// Computer Networks project 2 B
// author: Marcin Gregorczyk (mg359198)

#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/select.h>
#include <time.h>

#include "err.h"

#define BUFFER_SIZE 2000
#define QUEUE_LENGTH 5
#define MAX_ARGS 15
#define MAX_ARG_LENGTH 100
#define MAX_CLIENTS 20
#define TIME_CYCLE 24 * 360

typedef struct radioClient {
  int active;
  int descriptor;
  char* host;
  int port;
  int owner;
  pthread_t thread;
} radioClient;


radioClient clients[MAX_CLIENTS];
int telnetSessions[MAX_CLIENTS];
fd_set fds, afds;

//mutex for accessing critical section
pthread_mutex_t mutex;

//buffers
char buffer[BUFFER_SIZE];
char arguments[MAX_ARGS][MAX_ARG_LENGTH];
char command[BUFFER_SIZE];
char vBuffer[BUFFER_SIZE];
char titleBuffer[BUFFER_SIZE];
char echoBuffer[BUFFER_SIZE];


//number of parameters in datagram
int argCount;

//reads int from char*
int extractInt(char* arg) {
  int res = 0;
  int len = strlen(arg);
  for(int i = 0; i < len; i++) {
    if(arg[i] < '0' || arg[i] > '9')
      return -1;
    res *= 10;
    if(res < 0)
      return 0;
    res += arg[i] -'0';
  }
  return res;
}

// Copies strings written in arguments[]
// and writes ssh command to be invoked in *command
void buildSshCmd() {
  strcpy(command, "ssh ");
  strcpy(command+4, arguments[1]);
  int shift = strlen(arguments[1]) +4;
  strcpy(command + shift, " 'bash -l -c \"player");
  shift += 20;
  for(int i = 2; i < argCount; i++) {
    strcpy(command + shift, " ");
    strcpy(command + shift + 1, arguments[i]);
    shift += strlen(arguments[i]) +1;
  }
  strcpy(command + shift, " 3>&2 2>&1 1>&3\"'");
  shift += 17;
  command[shift] = '\0';
}


//sends and UDP datagram
//if getResponse flag is set to 1, receives datagram and stores its content
//in titleBuffer
void echo_message(char* host, int port, char* msg, int getResponse) {
  int echo_sock;
  struct addrinfo addr_hints;
  struct addrinfo *addr_result;
  int flags, sflags, err;
  ssize_t rcv_len;
  struct sockaddr_in my_address;
  struct sockaddr_in srvr_address;
  socklen_t rcva_len;

  (void) memset(&addr_hints, 0, sizeof(struct addrinfo));
  
  addr_hints.ai_family = AF_INET;
  addr_hints.ai_socktype = SOCK_DGRAM;
  addr_hints.ai_protocol = IPPROTO_UDP;
  addr_hints.ai_flags = 0;
  addr_hints.ai_addrlen = 0;
  addr_hints.ai_addr = NULL;
  addr_hints.ai_canonname = NULL;
  addr_hints.ai_next = NULL;
  err = getaddrinfo(host, NULL, &addr_hints, &addr_result);
  if (err == EAI_SYSTEM) {
    syserr("getaddrinfo: %s", gai_strerror(err));
  }
  else if (err != 0) {
    fatal("getaddrinfo: %s", gai_strerror(err));
  }

  my_address.sin_family = AF_INET;
  my_address.sin_addr.s_addr =
      ((struct sockaddr_in*) (addr_result->ai_addr))->sin_addr.s_addr;
  my_address.sin_port = htons((uint16_t) port);

  freeaddrinfo(addr_result);

  echo_sock = socket(PF_INET, SOCK_DGRAM, 0);
  if (echo_sock < 0)
    syserr("socket");

  sflags = 0;
  rcva_len = (socklen_t) sizeof(my_address);
  ssize_t snd_len = sendto(echo_sock, msg, strlen(msg), sflags,
      (struct sockaddr *) &my_address, rcva_len);

  if(snd_len != strlen(msg))
    syserr("sendto");

  flags = 0;
  if(getResponse) {
    rcva_len = (socklen_t) sizeof(srvr_address);

    struct timeval timeout;
    fd_set timeoutSet;
    timeout.tv_sec = 3;

    FD_ZERO(&timeoutSet);
    FD_SET(echo_sock, &timeoutSet);

    int rv = select(FD_SETSIZE, &timeoutSet, NULL, NULL, &timeout);
    if(rv == -1)
      syserr("select");
    else if(rv == 0) {
      titleBuffer[0] = '\0';
    }
    else {
      rcv_len = recvfrom(echo_sock, titleBuffer, BUFFER_SIZE-1, flags,
                       (struct sockaddr*) &srvr_address, &rcva_len);
      titleBuffer[rcv_len] = '\0';
    }
  }

  close(echo_sock);
  
}


typedef struct threadArg {
  int id;
  int seconds;
  int length;
  char param[BUFFER_SIZE];
} threadArg;

//returns new player id, or -1 if error occured
int createPlayer(int owner) {
  int freeId = -1;
  for(int i = 0; i < MAX_CLIENTS; i++) {
    if(clients[i].active == 0) {
      freeId = i;
      break;
    }
  }
  if(freeId == -1)
    return -1;
  
  int argLen = strlen(arguments[1]);
  char* hostName = malloc(argLen+1);
  strcpy(hostName, arguments[1]);
  buildSshCmd(hostName);

  //invokes ssh command, and gets descriptor
  //to its stderr
  FILE *input = popen(command, "r");
  int desc = fileno(input);

  radioClient cl;
  cl.active = 1;
  cl.descriptor = desc;
  cl.host = hostName;
  cl.port = extractInt(arguments[6]);
  cl.owner = owner;
  clients[freeId] = cl;
  FD_SET(desc, &afds);
  return freeId;
}

//splits received message into arguments list
//returns number of arguments
int splitBuffer(int len) {
  int currentArg = 0;
  int currentPos = 0;
  int ctrl = 0;
  int cnt = 0;
  
  //gets rid of telnet control bytes
  for(int i = 0; i < len; i++) {
    if(ctrl == 0) {
      if(buffer[i] == 255) {
        ctrl = 1;
      }
      else {
        vBuffer[cnt] = buffer[i];
        cnt++;
      }
    }
    else if(ctrl == 1) {
      if(buffer[i] == 255) {
        ctrl = 0;
        vBuffer[cnt] = 255;
        cnt++;
      }
      else if(buffer[i] > 250) {
        ctrl = 2;
      }
      else {
        ctrl = 0;
      }
    }
    else if(ctrl == 2) {
      ctrl = 0;
    }
  }

  for(int i = 0; i < cnt; i++) {
    buffer[i] = vBuffer[i];
  }
  buffer[cnt] = '\0';
  len = strlen(buffer);

  //splits the rest of buffer
  for(int i = 0; i < len; i++) {
    if(isspace(buffer[i])) {
      if(currentPos > 0) {
        arguments[currentArg][currentPos] = '\0';
        currentArg++;
        currentPos = 0;
        if(currentArg >= MAX_ARGS)
          return 0;
      }
    } else {
      if(currentPos >= MAX_ARG_LENGTH)
        return 0;
      arguments[currentArg][currentPos] = buffer[i];
      currentPos++;
    }
  }
  arguments[currentArg][currentPos] = '\0';
  if(currentPos > 0)
    return currentArg +1;
  return currentArg;
}

int writeInt(char* buf, int n) {
  if(n == 0) {
    buf[0] = '0';
    return 1;
  }
  char tmp[100];
  int cnt = 0;
  while(n > 0) {
    tmp[cnt] = n % 10 + '0';
    n /= 10;
    cnt++;
  }
  int j = 0;
  for(int i = cnt-1; i >= 0; i--) {
    buf[j] = tmp[i];
    j++;
  }
  return cnt;
}

//builds response that will be sent to client
//and stores it in buffer
//returns responses length
int response(char* status, int id, char* rsp) {
  int len = strlen(status);
  strcpy(buffer, status);
  if(id == -1) {
    buffer[len] = '\n';
    buffer[len+1] = '\0';
    return len+1;
  }
  buffer[len] = ' ';
  len++;
  len += writeInt(buffer+len, id);
  if(rsp == NULL) {
    buffer[len] = '\n';
    buffer[len+1] = '\0';
    return len+1;
  }

  buffer[len] = ' ';
  len++;
  strcpy(buffer+len, rsp);
  len += strlen(rsp);
  buffer[len] = '\n';
  buffer[len+1] = '\0';
  return len+1;
}

void sendError(int desc, int id) {
  int l = response("ERROR", id, NULL);
  write(desc, buffer, l);
}

int getSeconds(char* arg) {
  int len = strlen(arg);
  if(len != 5)
    return -1;

  char buf[3];
  buf[2] = '\0';
  buf[0] = arg[0];
  buf[1] = arg[1];
  int hh = extractInt(buf);
  buf[0] = arg[3];
  buf[1] = arg[4];
  int mm = extractInt(buf);
  if(hh < 0 || hh > 23)
    return -1;
  if(mm < 0 || mm > 59)
    return -1;
  
  int targetSeconds = 360 * hh + 60*mm;
  time_t t = time(NULL);
  struct tm tm = *localtime(&t);
  int curTime = tm.tm_hour *360 + tm.tm_min *60 + tm.tm_sec;
  if(curTime <= targetSeconds) {
    return targetSeconds - curTime;
  }
  return TIME_CYCLE - curTime + targetSeconds;
}

void delayedThread(void* arg) {
  threadArg* targ = (threadArg*) arg;
  
  sleep(targ->seconds);
  
  pthread_mutex_lock(&mutex);
  
  clients[targ->id].thread = pthread_self();
  //invoke ssh command
  FILE *input = popen(targ->param, "r");
  int desc = fileno(input);
  clients[targ->id].active = 1;
  clients[targ->id].descriptor = desc;
  FD_SET(desc, &afds);
  
  pthread_mutex_unlock(&mutex);
  
  sleep(targ->length);

  pthread_mutex_lock(&mutex);
  
  if(pthread_self() == clients[targ->id].thread) {
    //shuts down a player
    echo_message(clients[targ->id].host, clients[targ->id].port, "QUIT", 0);
  }
  
  pthread_mutex_unlock(&mutex);
}

//starts a thread that will run a player
//at correct time
int delayedStart(int owner) {
  int freeId = -1;
  for(int i = 0; i < MAX_CLIENTS; i++) {
    if(clients[i].active == 0) {
      freeId = i;
      break;
    }
  }
  if(freeId == -1)
    return -1;
  
  int seconds = getSeconds(arguments[1]);
  if(seconds < 0)
    return -1;

  threadArg* arg = malloc(sizeof(threadArg));
  arg->seconds = seconds;
  arg->length = extractInt(arguments[2]) * 60;
  arg->id = freeId;
  if(arg->length < 0) {
    free(arg);
    return -1;
  }

  if(strcmp(arguments[7], "-") == 0) {
    free(arg);
    return -1;
  }

  int argLen = strlen(arguments[3]);
  char* hostName = malloc(argLen+1);
  strcpy(hostName, arguments[3]);
  
  radioClient cl;
  cl.active = 2;
  cl.host = hostName;
  cl.port = extractInt(arguments[8]);
  cl.owner = owner;
  clients[freeId] = cl;
  
  int cnt = 0;
  strcpy(arg->param, "ssh ");
  strcpy(arg->param+4, hostName);
  cnt = argLen +4;
  strcpy(arg->param + cnt, " 'bash -l -c \"player");
  cnt += 20;
  
  for(int i = 4; i < 10; i++) {
    strcpy(arg->param +cnt, " ");
    strcpy(arg->param +cnt +1, arguments[i]);
    cnt += strlen(arguments[i]) +1;
  }
  strcpy(arg->param + cnt, " 3>&2 2>&1 1>&3\"'");
  cnt += 17;
  arg->param[cnt] = '\0';

  pthread_t th;
  pthread_create(&th, NULL, (void*)delayedThread, (void*)arg);
  return freeId;
}

int main(int argc, char *argv[])
{
  if (argc != 2) {
    fatal("Usage: %s port\n", argv[0]);
  }

  int port = extractInt(argv[1]);
  if(port <= 0)
    fatal("Invalid port\n");


  pthread_mutex_init(&mutex, NULL);
  int sock;
  struct sockaddr_in server_address;
  struct sockaddr_in client_address;
  socklen_t client_address_len;

  ssize_t len, snd_len;;
  sock = socket(PF_INET, SOCK_STREAM, 0);
  if (sock < 0)
    syserr("socket");

  server_address.sin_family = AF_INET;
  server_address.sin_addr.s_addr = htonl(INADDR_ANY);
  server_address.sin_port = htons(port);

  if (bind(sock, (struct sockaddr *) &server_address, sizeof(server_address)) < 0)
    syserr("bind");

  if (listen(sock, QUEUE_LENGTH) < 0)
    syserr("listen");

  FD_ZERO(&afds);
  FD_SET(sock, &afds);
  
  for (;;) {
    
    fds = afds;
    if(select(FD_SETSIZE, &fds, NULL, NULL, NULL) < 0)
      syserr("select");

    pthread_mutex_lock(&mutex);

    //checks ssh sessions stderr descriptors
    for(int i = 0; i < MAX_CLIENTS; i++) {
      if(clients[i].active == 1) {
        if(FD_ISSET(clients[i].descriptor, &fds)) {
          len = read(clients[i].descriptor, buffer, BUFFER_SIZE-1);
          if(clients[i].owner != 0) {
            if(len == 0) {
              int l = response("Player", i, "closed");
              write(clients[i].owner, buffer, l);
            }
            else {
              int l = response("ERROR", i, NULL);
              write(clients[i].owner, buffer, l);
            }
          }
          close(clients[i].descriptor);
          FD_CLR(clients[i].descriptor, &afds);
          clients[i].active = 0;
          clients[i].thread = 0;
          free(clients[i].host);
        }
      }
    }

    //checks telnet session descriptors
    for(int i = 0; i < MAX_CLIENTS; i++) {
      if(telnetSessions[i] != 0 && FD_ISSET(telnetSessions[i], &fds)) {
        len = read(telnetSessions[i], buffer, BUFFER_SIZE-1);
        if (len < 0)
          syserr("read");
        else if(len == 0) {
          close(telnetSessions[i]);
          FD_CLR(telnetSessions[i], &afds);
          telnetSessions[i] = 0;
        }
        else {
          buffer[len] = '\0';
          argCount = splitBuffer(len);
          if(argCount == 0) {
            sendError(telnetSessions[i], -1);
          }
          else if(strcmp(arguments[0], "START") == 0) {
            if(argCount < 2) {
              sendError(telnetSessions[i], -1);
              continue;
            }
            int id = createPlayer(telnetSessions[i]);
            if(id == -1) {
              sendError(telnetSessions[i], -1);
              continue;
            }

            int l = response("OK", id, NULL);
            snd_len = write(telnetSessions[i], buffer, l);
            if (snd_len != l)
              syserr("write");
          } else if(strcmp(arguments[0], "PLAY") == 0 || strcmp(arguments[0], "PAUSE") == 0
                    || strcmp(arguments[0], "QUIT") == 0 || strcmp(arguments[0], "TITLE") == 0) {
            if(argCount != 2) {
              sendError(telnetSessions[i], -1);
              continue;
            }
            int id = extractInt(arguments[1]);
            if(id > MAX_CLIENTS || clients[id].active == 0) {
              sendError(telnetSessions[i], -1);
              continue;
            }

            if(clients[id].active == 2) {
              if(strcmp(arguments[0], "QUIT") == 0) {
                clients[id].active = 0;
                clients[id].thread = 0;
                free(clients[id].host);
                int l = response("OK", id, NULL);
                write(telnetSessions[i], buffer, l);
              }
              else {
                sendError(telnetSessions[i], id);
              }
            }
            else if(strcmp(arguments[0], "TITLE") == 0) {
              echo_message(clients[id].host, clients[id].port, arguments[0], 1);
              int l = response("OK", id, titleBuffer);
              write(telnetSessions[i], buffer, l);
            }
            else {
              echo_message(clients[id].host, clients[id].port, arguments[0], 0);
              int l = response("OK", id, NULL);
              write(telnetSessions[i], buffer, l);
            }
          } else if(strcmp(arguments[0], "AT") == 0) {
            int res = delayedStart(telnetSessions[i]);
            if(res < 0)
              sendError(telnetSessions[i], -1);
            else {
              int l = response("OK", res, NULL);
              write(telnetSessions[i], buffer, l);
            }
          } else {
            sendError(telnetSessions[i], -1);
          }
        }
      }
    }

    //accepts new telnet connection
    if(FD_ISSET(sock, &fds)) {
      int newCon = accept(sock, (struct sockaddr*) &client_address, &client_address_len);

      if(newCon < 0)
        syserr("cannot accept new connection");

      FD_SET(newCon, &afds);
      int fnd = 0;
      for(int j = 0; j < MAX_CLIENTS; j++) {
        if(telnetSessions[j] == 0) {
          telnetSessions[j] = newCon;
          fnd = 1;
          break;
        }
      }
      if(fnd == 0) {
        FD_CLR(newCon, &afds);
        close(newCon);
      }
    }
    pthread_mutex_unlock(&mutex);
  }
  return 0;
}
