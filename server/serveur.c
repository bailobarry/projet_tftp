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
#define MAX_TENTATIVES 5

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
    addr_serveur->sin_addr.s_addr = INADDR_ANY;
    addr_serveur->sin_port = htons(port);

    // Lier le socket à l'adresse du serveur
    if (bind(*sockfd, (struct sockaddr *)addr_serveur, sizeof(*addr_serveur)) < 0) {
        erreur("Erreur lors du liage du socket");
    }

    return *sockfd;
}

// Fonction pour envoyer un paquet de données au client
void envoyer_paquet_donnees(int sockfd, struct sockaddr_in *addr_client, FILE *fichier, int numero_bloc) {
    char buffer[TAILLE_PAQUET];
    int bytes_lus = fread(buffer + 4, 1, TAILLE_PAQUET - 4, fichier); // Lecture des données du fichier dans le buffer

    // Construction du paquet DATA
    buffer[0] = 0;
    buffer[1] = OPCODE_DATA;
    buffer[2] = (numero_bloc >> 8) & 0xFF; // Numéro de bloc élevé
    buffer[3] = numero_bloc & 0xFF;        // Numéro de bloc bas

    // Vérification si c'est le dernier paquet de données
    if (bytes_lus < TAILLE_PAQUET - 4) {
        // Dernier paquet de données
        if (ferror(fichier)) {
            erreur("Erreur lors de la lecture du fichier");
        }
    }

    int len = bytes_lus + 4;
    // Envoi du paquet de données au client
    if (sendto(sockfd, buffer, len, 0, (struct sockaddr *)addr_client, sizeof(struct sockaddr_in)) < 0) {
        erreur("Erreur lors de l'envoi du paquet de données");
    }
}

