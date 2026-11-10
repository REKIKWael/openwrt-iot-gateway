/*
 * iotd - IoT gateway status publisher
 * v3 : etat reseau en JSON, disponibilite (LWT) et arret propre
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdbool.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <uci.h>
#include <mosquitto.h>

struct iotd_config {
    int  enabled;
    char broker[64];
    int  port;
    char topic[128];
    int  interval;
};

/* Contexte de l'application, transmis aux callbacks libmosquitto */
struct iotd_app {
    struct iotd_config cfg;
    char avail_topic[160];      /* <topic>/availability : online / offline */
};

/* Mis a 0 par le gestionnaire de signal pour demander l'arret */
static volatile sig_atomic_t g_running = 1;

static void on_signal(int sig)
{
    (void)sig;
    g_running = 0;              /* le strict minimum dans un gestionnaire */
}

/* ------------------------------------------------------------------ */
/* Configuration UCI                                                   */
/* ------------------------------------------------------------------ */

static int uci_read_string(struct uci_context *ctx, const char *path,
                           char *out, size_t out_len)
{
    char buf[128];
    struct uci_ptr ptr;

    snprintf(buf, sizeof(buf), "%s", path);

    if (uci_lookup_ptr(ctx, &ptr, buf, true) != UCI_OK)
        return -1;
    if (!(ptr.flags & UCI_LOOKUP_COMPLETE) || ptr.o == NULL)
        return -1;
    if (ptr.o->type != UCI_TYPE_STRING)
        return -1;

    snprintf(out, out_len, "%s", ptr.o->v.string);
    return 0;
}

static int uci_read_int(struct uci_context *ctx, const char *path, int *out)
{
    char txt[32];
    char *end;
    long v;

    if (uci_read_string(ctx, path, txt, sizeof(txt)) != 0)
        return -1;

    v = strtol(txt, &end, 10);
    if (end == txt || *end != '\0')
        return -1;

    *out = (int)v;
    return 0;
}

