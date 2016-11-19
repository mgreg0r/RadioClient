// Computer Networks project 2 A
// author: Marcin Gregorczyk (mg359198)

#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>

#include "err.h"

#define BUFFER_SIZE 2
#define CMD_BUF_SIZE 10
#define LINE_SIZE 1000000
#define META_SIZE 4099

const char* META_IDENTIFIER = "icy-metaint:";
const char* TITLE_IDENTIFIER = "StreamTitle=";

char* resource;
char* file;
int fileDesc;
int metaInt;

//radio server address info
struct addrinfo *addr_result;

//socket for communication with radio server
int radioSock;

//socket for receiving commands
int serverSock;

//0 = no, 1 = yes
int metaFlag;

//byte counter for tracking meta information in radio stream
int byteCount;

//meta information length
int metaLength;

//byte counter for received meta information
int metaCount;

//buffers
char line[LINE_SIZE];
char metaData[META_SIZE];
char title[META_SIZE];
char buffer[BUFFER_SIZE];
char cmdBuf[CMD_BUF_SIZE];

//fd_sets for select
fd_set fds, afds;

//0 = paused, 1 = playing
int playing = 0;

int lineIndex;
int headerFinished;


void terminateProgram(int code) {
  (void) close(radioSock);
  (void) close(serverSock);
  if(strcmp(file, "-") != 0)
    (void) close(fileDesc);
  exit(code);
}

// Parses single portion of metadata, and stores stream title
// in global variable "title" if it's found
void parseMeta() {
  metaData[metaCount] = '\0';
  char *beg = strstr(metaData, "StreamTitle=");
  if(beg == NULL)
    return;
  
  memset(title, 0, META_SIZE);
  char *end = metaData + metaCount;
  beg += strlen(TITLE_IDENTIFIER) +1;
  int cnt = 0;
  
  while(beg != end) {
    if(*beg == '\'' && *(beg+1) == ';')
      return;

    title[cnt] = *beg;
    beg++;
    cnt++;
  }
}

// Parses single line of http response header
// It detects end of the header,
// and stores metaInt in global variable if it's present
void parseLine() {
  line[lineIndex] = '\0';
  if(lineIndex == 0 || (lineIndex == 1 && line[0] == '\r')) {
    headerFinished = 1;
    return;
  }
  
  int mlen = strlen(META_IDENTIFIER);
  if(lineIndex < mlen + 1)
    return; 

  char tmpBuf[mlen + 1];
  for(int i = 0; i < mlen; i++)
    tmpBuf[i] = line[i];

  tmpBuf[mlen] = '\0';

  int res = 0;
  if(strcmp(tmpBuf, META_IDENTIFIER) == 0) {
    for(int i = mlen; i < lineIndex; i++) {
      if(line[i] >= '0' && line[i] <= '9') {
        res *= 10;
        if(res < 0)
          return;
        res += line[i]-'0';
      }
      else if(line[i] != '\r')
        return;
    }
    metaInt = res;
  }
}

// Connects to server and starts playing radio
void radioPlay() {
  
  byteCount = 0;
  metaLength = 0;
  metaCount = 0;
  metaInt = -1;
  headerFinished = 0;
  
  radioSock = socket(addr_result->ai_family, addr_result->ai_socktype, addr_result->ai_protocol);
  if (radioSock < 0)
    syserr("socket");
  
  if (connect(radioSock, addr_result->ai_addr, addr_result->ai_addrlen) < 0)
    syserr("connect");

  FD_ZERO(&afds);
  FD_SET(radioSock, &afds);
  FD_SET(serverSock, &afds);

  // send http GET request
  char* bf;
  if(metaFlag == 0)
    bf = "GET / HTTP/1.1\r\n\r\n";
  else bf = "GET / HTTP/1.1\r\nIcy-MetaData:1\r\n\r\n";
  
  if (write(radioSock, bf, strlen(bf)) != strlen(bf)) {
      syserr("write");
  }
  
  lineIndex = 0;

  // Receive header
  while(headerFinished == 0) {
    buffer[0] = 0;
    ssize_t rcv_len = read(radioSock, buffer, 1);
    if(rcv_len < 0)
      syserr("read");
    
    char next = buffer[0];
    if(next == '\n') {
      if(lineIndex < LINE_SIZE)
        parseLine(lineIndex);
      lineIndex = 0;
    }
    else {
      if(lineIndex < LINE_SIZE) {
        line[lineIndex] = next;
        lineIndex++;
      }
    }
  }
  playing = 1;
}


//Disconnects from radio server and stops playing
void radioPause() {
  playing = 0;
  FD_ZERO(&afds);
  FD_SET(serverSock, &afds);
  (void) close(radioSock);
}

