/*
 * wifi_speedtest.c
 * ----------------
 * Diagnostic WiFi combinant :
 *   1) Test LAN local  : ping ICMP vers la gateway (routeur WiFi)
 *   2) Test Internet    : téléchargement HTTP pour mesurer le débit réel
 *
 * Compilation :
 *   gcc -O2 -o wifi_speedtest wifi_speedtest.c
 *
 * Exécution (le ping ICMP nécessite les privilèges root pour les sockets RAW) :
 *   sudo ./wifi_speedtest
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <math.h>

#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>
#include <arpa/inet.h>
#include <netdb.h>

/* ---------- Paramètres modifiables ---------- */
#define PING_COUNT          6
#define PING_TIMEOUT_SEC    2
#define ICMP_PAYLOAD_SIZE   32

#define DL_HOST             "ipv4.download.thinkbroadband.com"
#define DL_PATH             "/10MB.zip"
#define DL_PORT             "80"

/* Seuils de diagnostic (ajustables) */
#define PING_LOW_MS         15.0     /* en dessous : latence "bas & stable" */
#define PING_HIGH_MS        50.0     /* au dessus  : latence "élevée" */
#define LOSS_THRESHOLD_PCT  2.0      /* perte de paquets jugée significative */
#define SPEED_GOOD_MBPS     20.0
#define SPEED_BAD_MBPS      5.0

/* ---------------------------------------------------------------- */
/*  Utilitaires                                                      */
/* ---------------------------------------------------------------- */

static unsigned short checksum(void *b, int len) {
    unsigned short *buf = b;
    unsigned int sum = 0;
    unsigned short result;

    for (sum = 0; len > 1; len -= 2)
        sum += *buf++;
    if (len == 1)
        sum += *(unsigned char *)buf;

    sum = (sum >> 16) + (sum & 0xFFFF);
    sum += (sum >> 16);
    result = ~sum;
    return result;
}

static double timespec_diff_ms(struct timespec *start, struct timespec *end) {
    double s  = (double)(end->tv_sec - start->tv_sec) * 1000.0;
    double ns = (double)(end->tv_nsec - start->tv_nsec) / 1e6;
    return s + ns;
}

/* Récupère l'IP de la passerelle par défaut (routeur WiFi) via /proc/net/route */
static int get_default_gateway(char *out_ip, size_t out_len) {
    FILE *f = fopen("/proc/net/route", "r");
    if (!f) return -1;

    char line[256];
    /* Sauter l'entête */
    if (!fgets(line, sizeof(line), f)) { fclose(f); return -1; }

    while (fgets(line, sizeof(line), f)) {
        char iface[64];
        unsigned long dest, gateway;
        int flags, refcnt, use, metric, mask, mtu, window, irtt;

        int n = sscanf(line, "%63s %lx %lx %d %d %d %d %x %d %d %d",
                        iface, &dest, &gateway, &flags, &refcnt, &use,
                        &metric, (unsigned int *)&mask, &mtu, &window, &irtt);
        if (n < 3) continue;

        if (dest == 0) { /* route par défaut (destination 0.0.0.0) */
            struct in_addr addr;
            addr.s_addr = gateway;
            strncpy(out_ip, inet_ntoa(addr), out_len - 1);
            out_ip[out_len - 1] = '\0';
            fclose(f);
            return 0;
        }
    }
    fclose(f);
    return -1;
}

/* ---------------------------------------------------------------- */
/*  1) Test LAN : ping ICMP vers la gateway                          */
/* ---------------------------------------------------------------- */

typedef struct {
    double avg_rtt_ms;
    double jitter_ms;
    double loss_pct;
    int    success; /* 0 si erreur (ex: pas de permission root) */
} ping_result_t;

