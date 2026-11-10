/*
 * iotd - IoT gateway status publisher
 * v1 : lecture de la configuration UCI
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uci.h>

struct iotd_config {
    int  enabled;
    char broker[64];
    int  port;
    char topic[128];
    int  interval;
};


/* Lit une option UCI de type texte.
 * path : chemin complet, ex. "iotd.main.broker"
 * Retourne 0 si trouvée, -1 sinon (out n'est alors pas modifié). */
static int uci_read_string(struct uci_context *ctx, const char *path,
                           char *out, size_t out_len)
{
    char buf[128];          /* copie modifiable : uci_lookup_ptr découpe la chaîne */
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
 * Retourne 0 si OK, -1 si l'option manque ou n'est pas un nombre valide
 * (out n'est alors pas modifié). */
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


/* TODO 1 : écrire uci_read_int(ctx, path, int *out)
 *   - utilise uci_read_string() pour récupérer le texte
 *   - convertis avec strtol() et vérifie que toute la chaîne est un nombre
 *     (indice : le 2e argument de strtol, char *end, doit pointer sur '\0')
 *   - retourne 0 si OK, -1 sinon
 */

static int load_config(struct iotd_config *cfg)
{
    struct uci_context *ctx;

    /* Valeurs par défaut : utilisées si une option manque */
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

int main(void)
{
    struct iotd_config cfg;

    if (load_config(&cfg) != 0)
        return EXIT_FAILURE;

    printf("iotd: enabled=%d broker=%s port=%d topic=%s interval=%d\n",
           cfg.enabled, cfg.broker, cfg.port, cfg.topic, cfg.interval);

    return EXIT_SUCCESS;
}