//Receives command from serverSocket
void readCommand() {
  struct sockaddr_in client_address;
  socklen_t rcva_len = (socklen_t) sizeof(client_address);
  socklen_t snda_len = (socklen_t) sizeof(client_address);
  int flags = 0;
  int sflags = 0;
  int snd_len;
  
  int len = recvfrom(serverSock, cmdBuf, sizeof(cmdBuf)-1, flags,
    (struct sockaddr *) &client_address, &rcva_len);
  
  if (len < 0)
    syserr("error on datagram from client socket");
  else {
    cmdBuf[len] = '\0';
    
    if(strcmp(cmdBuf, "QUIT") == 0)
      exit(0);
    
    else if(strcmp(cmdBuf, "TITLE") == 0) {
      len = strlen(title);
      snd_len = sendto(serverSock, title, (size_t) len, sflags,
                (struct sockaddr *) &client_address, snda_len);
      if (snd_len != len)
        syserr("error on sending datagram to client socket");
    }
    else if(strcmp(cmdBuf, "PAUSE") == 0)
      radioPause();
    else if(strcmp(cmdBuf, "PLAY") == 0)
      radioPlay();
    else fprintf(stderr, "ignored invalid command: %s\n", cmdBuf);    
  }
}


//Reads byte of data from radio server, and handles it
void readRadio() {
  buffer[0] = 0;
  int rcv_len = read(radioSock, buffer, 1);
  if (rcv_len < 0) {
    syserr("read");
  }

  if(rcv_len == 0) {
    terminateProgram(0);
  }
  if(metaInt != -1 && byteCount == metaInt) {
    //byte contains meta block length
    metaLength = buffer[0] * 16;
    byteCount = 0;
  }
  else if(metaLength > 0) {
    //byte contains meta information
    metaData[metaCount] = buffer[0];
    metaCount++;
    metaLength--;
    if(metaLength == 0) {
      parseMeta();
      metaCount = 0;
    }
  }
  else {
    //byte contains audio stream
    write(fileDesc, buffer, 1);
    byteCount++;
  }
}

int getPort(char* arg) {
  int res = 0;
  int len = strlen(arg);
  for(int i = 0; i < len; i++) {
    if(arg[i] < '0' || arg[i] > '9')
      return 0;
    res *= 10;
    if(res < 0)
      return 0;
    res += arg[i] -'0';
  }
  return res;
}

int main(int argc, char *argv[])
{
  struct sockaddr_in server_address;

  if (argc != 7) {
    fatal("Usage: %s host path r-port file m-port md\n", argv[0]);
  }

  resource = argv[2];
  file = argv[4];

  if(strcmp(file, "-") == 0)
    fileDesc = 1;
  else {
    fileDesc = open(file, O_CREAT | O_WRONLY, S_IRUSR | S_IWUSR);
  }
  
  metaFlag = 0;
  if(strcmp(argv[6], "yes") == 0) metaFlag = 1;
  metaInt = -1;
  
  serverSock = socket(AF_INET, SOCK_DGRAM, 0);
  if (serverSock < 0)
    syserr("socket");
  
  server_address.sin_family = AF_INET;
  server_address.sin_addr.s_addr = htonl(INADDR_ANY);
  server_address.sin_port = htons(getPort(argv[5]));

  if (bind(serverSock, (struct sockaddr *) &server_address,
     (socklen_t) sizeof(server_address)) < 0)
      syserr("bind");

  int err;

  struct addrinfo addr_hints;
  
  memset(&addr_hints, 0, sizeof(struct addrinfo));
  addr_hints.ai_family = AF_INET;
  addr_hints.ai_socktype = SOCK_STREAM;
  addr_hints.ai_protocol = IPPROTO_TCP;
  err = getaddrinfo(argv[1], argv[3], &addr_hints, &addr_result);
  if (err == EAI_SYSTEM) {
    syserr("getaddrinfo: %s", gai_strerror(err));
  }
  else if (err != 0) {
    fatal("getaddrinfo: %s", gai_strerror(err));
  }

  struct timeval timeout;
  fd_set timeoutSet;
  timeout.tv_sec = 5;
  
  radioPlay();

  FD_ZERO(&timeoutSet);
  FD_SET(radioSock, &timeoutSet);
  
  while(666) {
    fds = afds;
    if(playing) {
      int rv = select(FD_SETSIZE, &timeoutSet, NULL, NULL, &timeout);
      if(rv == -1)
        syserr("select");
      else if(rv == 0) {
        terminateProgram(1);
      }
      FD_ZERO(&timeoutSet);
      FD_SET(radioSock, &timeoutSet);
      if(select(FD_SETSIZE, &fds, NULL, NULL, NULL) < 0)
        syserr("select");
      for(int i = 0; i < FD_SETSIZE; i++) {
        if(FD_ISSET(i, &fds)) {
          if(i == radioSock) {
            readRadio();
          }
          else if(i == serverSock) {
            readCommand();
            break;
          }
        }
      }
    }
    else {
      if(select(FD_SETSIZE, &fds, NULL, NULL, NULL) < 0)
        syserr("select");

      for(int i = 0; i < FD_SETSIZE; i++) {
        if(FD_ISSET(i, &fds)) {
          if(i == serverSock) {
            readCommand();
          }
        }
      }
    }
  }

  terminateProgram(0);
  return 0;
}
