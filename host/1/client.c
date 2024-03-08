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
        unsigned short numero_bloc;
        unsigned short code_erreur;
    };
    char donnees[TAILLE_BUFFER - 4];
};

// Structure pour un paquet ACK TFTP
struct paquet_ack_tftp {
    unsigned short code_operation;
    unsigned short numero_bloc;
};

// Fonction pour arrêter le programme avec un message d'erreur
void arreter(char *s) {
    perror(s);
    exit(1);
}

// Fonction pour initialiser le socket
int initialiser_socket(struct sockaddr_in *si_serveur, char *ip_serveur, int port_serveur) {
    int socket_fd;
    // Création du socket
    if ((socket_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == -1) {
        arreter("socket");
    }

    // Configuration de l'adresse du serveur
    memset((char *)si_serveur, 0, sizeof(*si_serveur));
    si_serveur->sin_family = AF_INET;
    si_serveur->sin_port = htons(port_serveur);
    si_serveur->sin_addr.s_addr = inet_addr(ip_serveur);

    // Configuration du timeout pour les opérations de réception
    struct timeval tv;
    tv.tv_sec = TIMEOUT_SECONDES;
    tv.tv_usec = 0;
    if (setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        arreter("setsockopt");
    }

    return socket_fd;
}

// Fonction pour envoyer une requête de lecture (RRQ)
void envoyer_rrq(int socket_fd, struct sockaddr_in *si_serveur, char *nom_fichier) {
    char paquet_requete[TAILLE_BUFFER];
    int longueur_nom_fichier = strlen(nom_fichier);
    // Construction du paquet RRQ
    paquet_requete[0] = 0;
    paquet_requete[1] = OPCODE_RRQ;
    strcpy(paquet_requete + 2, nom_fichier);
    strcpy(paquet_requete + 2 + longueur_nom_fichier + 1, "octet");
    int taille_paquet = longueur_nom_fichier + strlen("octet") + 4;
    // Envoi du paquet au serveur
    if (sendto(socket_fd, paquet_requete, taille_paquet, 0, (struct sockaddr *)si_serveur, sizeof(*si_serveur)) == -1) {
        arreter("sendto()");
    }
}

// Fonction pour envoyer une requête d'écriture (WRQ)
void envoyer_wrq(int socket_fd, struct sockaddr_in *si_serveur, char *nom_fichier) {
    char paquet_requete[TAILLE_BUFFER];
    int longueur_nom_fichier = strlen(nom_fichier);
    // Construction du paquet WRQ
    paquet_requete[0] = 0;
    paquet_requete[1] = OPCODE_WRQ;
    strcpy(paquet_requete + 2, nom_fichier);
    strcpy(paquet_requete + 2 + longueur_nom_fichier + 1, "octet");
    int taille_paquet = longueur_nom_fichier + strlen("octet") + 4;
    // Envoi du paquet au serveur
    if (sendto(socket_fd, paquet_requete, taille_paquet, 0, (struct sockaddr *)si_serveur, sizeof(*si_serveur)) == -1) {
        arreter("sendto()");
    }
}

// Fonction pour recevoir un paquet ACK
int recevoir_ack(int socket_fd, struct sockaddr_in *si_serveur, struct paquet_ack_tftp *paquet_ack) {
    socklen_t longueur_serveur = sizeof(*si_serveur);
    // Réception du paquet ACK du serveur
    int octets_recus = recvfrom(socket_fd, paquet_ack, sizeof(*paquet_ack), 0, (struct sockaddr *)si_serveur, &longueur_serveur);
    if (octets_recus == -1) {
        perror("recvfrom a échoué");
        return -1;
    }
    return 0;
}

// Fonction pour envoyer des données au serveur
void envoyer_donnees(int socket_fd, struct sockaddr_in *si_serveur, char *nom_fichier) {
    struct paquet_ack_tftp paquet_ack;
    if (recevoir_ack(socket_fd, si_serveur, &paquet_ack) == -1) {
        arreter("recvfrom a échoué");
    }
    FILE *fichier = fopen(nom_fichier, "rb");
    if (fichier == NULL) {
        arreter("fopen");
    }
    struct paquet_tftp paquet_donnees;
    int numero_bloc = 1;
    int tentatives;
    int octets_lus;
    socklen_t longueur_serveur = sizeof(*si_serveur);

    do {
        // Lecture du fichier
        octets_lus = fread(paquet_donnees.donnees, 1, TAILLE_BUFFER - 4, fichier);
        if (octets_lus < 0) {
            arreter("fread");
        }
        // Construction du paquet de données
        paquet_donnees.code_operation = htons(OPCODE_DATA);
        paquet_donnees.numero_bloc = htons(numero_bloc);

        tentatives = 0;

        while (tentatives < 3) { // Tentatives limitées à 3 pour éviter une boucle infinie
            // Envoi du paquet de données au serveur
            if (sendto(socket_fd, &paquet_donnees, octets_lus + 4, 0, (struct sockaddr *)si_serveur, longueur_serveur) == -1) {
                arreter("sendto");
            }
            // Réception du paquet ACK du serveur
            if (recevoir_ack(socket_fd, si_serveur, &paquet_ack) == 0 && paquet_ack.code_operation == htons(OPCODE_ACK) && paquet_ack.numero_bloc == htons(numero_bloc)) {
                break; // ACK reçu correctement
            } else {
                tentatives++;
            }
        }

        if (tentatives == 3) {
            printf("Échec de l'envoi après 5 tentatives, abandon.\n");
            fclose(fichier);
            return;
        }

        numero_bloc++;
    } while (octets_lus == TAILLE_BUFFER - 4);

    fclose(fichier);
}

// Fonction pour recevoir des données du serveur
void recevoir_donnees(int socket_fd, struct sockaddr_in *si_serveur, char *nom_fichier) {
    FILE *fichier = fopen(nom_fichier, "wb");
    if (fichier == NULL) {
        arreter("fopen");
    }

    struct paquet_tftp paquet_donnees;
    int numero_bloc = 1;
    socklen_t longueur_serveur = sizeof(*si_serveur);
    int octets_recus;

    do {
        // Réception du paquet de données du serveur
        octets_recus = recvfrom(socket_fd, &paquet_donnees, TAILLE_BUFFER, 0, (struct sockaddr *)si_serveur, &longueur_serveur);
        if (octets_recus == -1) {
            if (errno == EWOULDBLOCK) {
                printf("Aucune réponse du serveur, nouvelle tentative...\n");
            } else {
                arreter("recvfrom()");
            }
        } else {
            // Vérification du code opération
            unsigned short code_operation = ntohs(paquet_donnees.code_operation);
            if (code_operation == OPCODE_ERROR) {
                printf("Le serveur a renvoyé une erreur : %s\n", paquet_donnees.donnees);
                fclose(fichier);
                close(socket_fd);
                exit(1);
            } else if (code_operation == OPCODE_ACK) {
                // ACK reçu du serveur
            } else if (code_operation != OPCODE_DATA) {
                printf("Réponse inattendue du serveur.\n");
                fclose(fichier);
                close(socket_fd);
                exit(1);
            }

            // Vérification du numéro de bloc
            unsigned short numero_bloc_recu = ntohs(paquet_donnees.numero_bloc);
            if (numero_bloc_recu != numero_bloc) {
                printf("Numéro de bloc inattendu : %d, attendu : %d\n", numero_bloc_recu, numero_bloc);
                fclose(fichier);
                close(socket_fd);
                exit(1);
            }

            // Écriture des données dans le fichier
            fwrite(paquet_donnees.donnees, 1, octets_recus - 4, fichier);

            // Envoi d'un acquittement (ACK) au serveur
            struct paquet_ack_tftp paquet_ack;
            paquet_ack.code_operation = htons(OPCODE_ACK);
            paquet_ack.numero_bloc = htons(numero_bloc);
            if (sendto(socket_fd, &paquet_ack, 4, 0, (struct sockaddr *)si_serveur, sizeof(*si_serveur)) == -1) {
                arreter("sendto()");
            }

            numero_bloc++;
        }
    } while (octets_recus == TAILLE_BUFFER);

    fclose(fichier);
}

int main(int argc, char *argv[]) {
    // Vérification du nombre d'arguments et de la commande
    if (argc != 5 || (strcmp(argv[1], "get") != 0 && strcmp(argv[1], "put") != 0)) {
        printf("Usage: %s <get/put> <ip_serveur> <port_serveur> <nom_fichier>\n", argv[0]);
        exit(1);
    }

    char *operation = argv[1];
    char *ip_serveur = argv[2];
    int port_serveur = atoi(argv[3]);
    char *nom_fichier = argv[4];

    // Initialisation du socket
    struct sockaddr_in si_serveur;
    int socket_fd = initialiser_socket(&si_serveur, ip_serveur, port_serveur);

    // Traitement en fonction de la commande (GET ou PUT)
    if (strcmp(operation, "get") == 0) {
        // Envoi de la requête GET
        envoyer_rrq(socket_fd, &si_serveur, nom_fichier);
        //sleep(5);
        // Réception des données du serveur
        recevoir_donnees(socket_fd, &si_serveur, nom_fichier);
        printf("Le fichier '%s' a été téléchargé avec succès.\n", nom_fichier);
    } else if (strcmp(operation, "put") == 0) {
        // Envoi de la requête PUT
        envoyer_wrq(socket_fd, &si_serveur, nom_fichier);
        //sleep(5);
        // Envoi des données au serveur
        envoyer_donnees(socket_fd, &si_serveur, nom_fichier);
        printf("Le fichier '%s' a été envoyé avec succès.\n", nom_fichier);
    }

    // Fermeture du socket
    close(socket_fd);

    return 0;
}
