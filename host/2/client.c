#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <sys/time.h>

#define TAILLE_BUFFER 516
#define TIMEOUT_SECONDES 5
#define MAX_TENTATIVES 5

#define OPCODE_RRQ 1
#define OPCODE_WRQ 2
#define OPCODE_DATA 3
#define OPCODE_ACK 4
#define OPCODE_ERROR 5

// Structure pour un paquet TFTP
struct paquet_tftp {
    unsigned short code_operation;
    union {
        unsigned short numero_b