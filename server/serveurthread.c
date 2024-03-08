#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h> 

#define BUFFER_SIZE 516 
#define SERVER_IP "127.0.0.1"
#define TIMEOUT_SECONDS 5 
#define MAX_ATTEMPTS 3
#define OPCODE_RRQ 1
#define OPCODE_WRQ 2
#define OPCODE_DATA 3
#define OPCODE_ACK 4
#define OPCODE_ERROR 5

// ***** Structure de la requête RRQ/WRQ ******
struct tftp_request {
    unsigned short opcode;
    char filename[BUFFER_SIZE - 2];
};

// ***** Structure du paquet DATA ******
struct tftp_data_packet {
    unsigned short opcode;
    unsigned short block_num;
    char data[BUFFER_SIZE - 4];
};

// ****** Structure du paquet ACK ******
struct tftp_ack_packet {
    unsigned short opcode;
    unsigned short block_num;
};
struct thread_args {
    int numero;
    struct sockaddr_in si_client;
    struct tftp_request request;
};


void die(char *s) {
    perror(s);
    exit(1);
}

// ****** Mutex pour la synchronisation des accès aux fichiers ******
pthread_mutex_t file_mutex = PTHREAD_MUTEX_INITIALIZER;

// ****** Initialisation du socket ******
int init_socket() {
    int socket_fd;
    if ((socket_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == -1) {
        die("socket");
    }
    return socket_fd;
}

// ****** Réception de la requête RRQ/WRQ ******
void receive_request(int socket_fd, struct sockaddr_in *si_client, struct tftp_request *request) {
    socklen_t slen = sizeof(*si_client);
    int bytes_received = recvfrom(socket_fd, request, sizeof(*request), 0, (struct sockaddr *)si_client, &slen);
    if (bytes_received == -1) {
        die("recvfrom()");
    }
}

// ****** Envoi du paquet DATA ****** 
void send_data(int socket_fd, struct sockaddr_in *si_client, struct tftp_data_packet *data_packet, int data_size) {
    if (sendto(socket_fd, data_packet, data_size, 0, (struct sockaddr *)si_client, sizeof(*si_client)) == -1) {
        die("sendto()");
    }
}

// ****** Réception du paquet ACK ******
void receive_ack(int socket_fd, struct sockaddr_in *si_client, struct tftp_ack_packet *ack_packet) {
    socklen_t slen = sizeof(*si_client);
    int bytes_received = recvfrom(socket_fd, ack_packet, sizeof(*ack_packet), 0, (struct sockaddr *)si_client, &slen);
    if (bytes_received == -1) {
        die("recvfrom()");
    }
}

// ****** Envoi du paquet ACK ******
void send_ack(int socket_fd, struct sockaddr_in *si_client, unsigned short block_num) {
    struct tftp_ack_packet ack_packet;
    ack_packet.opcode = htons(OPCODE_ACK);
    ack_packet.block_num = htons(block_num);
    if (sendto(socket_fd, &ack_packet, sizeof(ack_packet), 0, (struct sockaddr *)si_client, sizeof(*si_client)) == -1) {
        die("sendto()");
    }
}

// ****** Fonction pour traiter une requête RRQ ******
void process_rrq(struct sockaddr_in *si_client, struct tftp_request *request) {
    char *filename = request->filename;
    printf("****** Requête de lecture reçue pour le fichier ****** : %s\n", filename);

    // ******* Créer un nouveau socket pour la session RRQ *******
    int rrq_socket_fd = init_socket();

    // ******* Mutex lock avant l'accès au fichier *******
    pthread_mutex_lock(&file_mutex);

    // ****** Ouvrir le fichier demandé ******
    FILE *file = fopen(filename, "rb");
    if (file == NULL) {
        // ******* En cas d'erreur, envoyer un paquet d'erreur au client ******
        struct tftp_data_packet error_packet;
        error_packet.opcode = htons(OPCODE_ERROR);
        error_packet.block_num = htons(1); 
        strcpy(error_packet.data, "****** File not found or access denied ******");
        send_data(rrq_socket_fd, si_client, &error_packet, strlen("File not found or access denied") + 4);
        fclose(file);

        // ******* Mutex unlock après l'accès au fichier ******
        pthread_mutex_unlock(&file_mutex);
        close(rrq_socket_fd);
        return;
    }

    // ****** Lire et envoyer les données par blocs ****** 
    struct tftp_data_packet data_packet;
    int block_num = 1;
    size_t bytes_read;
    do {
        bytes_read = fread(data_packet.data, 1, BUFFER_SIZE - 4, file);
        if (bytes_read < 0) {
            die("fread");
        }
        data_packet.opcode = htons(OPCODE_DATA);
        data_packet.block_num = htons(block_num);
        send_data(rrq_socket_fd, si_client, &data_packet, bytes_read + 4);

        // ****** Attendre le paquet ACK du client ******
        struct tftp_ack_packet ack_packet;
        receive_ack(rrq_socket_fd, si_client, &ack_packet);
        if (ack_packet.opcode != htons(OPCODE_ACK) || ack_packet.block_num != htons(block_num)) {
            // ****** Si le paquet ACK n'est pas conforme, réessayer ******
            fclose(file);

            // ****** Mutex unlock après l'accès au fichier ******
            pthread_mutex_unlock(&file_mutex);
            close(rrq_socket_fd);
            return;
        }
        block_num++;
    } while (bytes_read == BUFFER_SIZE - 4);

    fclose(file);

    // ****** Mutex unlock après l'accès au fichier ******
    pthread_mutex_unlock(&file_mutex);

    // ****** Fermer le socket RRQ ******
    close(rrq_socket_fd);
}

// ****** Fonction pour traiter une requête WRQ ******
void process_wrq(struct sockaddr_in *si_client, struct tftp_request *request) {
    char *filename = request->filename;
    printf("****** Requête d'écriture reçue pour le fichier ****** : %s\n", filename);

    // ******* Créer un nouveau socket pour la session WRQ ******
    int wrq_socket_fd = init_socket();

    // ******* Mutex lock avant l'accès au fichier ******
    pthread_mutex_lock(&file_mutex);

    // ****** Ouvrir le fichier pour écriture ******
    FILE *file = fopen(filename, "wb");
    if (file == NULL) {
        // ****** En cas d'erreur, envoyer un paquet d'erreur au client ******
        struct tftp_data_packet error_packet;
        error_packet.opcode = htons(OPCODE_ERROR);
        error_packet.block_num = htons(1);
        strcpy(error_packet.data, "****** Error opening file for writing ******");
        send_data(wrq_socket_fd, si_client, &error_packet, strlen("****** Error opening file for writing ****** ") + 4);
        fclose(file);

        // ****** Mutex unlock après l'accès au fichier ******
        pthread_mutex_unlock(&file_mutex);
        close(wrq_socket_fd);
        return;
    }

    // ****** Envoyer le premier paquet ACK au client ******
    send_ack(wrq_socket_fd, si_client, 0);

    // ****** Attendre les paquets DATA et les écrire dans le fichier ******
    struct tftp_data_packet data_packet;
    int block_num = 1;
    while (1) {
        receive_request(wrq_socket_fd, si_client, (struct tftp_request *)&data_packet);

        // ****** Vérifier le code d'opération ******
        if (ntohs(data_packet.opcode) != OPCODE_DATA) {
            printf("****** Unexpected packet opcode *******.\n");
            fclose(file);

            // ****** Mutex unlock après l'accès au fichier ******
            pthread_mutex_unlock(&file_mutex);
            close(wrq_socket_fd);
            return;
        }

        // ****** Vérifier le numéro de bloc ******
        if (ntohs(data_packet.block_num) != block_num) {
            printf("****** Unexpected block number ******.\n");
            fclose(file);

            // ****** Mutex unlock après l'accès au fichier ****** 
            pthread_mutex_unlock(&file_mutex);
            close(wrq_socket_fd);
            return;
        }

        // ****** Écrire les données dans le fichier ******
        fwrite(data_packet.data, 1, strlen(data_packet.data), file);

        // ****** Envoyer un acquittement (ACK) au client ******
        send_ack(wrq_socket_fd, si_client, block_num);

        // ****** Vérifier si c'est le dernier paquet de données ******
        if (strlen(data_packet.data) < BUFFER_SIZE - 4) {
            break;
        }

        block_num++;
    }

    fclose(file);

    // ****** Mutex unlock après l'accès au fichier ******
    pthread_mutex_unlock(&file_mutex);

    // ****** Fermer le socket WRQ ******
    close(wrq_socket_fd);
}

// ****** Nouvelle fonction pour traiter les requêtes dans un thread ******
void* process_request(void* arg) {
    struct thread_args* args = (struct thread_args*)arg;
    struct sockaddr_in si_client = args->si_client;
    struct tftp_request request = args->request;

    // ****** Libérer la mémoire des arguments ******
    free(args);

    // ****** Traiter la requête RRQ ou WRQ ****** 
    if (request.opcode == htons(OPCODE_RRQ)) {
        process_rrq(&si_client, &request);
    } else if (request.opcode == htons(OPCODE_WRQ)) {
        process_wrq(&si_client, &request);
    }

    return NULL;
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        printf("Usage: %s <port_number>\n", argv[0]);
        exit(1);
    }

    int server_port = atoi(argv[1]);
    if (server_port <= 0 || server_port > 65535) {
        printf("Invalid port number.\n");
        exit(1);
    }

    int socket_fd = init_socket();

    struct sockaddr_in si_server, si_client;
    memset((char *)&si_server, 0, sizeof(si_server));
    si_server.sin_family = AF_INET;
    si_server.sin_port = htons(server_port);
    si_server.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(socket_fd, (struct sockaddr *)&si_server, sizeof(si_server)) == -1) {
        die("bind");
    }
    int i=1;
    printf("****** Serveur TFTP démarré sur le port ****** %d...\n", server_port);
    while (1) {
    struct tftp_request request;
    struct sockaddr_in si_client;
    receive_request(socket_fd, &si_client, &request);

    //****** Créer une structure d'arguments pour le thread ******
    struct thread_args* args = malloc(sizeof(struct thread_args));
    if (args == NULL) {
        die("malloc");
    }
    args->si_client = si_client;
    args->request = request;
    
    //****** Créer un nouveau thread pour traiter la requête ******
    pthread_t thread_id;
    if (pthread_create(&thread_id, NULL, process_request, (void*)args) != 0) {
        die("pthread_create");
    }

    // ****** Détacher le thread pour nettoyer automatiquement ses ressources à la fin de son exécution ******
    pthread_detach(thread_id);
}
    
    close(socket_fd);
    return 0;
}
