#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <errno.h>
#include <sys/time.h>

#define TAILLE_PAQUET 516
#define TIMEOUT_SEC 10
#define MAX_TENTATIVES 5

#define OPCODE_RRQ 1
#define OPCODE_WRQ 2
#define OPCODE_DATA 3
#define OPCODE_ACK 4
#define OPCODE_ERROR 5

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

// Fonction pour gérer les erreurs et quitter le programme
void erreur(const char *msg) {
    perror(msg);
    exit(1);
}

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
    addr_serveur->sin_addr.s_addr = htonl(INADDR_ANY);
    addr_serveur->sin_port = htons(port);

    // Lier le socket à l'adresse du serveur
    if (bind(*sockfd, (struct sockaddr *)addr_serveur, sizeof(*addr_serveur)) < 0) {
        erreur("Erreur lors du liage du socket");
    }

    return *sockfd;
}

// Fonction pour envoyer un paquet de données au client avec timeout
void envoyer_paquet_donnees(int sockfd, struct sockaddr_in *addr_client, struct tftp_data_packet *data_packet, int data_size, const char *nom_fichier) {
    int tentatives = 0;
    while (tentatives < MAX_TENTATIVES) {
        if (sendto(sockfd, data_packet, data_size, 0, (struct sockaddr *)addr_client, sizeof(struct sockaddr_in)) < 0) {
            perror("Erreur lors de l'envoi du paquet de données");
            tentatives++;
            sleep(TIMEOUT_SEC);
        } else {
            if (data_packet->opcode == htons(OPCODE_DATA)) {
                printf("Envoi des données du fichier '%s' au client...\n", nom_fichier);
            } else if (data_packet->opcode == htons(OPCODE_ACK)) {
                printf("Réception de l'ACK du client pour le fichier '%s'\n", nom_fichier);
            }
            break;
        }
    }
    if (tentatives == MAX_TENTATIVES) {
        fprintf(stderr, "Échec de l'envoi du paquet de données après %d tentatives. Le client semble indisponible.\n", MAX_TENTATIVES);
        exit(1);
    }
}

// Fonction pour recevoir une demande d'écriture (WRQ) du client avec timeout
int recevoir_wrq(int sockfd, struct sockaddr_in *addr_client, const char *nom_fichier, const char *mode) {
    printf("Requête d'écriture (WRQ) reçue pour le fichier '%s'\n", nom_fichier);
    char buffer[TAILLE_PAQUET];
    socklen_t longueur_client = sizeof(struct sockaddr_in);
    int numero_bloc = 0;
    FILE *fichier;
    fichier = fopen(nom_fichier, "r+b"); // Ouverture en mode lecture et écriture binaire
    if (fichier == NULL) {
        // Si le fichier n'existe pas, créez-le
        fichier = fopen(nom_fichier, "wb");
        if (fichier == NULL) {
            erreur("Erreur lors de la création du fichier pour l'écriture");
        }
    }

    // Envoi de l'ACK pour WRQ
    struct tftp_ack_packet ack_packet;
    ack_packet.opcode = htons(OPCODE_ACK);
    ack_packet.block_num = htons(0);

    if (sendto(sockfd, &ack_packet, sizeof(ack_packet), 0, (struct sockaddr *)addr_client, longueur_client) < 0) {
        erreur("Erreur lors de l'envoi de l'ACK pour WRQ");
    }

    while (1) {
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
    fclose(fichier);
    printf("Fin de la réception du fichier du fichier '%s'\n", nom_fichier);
    return 0;
}

// Fonction pour recevoir une demande de lecture (RRQ) du client avec timeout
int recevoir_rrq(int sockfd, struct sockaddr_in *addr_client, const char *nom_fichier, const char *mode) {
    printf("Requête de lecture (RRQ) reçue pour le fichier '%s'\n", nom_fichier);
    char buffer[TAILLE_PAQUET];
    FILE *fichier;
    fichier = fopen(nom_fichier, "rb"); // Ouverture en mode lecture binaire
    int numero_bloc = 1;
    if (fichier == NULL) {
        // Fichier introuvable, envoi du paquet d'erreur
        buffer[0] = 0;
        buffer[1] = OPCODE_ERROR;
        buffer[2] = 0;
        buffer[3] = 1; // Code d'erreur pour fichier non trouvé
        strcpy(buffer + 4, "Fichier non trouvé.");
        sendto(sockfd, buffer, strlen("Fichier non trouvé.") + 5, 0, (struct sockaddr *)addr_client, sizeof(struct sockaddr_in));
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
            sendto(sockfd, &data_packet, bytes_lus + 4, 0, (struct sockaddr *)addr_client, sizeof(struct sockaddr_in));
            break;
        } else {
            sendto(sockfd, &data_packet, TAILLE_PAQUET, 0, (struct sockaddr *)addr_client, sizeof(struct sockaddr_in));
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

    fd_set readfds; // Ensemble de descripteurs de fichier en lecture
    int max_fd = sockfd + 1; // Le plus grand descripteur de fichier

    while (1) {
        FD_ZERO(&readfds); // Effacer l'ensemble des descripteurs de fichier
        FD_SET(sockfd, &readfds); // Ajouter le socket principal à l'ensemble

        // Définir le timeout
        struct timeval timeout;
        timeout.tv_sec = TIMEOUT_SEC;
        timeout.tv_usec = 0;

        // Sélectionner les sockets prêts en lecture avec timeout
        int activite = select(max_fd, &readfds, NULL, NULL, &timeout);
        if (activite == 0) {
            // Timeout atteint
            printf("\n");
            printf("Timeout atteint. Aucune activité détectée.\n");
            printf("\n");
            continue;
        } else if (activite < 0) {
            // Erreur dans la fonction select
            erreur("Erreur dans la fonction select");
        }

        if (FD_ISSET(sockfd, &readfds)) {
            // Nouvelle connexion, accepter la connexion
            struct sockaddr_in addr_client;
            struct tftp_request buffer;
            socklen_t longueur_client = sizeof(struct sockaddr_in);

            // Recevoir la requête du client
            int bytes_recus = recvfrom(sockfd, &buffer, sizeof(struct tftp_request), 0, (struct sockaddr *)&addr_client, &longueur_client);
            if (bytes_recus < 0) {
                erreur("Erreur de réception des données");
                continue;
            }

            if (buffer.opcode == htons(OPCODE_WRQ)) {
                // Requête d'écriture (WRQ) reçue
                recevoir_wrq(sockfd, &addr_client, buffer.filename, "Octet");
            } else if (buffer.opcode == htons(OPCODE_RRQ)) {
                // Requête de lecture (RRQ) reçue
                recevoir_rrq(sockfd, &addr_client, buffer.filename, "Octet");
            } else {
                printf("\n");
            }
        }
    }


    close(sockfd);

    return 0;
}
