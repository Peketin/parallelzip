#define _GNU_SOURCE

#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <sys/types.h>
#include <unistd.h>

#define TIETUEEN_KOKO (sizeof(int) + sizeof(unsigned char))
#define TIETUEITA_PALASSA 65536

/* Muistiin mapatun pakatun tiedoston tiedot. */
typedef struct {
    unsigned char *data;
    size_t koko;
} MapattuTiedosto;

/* Yksi työpala sisältää joukon peräkkäisiä pakattuja RLE-tietueita. */
typedef struct {
    int tiedosto_indeksi;
    size_t ensimmainen_tietue;
    size_t tietueiden_maara;
} TyoPala;

/* Yhden työpalan purettu tulos. */
typedef struct {
    unsigned char *data;
    size_t koko;
} PurettuTulos;

/* Säikeiden jakama työtila. */
typedef struct {
    MapattuTiedosto *tiedostot;
    TyoPala *tyot;
    PurettuTulos *tulokset;
    size_t tyojen_maara;
    size_t seuraava_tyo;
    pthread_mutex_t lukko;
} JaettuTila;

static void vapauta_tiedostot(MapattuTiedosto *tiedostot, int maara) {
    for (int i = 0; i < maara; i++) {
        if (tiedostot[i].data != NULL && tiedostot[i].koko > 0) {
            munmap(tiedostot[i].data, tiedostot[i].koko);
        }
    }
}

static int lue_maara(const unsigned char *osoite) {
    int maara;
    memcpy(&maara, osoite, sizeof(int));
    return maara;
}

static int pura_pala(const unsigned char *data, size_t tietueiden_maara, PurettuTulos *tulos) {
    size_t purettu_koko = 0;

    /* Ensimmäinen kierros laskee, kuinka paljon muistia tarvitaan. */
    for (size_t i = 0; i < tietueiden_maara; i++) {
        const unsigned char *tietue = data + i * TIETUEEN_KOKO;
        int maara = lue_maara(tietue);

        if (maara < 0 || SIZE_MAX - purettu_koko < (size_t)maara) {
            return -1;
        }

        purettu_koko += (size_t)maara;
    }

    tulos->koko = purettu_koko;
    if (purettu_koko == 0) {
        tulos->data = NULL;
        return 0;
    }

    tulos->data = malloc(purettu_koko);
    if (tulos->data == NULL) {
        return -1;
    }

    /* Toinen kierros täyttää tulospuskurin. */
    size_t sijainti = 0;
    for (size_t i = 0; i < tietueiden_maara; i++) {
        const unsigned char *tietue = data + i * TIETUEEN_KOKO;
        int maara = lue_maara(tietue);
        unsigned char merkki = *(tietue + sizeof(int));

        memset(tulos->data + sijainti, merkki, (size_t)maara);
        sijainti += (size_t)maara;
    }

    return 0;
}

static void *purkaja_saie(void *argumentti) {
    JaettuTila *tila = argumentti;

    while (1) {
        size_t tyon_indeksi;

        pthread_mutex_lock(&tila->lukko);
        if (tila->seuraava_tyo >= tila->tyojen_maara) {
            pthread_mutex_unlock(&tila->lukko);
            break;
        }
        tyon_indeksi = tila->seuraava_tyo;
        tila->seuraava_tyo++;
        pthread_mutex_unlock(&tila->lukko);

        TyoPala *tyo = &tila->tyot[tyon_indeksi];
        MapattuTiedosto *tiedosto = &tila->tiedostot[tyo->tiedosto_indeksi];
        const unsigned char *alku = tiedosto->data + tyo->ensimmainen_tietue * TIETUEEN_KOKO;

        if (pura_pala(alku, tyo->tietueiden_maara, &tila->tulokset[tyon_indeksi]) != 0) {
            fprintf(stderr, "punzip: invalid compressed data or memory allocation failed\n");
            exit(1);
        }
    }

    return NULL;
}

static int mapita_tiedosto(const char *polku, MapattuTiedosto *kohde) {
    int tiedostokuvaaja = open(polku, O_RDONLY);

    if (tiedostokuvaaja < 0) {
        fprintf(stderr, "punzip: cannot open file\n");
        return -1;
    }

    struct stat tiedot;
    if (fstat(tiedostokuvaaja, &tiedot) != 0) {
        fprintf(stderr, "punzip: cannot stat file\n");
        close(tiedostokuvaaja);
        return -1;
    }

    if (tiedot.st_size == 0) {
        kohde->data = NULL;
        kohde->koko = 0;
        close(tiedostokuvaaja);
        return 0;
    }

    if (tiedot.st_size < 0 || (unsigned long long)tiedot.st_size > (unsigned long long)SIZE_MAX) {
        fprintf(stderr, "punzip: file too large\n");
        close(tiedostokuvaaja);
        return -1;
    }

    kohde->koko = (size_t)tiedot.st_size;
    if (kohde->koko % TIETUEEN_KOKO != 0) {
        fprintf(stderr, "punzip: invalid compressed file\n");
        close(tiedostokuvaaja);
        return -1;
    }

    kohde->data = mmap(NULL, kohde->koko, PROT_READ, MAP_PRIVATE, tiedostokuvaaja, 0);
    close(tiedostokuvaaja);

    if (kohde->data == MAP_FAILED) {
        kohde->data = NULL;
        kohde->koko = 0;
        fprintf(stderr, "punzip: mmap failed\n");
        return -1;
    }

    return 0;
}