static ping_result_t ping_host(const char *ip_str, int count) {
    ping_result_t res = {0};
    struct sockaddr_in addr;
    int sock = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (sock < 0) {
        fprintf(stderr, "[!] Impossible d'ouvrir la socket RAW (%s).\n"
                        "    Lance le programme avec 'sudo' pour le test de ping.\n",
                strerror(errno));
        res.success = 0;
        return res;
    }

    struct timeval tv = { .tv_sec = PING_TIMEOUT_SEC, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    inet_pton(AF_INET, ip_str, &addr.sin_addr);

    double rtts[64];
    int received = 0;

    for (int i = 0; i < count; i++) {
        char packet[sizeof(struct icmphdr) + ICMP_PAYLOAD_SIZE];
        memset(packet, 0, sizeof(packet));

        struct icmphdr *icmp = (struct icmphdr *)packet;
        icmp->type = ICMP_ECHO;
        icmp->code = 0;
        icmp->un.echo.id = getpid() & 0xFFFF;
        icmp->un.echo.sequence = i;
        icmp->checksum = 0;
        icmp->checksum = checksum(packet, sizeof(packet));

        struct timespec t_start, t_end;
        clock_gettime(CLOCK_MONOTONIC, &t_start);

        if (sendto(sock, packet, sizeof(packet), 0,
                   (struct sockaddr *)&addr, sizeof(addr)) <= 0) {
            continue;
        }

        char buf[512];
        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);
        ssize_t n = recvfrom(sock, buf, sizeof(buf), 0,
                             (struct sockaddr *)&from, &fromlen);

        clock_gettime(CLOCK_MONOTONIC, &t_end);

        if (n > 0) {
            double rtt = timespec_diff_ms(&t_start, &t_end);
            rtts[received++] = rtt;
        }
        usleep(200000); /* 200 ms entre chaque ping */
    }

    close(sock);

    if (received == 0) {
        res.success = 0;
        res.loss_pct = 100.0;
        return res;
    }

    double sum = 0;
    for (int i = 0; i < received; i++) sum += rtts[i];
    double avg = sum / received;

    double var_sum = 0;
    for (int i = 0; i < received; i++) var_sum += (rtts[i] - avg) * (rtts[i] - avg);
    double jitter = (received > 1) ? sqrt(var_sum / (received - 1)) : 0.0;

    res.avg_rtt_ms = avg;
    res.jitter_ms  = jitter;
    res.loss_pct   = 100.0 * (double)(count - received) / (double)count;
    res.success    = 1;
    return res;
}

/* ---------------------------------------------------------------- */
/*  2) Test Internet : téléchargement HTTP                           */
/* ---------------------------------------------------------------- */

typedef struct {
    double mbps;
    int    success;
} speed_result_t;

static speed_result_t test_download_speed(const char *host, const char *port, const char *path) {
    speed_result_t res = {0, 0};
    struct addrinfo hints, *server_info, *p;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host, port, &hints, &server_info) != 0) {
        fprintf(stderr, "[!] Résolution DNS échouée pour %s\n", host);
        return res;
    }

    int sock = -1;
    for (p = server_info; p != NULL; p = p->ai_next) {
        sock = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (sock < 0) continue;
        if (connect(sock, p->ai_addr, p->ai_addrlen) == 0) break;
        close(sock);
        sock = -1;
    }
    freeaddrinfo(server_info);

    if (sock < 0) {
        fprintf(stderr, "[!] Connexion impossible à %s\n", host);
        return res;
    }

    char request[512];
    snprintf(request, sizeof(request),
             "GET %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\nUser-Agent: wifi_speedtest/1.0\r\n\r\n",
             path, host);

    if (send(sock, request, strlen(request), 0) < 0) {
        close(sock);
        return res;
    }

    char buf[65536];
    long total_bytes = 0;
    ssize_t n;

    struct timespec t_start, t_end;
    clock_gettime(CLOCK_MONOTONIC, &t_start);

    while ((n = recv(sock, buf, sizeof(buf), 0)) > 0) {
        total_bytes += n;
    }

    clock_gettime(CLOCK_MONOTONIC, &t_end);
    close(sock);

    double elapsed_sec = timespec_diff_ms(&t_start, &t_end) / 1000.0;
    if (elapsed_sec <= 0 || total_bytes <= 0) {
        return res;
    }

    /* On enlève approximativement les en-têtes HTTP (~300 octets) du calcul */
    long payload_bytes = total_bytes > 300 ? total_bytes - 300 : total_bytes;
    double mbps = ((double)payload_bytes * 8.0) / (elapsed_sec * 1000000.0);

    res.mbps = mbps;
    res.success = 1;
    return res;
}

