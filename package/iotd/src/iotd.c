/*
 * iotd - IoT gateway status publisher
 * v2 : connexion MQTT, publication periodique et reprise automatique
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <time.h>
#include <unistd.h>
#include <uci.h>
#include <mosquitto.h>

struct iotd_config {
    int  enabled;
    char broker[64];
    int  port;
    char topic[128];
    int  interval;
};

/* Lit une option UCI de type texte.
 * Retourne 0 si trouvee, -1 sinon (out n'est alors pas modifie). */
static int uci_read_string(struct uci_context *ctx, const char *path,
                           char *out, size_t out_len)
{
    char buf[128];          /* copie modifiable : uci_lookup_ptr decoupe la chaine */
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

/* Lit une option UCI et la convertit en entier.
 * Retourne 0 si OK, -1 si absente ou invalide (out n'est alors pas modifie). */
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

    /* Valeurs par defaut : utilisees si une option manque */
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

/* Secondes ecoulees depuis le demarrage, horloge monotone :
 * elle ne recule jamais, meme si NTP corrige l'heure. */
static time_t now_monotonic(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec;
}

/* Lit l'uptime du systeme dans /proc/uptime (premiere valeur, en secondes). */
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

/* Appelee par libmosquitto quand le broker repond au CONNECT (CONNACK). */
static void on_connect(struct mosquitto *mosq, void *userdata, int rc)
{
    (void)mosq;
    (void)userdata;

    if (rc == 0)
        fprintf(stderr, "iotd: connected to broker\n");
    else
        fprintf(stderr, "iotd: connection refused: %s\n",
                mosquitto_connack_string(rc));
}

/* Appelee quand la connexion est perdue ou fermee. */
static void on_disconnect(struct mosquitto *mosq, void *userdata, int rc)
{
    (void)mosq;
    (void)userdata;

    fprintf(stderr, "iotd: disconnected (rc=%d)\n", rc);
}

int main(void)
{
    struct iotd_config cfg;
    struct mosquitto *mosq;
    time_t next_publish = 0;
    int rc;

    if (load_config(&cfg) != 0)
        return EXIT_FAILURE;

    fprintf(stderr, "iotd: enabled=%d broker=%s port=%d topic=%s interval=%d\n",
            cfg.enabled, cfg.broker, cfg.port, cfg.topic, cfg.interval);

    if (!cfg.enabled) {
        fprintf(stderr, "iotd: disabled in configuration, exiting\n");
        return EXIT_SUCCESS;
    }

    mosquitto_lib_init();

    mosq = mosquitto_new("iotd", true, NULL);
    if (mosq == NULL) {
        fprintf(stderr, "iotd: mosquitto_new failed\n");
        mosquitto_lib_cleanup();
        return EXIT_FAILURE;
    }

    mosquitto_connect_callback_set(mosq, on_connect);
    mosquitto_disconnect_callback_set(mosq, on_disconnect);

    /* Premier essai. En cas d'echec, on ne quitte pas :
     * la boucle ci-dessous se charge de reessayer. */
    rc = mosquitto_connect(mosq, cfg.broker, cfg.port, 60);
    if (rc != MOSQ_ERR_SUCCESS)
        fprintf(stderr, "iotd: initial connect failed: %s\n",
                mosquitto_strerror(rc));

    for (;;) {
        /* Gere le reseau : envoi, reception, acquittements, keepalive.
         * Attend au plus 1000 ms qu'il se passe quelque chose. */
        rc = mosquitto_loop(mosq, 1000, 1);
        if (rc != MOSQ_ERR_SUCCESS) {
            fprintf(stderr, "iotd: %s, retrying in 5 s\n",
                    mosquitto_strerror(rc));
            sleep(5);
            mosquitto_reconnect(mosq);
            continue;
        }

        if (now_monotonic() >= next_publish) {
            char payload[64];
            double uptime = 0.0;

            read_uptime(&uptime);
            snprintf(payload, sizeof(payload), "uptime=%.0f", uptime);

            rc = mosquitto_publish(mosq, NULL, cfg.topic,
                                   (int)strlen(payload), payload, 1, false);
            if (rc != MOSQ_ERR_SUCCESS)
                fprintf(stderr, "iotd: publish failed: %s\n",
                        mosquitto_strerror(rc));

            next_publish = now_monotonic() + cfg.interval;
        }
    }

    /* Non atteint pour l'instant : la v4 ajoutera un arret propre sur SIGTERM. */
    mosquitto_destroy(mosq);
    mosquitto_lib_cleanup();
    return EXIT_SUCCESS;
}