static int laske_tyot(MapattuTiedosto *tiedostot, int tiedostojen_maara, size_t *tyojen_maara) {
    *tyojen_maara = 0;

    for (int i = 0; i < tiedostojen_maara; i++) {
        size_t tietueita = tiedostot[i].koko / TIETUEEN_KOKO;

        if (tietueita == 0) {
            continue;
        }

        size_t paloja = (tietueita + TIETUEITA_PALASSA - 1) / TIETUEITA_PALASSA;
        if (SIZE_MAX - *tyojen_maara < paloja) {
            return -1;
        }
        *tyojen_maara += paloja;
    }

    return 0;
}

static void tayta_tyot(MapattuTiedosto *tiedostot, int tiedostojen_maara, TyoPala *tyot) {
    size_t tyo_indeksi = 0;

    for (int i = 0; i < tiedostojen_maara; i++) {
        size_t tietueita = tiedostot[i].koko / TIETUEEN_KOKO;
        size_t sijainti = 0;

        while (sijainti < tietueita) {
            size_t jaljella = tietueita - sijainti;
            size_t maara = jaljella < TIETUEITA_PALASSA ? jaljella : TIETUEITA_PALASSA;

            tyot[tyo_indeksi].tiedosto_indeksi = i;
            tyot[tyo_indeksi].ensimmainen_tietue = sijainti;
            tyot[tyo_indeksi].tietueiden_maara = maara;

            tyo_indeksi++;
            sijainti += maara;
        }
    }
}

int main(int argumenttien_maara, char *argumentit[]) {
    if (argumenttien_maara < 2) {
        printf("punzip: file1 [file2 ...]\n");
        return 1;
    }

    int tiedostojen_maara = argumenttien_maara - 1;
    MapattuTiedosto *tiedostot = calloc((size_t)tiedostojen_maara, sizeof(MapattuTiedosto));

    if (tiedostot == NULL) {
        fprintf(stderr, "punzip: memory allocation failed\n");
        return 1;
    }

    for (int i = 0; i < tiedostojen_maara; i++) {
        if (mapita_tiedosto(argumentit[i + 1], &tiedostot[i]) != 0) {
            vapauta_tiedostot(tiedostot, tiedostojen_maara);
            free(tiedostot);
            return 1;
        }
    }

    size_t tyojen_maara;
    if (laske_tyot(tiedostot, tiedostojen_maara, &tyojen_maara) != 0) {
        fprintf(stderr, "punzip: too many work items\n");
        vapauta_tiedostot(tiedostot, tiedostojen_maara);
        free(tiedostot);
        return 1;
    }

    if (tyojen_maara == 0) {
        vapauta_tiedostot(tiedostot, tiedostojen_maara);
        free(tiedostot);
        return 0;
    }

    TyoPala *tyot = calloc(tyojen_maara, sizeof(TyoPala));
    PurettuTulos *tulokset = calloc(tyojen_maara, sizeof(PurettuTulos));

    if (tyot == NULL || tulokset == NULL) {
        fprintf(stderr, "punzip: memory allocation failed\n");
        free(tyot);
        free(tulokset);
        vapauta_tiedostot(tiedostot, tiedostojen_maara);
        free(tiedostot);
        return 1;
    }

    tayta_tyot(tiedostot, tiedostojen_maara, tyot);

    int saikeiden_maara = get_nprocs();
    if (saikeiden_maara < 1) {
        saikeiden_maara = 1;
    }
    if ((size_t)saikeiden_maara > tyojen_maara) {
        saikeiden_maara = (int)tyojen_maara;
    }

    pthread_t *saikeet = malloc((size_t)saikeiden_maara * sizeof(pthread_t));
    if (saikeet == NULL) {
        fprintf(stderr, "punzip: memory allocation failed\n");
        free(tyot);
        free(tulokset);
        vapauta_tiedostot(tiedostot, tiedostojen_maara);
        free(tiedostot);
        return 1;
    }

    JaettuTila tila;
    tila.tiedostot = tiedostot;
    tila.tyot = tyot;
    tila.tulokset = tulokset;
    tila.tyojen_maara = tyojen_maara;
    tila.seuraava_tyo = 0;
    pthread_mutex_init(&tila.lukko, NULL);

    for (int i = 0; i < saikeiden_maara; i++) {
        if (pthread_create(&saikeet[i], NULL, purkaja_saie, &tila) != 0) {
            fprintf(stderr, "punzip: pthread_create failed\n");
            return 1;
        }
    }

    for (int i = 0; i < saikeiden_maara; i++) {
        pthread_join(saikeet[i], NULL);
    }

    pthread_mutex_destroy(&tila.lukko);

    /* Kirjoitetaan puretut palat alkuperäisessä järjestyksessä. */
    for (size_t i = 0; i < tyojen_maara; i++) {
        if (tulokset[i].koko > 0) {
            fwrite(tulokset[i].data, 1, tulokset[i].koko, stdout);
        }
    }

    for (size_t i = 0; i < tyojen_maara; i++) {
        free(tulokset[i].data);
    }

    free(saikeet);
    free(tyot);
    free(tulokset);
    vapauta_tiedostot(tiedostot, tiedostojen_maara);
    free(tiedostot);

    return 0;
}