static int load_config(struct iotd_config *cfg)
{
    struct uci_context *ctx;

    cfg->enabled  = 1;
    snprintf(cfg->broker, sizeof(cfg->broker), "%s", "127.0.0.1");
    cfg->port     = 1883;
    snprintf(cfg->topic, sizeof(cfg->topic), "%s", "gateway/status");
    cfg->interval = 10;

    ctx = uci_alloc_context();
    if (ctx == NULL) {
        fprintf(stderr, "iotd: uci_alloc_context failed\n");
        return -1;
    }

    uci_read_string(ctx, "iotd.main.broker", cfg->broker, sizeof(cfg->broker));
    uci_read_string(ctx, "iotd.main.topic", cfg->topic, sizeof(cfg->topic));
    uci_read_int(ctx, "iotd.main.port", &cfg->port);
    uci_read_int(ctx, "iotd.main.interval", &cfg->interval);
    uci_read_int(ctx, "iotd.main.enabled", &cfg->enabled);

    if (cfg->port < 1 || cfg->port > 65535) {
        fprintf(stderr, "iotd: invalid port %d\n", cfg->port);
        uci_free_context(ctx);
        return -1;
    }

    if (cfg->interval < 1) {
        fprintf(stderr, "iotd: invalid interval %d\n", cfg->interval);
        uci_free_context(ctx);
        return -1;
    }

    uci_free_context(ctx);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Etat du systeme                                                     */
/* ------------------------------------------------------------------ */

static time_t now_monotonic(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec;
}

static int read_uptime(double *uptime)
{
    FILE *f = fopen("/proc/uptime", "r");
    int n;

    if (f == NULL)
        return -1;
    n = fscanf(f, "%lf", uptime);
    fclose(f);
    return (n == 1) ? 0 : -1;
}

/* Ajoute du texte formate a la fin de buf, sans jamais deborder.
 * Retourne -1 si le buffer est trop petit. */
static int buf_append(char *buf, size_t len, size_t *off, const char *fmt, ...)
{
    va_list ap;
    int n;

    if (*off >= len)
        return -1;

    va_start(ap, fmt);
    n = vsnprintf(buf + *off, len - *off, fmt, ap);
    va_end(ap);

    if (n < 0 || (size_t)n >= len - *off)
        return -1;              /* resultat tronque */

    *off += (size_t)n;
    return 0;
}

/* Construit l'etat de la passerelle en JSON :
 * {"hostname":"OpenWrt","uptime":1234,"interfaces":[{"name":"eth1","ipv4":"10.0.2.15"}]}
 * Les noms d'hote et d'interface ne contiennent pas de caracteres
 * a echapper en JSON. */
static int build_status_json(char *buf, size_t len)
{
    struct ifaddrs *list, *ifa;
    char host[64];
    char addr[INET_ADDRSTRLEN];
    double uptime = 0.0;
    size_t off = 0;
    bool first = true;
    int rc = 0;

    if (gethostname(host, sizeof(host)) != 0)
        snprintf(host, sizeof(host), "%s", "unknown");
    host[sizeof(host) - 1] = '\0';

    read_uptime(&uptime);

    if (buf_append(buf, len, &off,
                   "{\"hostname\":\"%s\",\"uptime\":%.0f,\"interfaces\":[",
                   host, uptime) != 0)
        return -1;

    if (getifaddrs(&list) == 0) {
        for (ifa = list; ifa != NULL; ifa = ifa->ifa_next) {
            struct sockaddr_in *sin;

            if (ifa->ifa_addr == NULL || ifa->ifa_addr->sa_family != AF_INET)
                continue;
            if (ifa->ifa_flags & IFF_LOOPBACK)
                continue;

            sin = (struct sockaddr_in *)ifa->ifa_addr;
            inet_ntop(AF_INET, &sin->sin_addr, addr, sizeof(addr));

            if (buf_append(buf, len, &off, "%s{\"name\":\"%s\",\"ipv4\":\"%s\"}",
                           first ? "" : ",", ifa->ifa_name, addr) != 0) {
                rc = -1;
                break;
            }
            first = false;
        }
        freeifaddrs(list);      /* liberer la liste allouee par getifaddrs */
    }

    if (rc == 0)
        rc = buf_append(buf, len, &off, "]}");

    return rc;
}

/* ------------------------------------------------------------------ */
/* Callbacks MQTT                                                      */
/* ------------------------------------------------------------------ */

static void on_connect(struct mosquitto *mosq, void *userdata, int rc)
{
    struct iotd_app *app = userdata;

    if (rc != 0) {
        fprintf(stderr, "iotd: connection refused: %s\n",
                mosquitto_connack_string(rc));
        return;
    }

    fprintf(stderr, "iotd: connected to broker\n");

    /* Annonce la disponibilite. Message retenu : un nouvel abonne
     * connait immediatement l'etat de la passerelle. */
    mosquitto_publish(mosq, NULL, app->avail_topic,
                      (int)strlen("online"), "online", 1, true);
}

static void on_disconnect(struct mosquitto *mosq, void *userdata, int rc)
{
    (void)mosq;
    (void)userdata;

    fprintf(stderr, "iotd: disconnected (rc=%d)\n", rc);
}

/* ------------------------------------------------------------------ */
/* Programme principal                                                 */
/* ------------------------------------------------------------------ */

int main(void)
{
    struct iotd_app app;
    struct mosquitto *mosq;
    struct sigaction sa;
    time_t next_publish = 0;
    int rc;

    memset(&app, 0, sizeof(app));

    if (load_config(&app.cfg) != 0)
        return EXIT_FAILURE;

    snprintf(app.avail_topic, sizeof(app.avail_topic), "%s/availability",
             app.cfg.topic);

    fprintf(stderr, "iotd: enabled=%d broker=%s port=%d topic=%s interval=%d\n",
            app.cfg.enabled, app.cfg.broker, app.cfg.port, app.cfg.topic,
            app.cfg.interval);

    if (!app.cfg.enabled) {
        fprintf(stderr, "iotd: disabled in configuration, exiting\n");
        return EXIT_SUCCESS;
    }

    /* Arret propre sur SIGTERM (procd, kill) et SIGINT (Ctrl+C) */
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    mosquitto_lib_init();

    /* &app est transmis aux callbacks (parametre userdata) */
    mosq = mosquitto_new("iotd", true, &app);
    if (mosq == NULL) {
        fprintf(stderr, "iotd: mosquitto_new failed\n");
        mosquitto_lib_cleanup();
        return EXIT_FAILURE;
    }

    mosquitto_connect_callback_set(mosq, on_connect);
    mosquitto_disconnect_callback_set(mosq, on_disconnect);

    /* Testament : publie par le broker si la passerelle disparait
     * sans se deconnecter proprement. A definir AVANT la connexion. */
    mosquitto_will_set(mosq, app.avail_topic,
                       (int)strlen("offline"), "offline", 1, true);

    rc = mosquitto_connect(mosq, app.cfg.broker, app.cfg.port, 60);
    if (rc != MOSQ_ERR_SUCCESS)
        fprintf(stderr, "iotd: initial connect failed: %s\n",
                mosquitto_strerror(rc));

    while (g_running) {
        rc = mosquitto_loop(mosq, 1000, 1);
        if (!g_running)
            break;

        if (rc != MOSQ_ERR_SUCCESS) {
            fprintf(stderr, "iotd: %s, retrying in 5 s\n",
                    mosquitto_strerror(rc));
            sleep(5);           /* interrompu immediatement par un signal */
            if (!g_running)
                break;

            rc = mosquitto_reconnect(mosq);
            if (rc != MOSQ_ERR_SUCCESS)     /* journaliser la vraie cause */
                fprintf(stderr, "iotd: reconnect failed: %s\n",
                        mosquitto_strerror(rc));
            continue;
        }

        if (now_monotonic() >= next_publish) {
            char payload[1024];

            if (build_status_json(payload, sizeof(payload)) == 0) {
                rc = mosquitto_publish(mosq, NULL, app.cfg.topic,
                                       (int)strlen(payload), payload, 1, false);
                if (rc != MOSQ_ERR_SUCCESS)
                    fprintf(stderr, "iotd: publish failed: %s\n",
                            mosquitto_strerror(rc));
            } else {
                fprintf(stderr, "iotd: status too large for buffer\n");
            }

            next_publish = now_monotonic() + app.cfg.interval;
        }
    }

    /* Arret propre : on annonce "offline" soi-meme, car le broker
     * n'envoie pas le testament lors d'une deconnexion normale. */
    fprintf(stderr, "iotd: stopping\n");
    mosquitto_publish(mosq, NULL, app.avail_topic,
                      (int)strlen("offline"), "offline", 1, true);
    mosquitto_loop(mosq, 500, 1);   /* laisser le temps d'envoyer le message */
    mosquitto_disconnect(mosq);
    mosquitto_destroy(mosq);
    mosquitto_lib_cleanup();
    return EXIT_SUCCESS;
}
