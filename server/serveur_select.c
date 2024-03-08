#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/time.h>

#define TAILLE_PAQUET 516
#define TIMEOUT_SEC 5
#define MAX_CLIENTS 10

#define OPCODE_RRQ 1
#define OPCODE_WRQ 2
#define OPCODE_DATA 3
#define OPCODE_ACK 4
#define OPCODE_ERROR 5

// Fonction pour gérer les erreurs et quitter le programme
void erreur(const char *msg) {
    perror(msg);
    exit(1);
}

// Structure de la requête RRQ/WRQ
struct tftp_request {
    unsigned short opcode;
    char filename[TAILLE_PAQUET - 2];
};

// Structure du paquet DATA
struct tftp_data_packet {
    unsigned short opcode;
    unsigned short block_num;
    char data[TAILLE_PAQUET - 4];
};

// Structure du paquet ACK
struct tftp_ack_packet {
    unsigned short opcode;
    unsigned short block_num;
};

// Fonction pour initialiser le socket
int initialiser_socket(int *sockfd, struct sockaddr_in *addr_serveur, int port) {
    // Création du socket
    *sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (*sockfd < 0) {
        erreur("Erreur lors de la création du socket");
    }

    // Initialisation de l'adresse du serveur
    memset(addr_serveur, 0, sizeof(*addr_serveur));
    addr_serveur->sin_family = AF_INET;
    addr_serveur->sin_addr.s_addr = htons(INADDR_ANY);
    addr_serveur->sin_port = htons(port);

    // Lier le socket à l'adresse du serveur
    if (bind(*sockfd, (struct sockaddr *)addr_serveur, sizeof(*addr_serveur)) < 0) {
        erreur("Erreur lors du liage du socket");
    }

    return *sockfd;
}

// Fonction pour envoyer un paquet de données au client
void envoyer_paquet_donnees(int sockfd, struct sockaddr_in *addr_client, struct tftp_data_packet *data_packet, int data_size, const char *nom_fichier) {
    if (sendto(sockfd, data_packet, data_size, 0, (struct sockaddr *)addr_client, sizeof(struct sockaddr_in)) < 0) {
        erreur("Erreur lors de l'envoi du paquet de données");
    }
    if (data_packet->opcode == htons(OPCODE_DATA)) {
        printf("Envoi des données du fichier '%s' au client...\n", nom_fichier);
    } else if (data_packet->opcode == htons(OPCODE_ACK)) {
        printf("Réception de l'ACK du client pour le fichier '%s'\n", nom_fichier);
    }
}

// Fonction pour recevoir une demande d'écriture (WRQ) du client
int recevoir_wrq(int sockfd, struct sockaddr_in *addr_client, const char *nom_fichier, const char *mode) {
    printf("Requête d'écriture (WRQ) reçue pour le fichier '%s'\n", nom_fichier);
    char buffer[TAILLE_PAQUET];
    socklen_t longueur_client = sizeof(struct sockaddr_in);
    int numero_bloc = 0;
    FILE *fichier = fopen(nom_fichier, "w+b"); // Ouverture en mode écriture binaire
    if (fichier == NULL) {
        erreur("Erreur lors de l'ouverture du fichier pour l'écriture");
    }

    // Envoi de l'ACK pour WRQ
    struct tftp_ack_packet ack_packet;
    ack_packet.opcode = htons(OPCODE_ACK);
    ack_packet.block_num = htons(0);

    if (sendto(sockfd, &ack_packet, sizeof(ack_packet), 0, (struct sockaddr *)addr_client, longueur_client) < 0) {
        erreur("Erreur lors de l'envoi de l'ACK pour WRQ");
    }

    while (1) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(sockfd, &readfds);

        struct timeval timeout;
        timeout.tv_sec = TIMEOUT_SEC;
        timeout.tv_usec = 0;

        int ready = select(sockfd + 1, &readfds, NULL, NULL, &timeout);
        if (ready < 0) {
            erreur("Erreur lors de l'appel à select()");
        } else if (ready == 0) {
            printf("Timeout lors de l'attente du paquet de données du client.\n");
            continue;
        } else {
            if (FD_ISSET(sockfd, &readfds)) {
                int bytes_recus = recvfrom(sockfd, buffer, TAILLE_PAQUET, 0, (struct sockaddr *)addr_client, &longueur_client);
                if (bytes_recus < 0) {
                    erreur("Erreur de réception des données");
                }

                unsigned char opcode = buffer[1];
                if (opcode == OPCODE_DATA) {
                    // Réception du paquet de données
                    numero_bloc++;
                    fwrite(buffer + 4, 1, bytes_recus - 4, fichier); // Écriture des données dans le fichier

                    // Envoi de l'ACK
                    ack_packet.block_num = htons(numero_bloc);

                    if (sendto(sockfd, &ack_packet, sizeof(ack_packet), 0, (struct sockaddr *)addr_client, longueur_client) < 0) {
                        erreur("Erreur lors de l'envoi de l'ACK pour DATA");
                    }

                    if (bytes_recus < TAILLE_PAQUET) {
                        // Dernier paquet de données
                        break;
                    }
                } else if (opcode == OPCODE_ERROR) {
                    // Erreur reçue du client
                    fprintf(stderr, "Erreur du client: %s\n", buffer + 4);
                    fclose(fichier);
                    remove(nom_fichier); // Supprimer le fichier en cas d'erreur
                    return -1;
                }
            }
        }
    }

    fclose(fichier);
    printf("Fin de la réception du fichier du fichier '%s'\n", nom_fichier);
    return 0;
}