/* ---------------------------------------------------------------- */
/*  3) Diagnostic combiné                                             */
/* ---------------------------------------------------------------- */

static void print_diagnosis(ping_result_t ping, speed_result_t dl) {
    printf("\n==================== DIAGNOSTIC ====================\n");

    int lan_ok = ping.success && ping.avg_rtt_ms < PING_LOW_MS && ping.loss_pct < LOSS_THRESHOLD_PCT;
    int lan_bad = !ping.success || ping.avg_rtt_ms > PING_HIGH_MS || ping.loss_pct >= LOSS_THRESHOLD_PCT;
    int net_ok = dl.success && dl.mbps >= SPEED_GOOD_MBPS;
    int net_bad = !dl.success || dl.mbps < SPEED_BAD_MBPS;

    if (lan_ok && net_ok) {
        printf(" [OK] Tout va bien : signal WiFi stable et débit Internet correct.\n");
    } else if (lan_ok && net_bad) {
        printf(" [!] Le WiFi local est bon (ping stable), mais le débit Internet est faible.\n");
        printf("     -> Problème probable : côté FAI (fournisseur d'accès) ou congestion en amont du routeur.\n");
    } else if (lan_bad && net_bad) {
        printf(" [!!] Ping instable/élevé ET débit Internet faible.\n");
        printf("     -> Problème probable : signal WiFi lui-même (distance au routeur, interférences,\n");
        printf("        murs épais, trop d'appareils connectés, canal WiFi encombré).\n");
    } else if (lan_bad && net_ok) {
        printf(" [!] Le ping vers le routeur est instable, mais le débit Internet reste correct.\n");
        printf("     -> Problème probable : interférences ponctuelles ou instabilité du signal WiFi,\n");
        printf("        sans impact majeur sur le débit pour l'instant (peut se dégrader).\n");
    } else {
        printf(" [?] Résultats mitigés, impossible de conclure clairement.\n");
        printf("     -> Relance le test, ou vérifie les valeurs manuellement ci-dessus.\n");
    }

    if (ping.success && ping.jitter_ms > 10.0) {
        printf(" [i] Jitter élevé (%.1f ms) : signe d'interférences WiFi ou de congestion réseau.\n",
               ping.jitter_ms);
    }
    printf("=====================================================\n");
}

/* ---------------------------------------------------------------- */
/*  main                                                              */
/* ---------------------------------------------------------------- */

int main(void) {
    printf("############################################\n");
    printf("#      WiFi Speed & Diagnostic Tester       #\n");
    printf("############################################\n\n");

    char gateway_ip[64];
    if (get_default_gateway(gateway_ip, sizeof(gateway_ip)) != 0) {
        fprintf(stderr, "[!] Impossible de déterminer la passerelle par défaut.\n");
        strcpy(gateway_ip, "");
    } else {
        printf(">> Passerelle WiFi détectée : %s\n\n", gateway_ip);
    }

    /* ---- Test 1 : LAN (ping gateway) ---- */
    printf("---- Test 1/2 : Latence & stabilité WiFi (ping routeur) ----\n");
    ping_result_t ping = {0};
    if (strlen(gateway_ip) > 0) {
        ping = ping_host(gateway_ip, PING_COUNT);
        if (ping.success) {
            printf("  RTT moyen   : %.2f ms\n", ping.avg_rtt_ms);
            printf("  Jitter      : %.2f ms\n", ping.jitter_ms);
            printf("  Perte paquets : %.1f %%\n", ping.loss_pct);
        } else {
            printf("  Echec du ping (permissions root nécessaires, ou routeur ne répond pas).\n");
        }
    }

    /* ---- Test 2 : Internet (débit) ---- */
    printf("\n---- Test 2/2 : Débit Internet (téléchargement) ----\n");
    printf("  Connexion à %s ...\n", DL_HOST);
    speed_result_t dl = test_download_speed(DL_HOST, DL_PORT, DL_PATH);
    if (dl.success) {
        printf("  Débit mesuré : %.2f Mbps\n", dl.mbps);
    } else {
        printf("  Echec du test de débit (vérifie ta connexion Internet).\n");
    }

    /* ---- Diagnostic ---- */
    print_diagnosis(ping, dl);

    return 0;
}