// Fonction pour recevoir une demande d'écriture (WRQ) du client
int recevoir_wrq(int sockfd, struct sockaddr_in *addr_client, const char *nom_fichier, const char *mode) {
    char buffer[TAILLE_PAQUET];
    socklen_t longueur_client = sizeof(struct sockaddr_in);
    int numero_bloc = 0;

    FILE *fichier = fopen(nom_fichier, "r+b"); // Ouverture en mode lecture et écriture binaire
    if (fichier == NULL) {
        // Si le fichier n'existe pas, créez-le
        fichier = fopen(nom_fichier, "wb");
        if (fichier == NULL) {
            erreur("Erreur lors de la création du fichier pour l'écriture");
        }
    }

    // Envoi de l'ACK pour WRQ
    buffer[0] = 0;
    buffer[1] = OPCODE_ACK;
    buffer[2] = 0;
    buffer[3] = 0;

    if (sendto(sockfd, buffer, 4, 0, (struct sockaddr *)addr_client, longueur_client) < 0) {
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
            buffer[0] = 0;
            buffer[1] = OPCODE_ACK;
            buffer[2] = (numero_bloc >> 8) & 0xFF; // Numéro de bloc élevé
            buffer[3] = numero_bloc & 0xFF;        // Numéro de bloc bas

            if (sendto(sockfd, buffer, 4, 0, (struct sockaddr *)addr_client, longueur_client) < 0) {
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
    return 0;
}

// Fonction pour recevoir une demande de lecture (RRQ) du client
int recevoir_rrq(int sockfd, struct sockaddr_in *addr_client, const char *nom_fichier, const char *mode) {
    char buffer[TAILLE_PAQUET];
    socklen_t longueur_client = sizeof(struct sockaddr_in);
    FILE *fichier = fopen(nom_fichier, "rb"); // Ouverture en mode lecture binaire
    int numero_bloc = 1;

    if (fichier == NULL) {
        // Fichier introuvable, envoi du paquet d'erreur
        buffer[0] = 0;
        buffer[1] = OPCODE_ERROR;
        buffer[2] = 0;
        buffer[3] = 1; // Code d'erreur pour fichier introuvable
        strcpy(buffer + 4, "Fichier introuvable");

        if (sendto(sockfd, buffer, strlen("Fichier introuvable") + 5, 0, (struct sockaddr *)addr_client, longueur_client) < 0) {
            erreur("Erreur lors de l'envoi de l'ERROR pour RRQ");
        }

        return -1;
    }

    // Envoi du premier paquet de données
    envoyer_paquet_donnees(sockfd, addr_client, fichier, numero_bloc);

    while (1) {
        fd_set readfds;
        struct timeval timeout;
        FD_ZERO(&readfds);
        FD_SET(sockfd, &readfds);
        timeout.tv_sec = TIMEOUT_SEC;
        timeout.tv_usec = 0;

        int pret = select(sockfd + 1, &readfds, NULL, NULL, &timeout);
        if (pret < 0) {
            erreur("Erreur de sélection");
        } else if (pret == 0) {
            // Timeout atteint
            erreur("Timeout atteint lors de l'attente de l'ACK pour RRQ");
        }

        int bytes_recus = recvfrom(sockfd, buffer, TAILLE_PAQUET, 0, (struct sockaddr *)addr_client, &longueur_client);
        if (bytes_recus < 0) {
            erreur("Erreur de réception des données");
        }

        unsigned char opcode = buffer[1];
        if (opcode == OPCODE_ACK) {
            // Réception de l'ACK
            int numero_bloc_recu = (buffer[2] << 8) | buffer[3]; // Numéro de bloc reçu depuis l'ACK
            if (numero_bloc_recu == numero_bloc) {
                if (feof(fichier)) {
                    // Fin du fichier atteinte
                    break;
                } else {
                    // Lire et envoyer le prochain bloc de données
                    numero_bloc++;
                    envoyer_paquet_donnees(sockfd, addr_client, fichier, numero_bloc);
                }
            } else {
                fprintf(stderr, "Numéro de bloc incorrect dans l'ACK. Réessayez.\n");
            }
        } else {
            fprintf(stderr, "Opcode inattendu reçu : %d\n", opcode);
            break;
        }
    }

    fclose(fichier);
    return 0;
}

// Fonction principale
int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        exit(1);
    }

    int sockfd;
    struct sockaddr_in addr_serveur, addr_client;

    // Initialisation du socket
    initialiser_socket(&sockfd, &addr_serveur, atoi(argv[1]));

    printf("Serveur TFTP démarré sur le port %s...\n", argv[1]);

    while (1) {
        char buffer[TAILLE_PAQUET];
        socklen_t longueur_client = sizeof(struct sockaddr_in);

        int bytes_recus = recvfrom(sockfd, buffer, TAILLE_PAQUET, 0, (struct sockaddr *)&addr_client, &longueur_client);
        if (bytes_recus < 0) {
            erreur("Erreur de réception des données");
        }

        unsigned char opcode = buffer[1];
        if (opcode == OPCODE_WRQ) {
            // Requête d'écriture (WRQ) reçue
            char nom_fichier[256], mode[10];
            sscanf(buffer + 2, "%s %s", nom_fichier, mode);

            printf("Requête d'écriture (WRQ) reçue pour le fichier '%s' \n", nom_fichier);
            
            // Traitement de la requête WRQ
            if (recevoir_wrq(sockfd, &addr_client, nom_fichier, mode) == 0) {
                printf("Fichier '%s' écrit avec succès.\n", nom_fichier);
            } else {
                printf("Échec de l'écriture du fichier '%s'.\n", nom_fichier);
            }
        } else if (opcode == OPCODE_RRQ) {
            // Requête de lecture (RRQ) reçue
            char nom_fichier[256], mode[10];
            sscanf(buffer + 2, "%s %s", nom_fichier, mode);

            printf("Requête de lecture (RRQ) reçue pour le fichier '%s' \n", nom_fichier);
            
            // Traitement de la requête RRQ
            if (recevoir_rrq(sockfd, &addr_client, nom_fichier, mode) == 0) {
                printf("Envoi du fichier '%s' avec succès.\n", nom_fichier);
            } else {
                printf("Échec de l'envoi du fichier '%s'.\n", nom_fichier);
            }
        } else {
            fprintf(stderr, "Opcode non supporté : %d\n", opcode);
        }
    }

    close(sockfd);

    return 0;
}