// Fonction pour recevoir une demande de lecture (RRQ) du client
int recevoir_rrq(int sockfd, struct sockaddr_in *addr_client, const char *nom_fichier, const char *mode) {
    printf("Requête de lecture (RRQ) reçue pour le fichier '%s'\n", nom_fichier);
    char buffer[TAILLE_PAQUET];
    socklen_t longueur_client = sizeof(struct sockaddr_in);
    FILE *fichier = fopen(nom_fichier, "rb"); // Ouverture en mode lecture binaire
    int numero_bloc = 1;
    if (fichier == NULL) {
        // Fichier introuvable, envoi du paquet d'erreur
        buffer[0] = 0;
        buffer[1] = OPCODE_ERROR;
        buffer[2] = 0;
        buffer[3] = 1; // Code d'erreur pour fichier non trouvé
        strcpy(buffer + 4, "Fichier non trouvé.");
        sendto(sockfd, buffer, strlen("Fichier non trouvé.") + 5, 0, (struct sockaddr *)addr_client, longueur_client);
        return -1;
    }

    // Envoi du premier paquet de données
    while (1) {
        struct tftp_data_packet data_packet;
        data_packet.opcode = htons(OPCODE_DATA);
        data_packet.block_num = htons(numero_bloc);

        int bytes_lus = fread(data_packet.data, 1, TAILLE_PAQUET - 4, fichier);
        if (bytes_lus < TAILLE_PAQUET - 4) {
            // Dernier paquet de données
            sendto(sockfd, &data_packet, bytes_lus + 4, 0, (struct sockaddr *)addr_client, longueur_client);
            break;
        } else {
            sendto(sockfd, &data_packet, TAILLE_PAQUET, 0, (struct sockaddr *)addr_client, longueur_client);
        }

        // Attendre l'ACK du client avec timeout
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(sockfd, &readfds);

        struct timeval timeout;
        timeout.tv_sec = TIMEOUT_SEC;
        timeout.tv_usec = 0;

        int ready = select(sockfd + 1, &readfds, NULL, NULL, &timeout);
        if (ready < 0) {
            erreur("Erreur lors de l'appel à select()");
        } else if (ready == 0) {
            printf("Timeout lors de l'attente de l'ACK du client.\n");
            continue;
        } else {
            if (FD_ISSET(sockfd, &readfds)) {
                int bytes_recus = recvfrom(sockfd, buffer, TAILLE_PAQUET, 0, (struct sockaddr *)addr_client, &longueur_client);
                if (bytes_recus < 0) {
                    erreur("Erreur de réception des données");
                }

                if (buffer[1] == OPCODE_ACK) {
                    unsigned short ack_block_num = ntohs(*(unsigned short *)(buffer + 2));
                    if (ack_block_num == numero_bloc) {
                        break;
                    }
                }
            }
        }
        numero_bloc++;
    }

    fclose(fichier);
    printf("Fin de l'envoi du fichier '%s'\n", nom_fichier);
    return 0;
}

// Fonction principale
int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        exit(1);
    }

    int sockfd;
    struct sockaddr_in addr_serveur;

    // Initialisation du socket
    initialiser_socket(&sockfd, &addr_serveur, atoi(argv[1]));

    printf("Serveur TFTP démarré sur le port %s...\n", argv[1]);

    fd_set readfds;
    int max_sd = sockfd;

    while (1) {
        FD_ZERO(&readfds);
        FD_SET(sockfd, &readfds);

        // Attendre une activité sur un des sockets
        int activity = select(max_sd + 1, &readfds, NULL, NULL, NULL);
        if (activity < 0) {
            erreur("Erreur lors de l'appel à select()");
        }

        if (FD_ISSET(sockfd, &readfds)) {
            struct sockaddr_in addr_client;
            struct tftp_request buffer;
            socklen_t longueur_client = sizeof(struct sockaddr_in);

            // Recevoir la requête du client
            int bytes_recus = recvfrom(sockfd, &buffer, sizeof(struct tftp_request), 0, (struct sockaddr *)&addr_client, &longueur_client);
            if (bytes_recus < 0) {
                erreur("Erreur de réception des données");
            }

            // Traiter la requête du client
            if (buffer.opcode == htons(OPCODE_WRQ)) {
                // Requête d'écriture (WRQ) reçue
                recevoir_wrq(sockfd, &addr_client, buffer.filename, "Octet");
            } else if (buffer.opcode == htons(OPCODE_RRQ)) {
                // Requête de lecture (RRQ) reçue
                recevoir_rrq(sockfd, &addr_client, buffer.filename, "Octet");
            } else {
                printf("Requette inconnue\n");
            }
        }
    }

    close(sockfd);

    return 0;
}
