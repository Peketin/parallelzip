#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
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

#define PALAN_KOKO (1024 * 1024)

/* Yksi RLE-jakso: sama merkki toistuu 'maara' kertaa. */
typedef struct {
    int maara;
    unsigned char merkki;
} Jakso;

/* Muistiin mapatun tiedoston tiedot. */
typedef struct {
    unsigned char *data;
    size_t koko;
} MapattuTiedosto;

/* Yksi työpala, jonka säie pakkaa. */
typedef struct {
    int tiedosto_indeksi;
    size_t alku;
    size_t pituus;
} TyoPala;

/* Yhden työpalan pakattu tulos. */
typedef struct {
    Jakso *jaksot;
    size_t jaksojen_maara;
    size_t kapasiteetti;
} PakattuTulos;

/* Säikeiden jakama työtila. */
typedef struct {
    MapattuTiedosto *tiedostot;
    TyoPala *tyot;
    PakattuTulos *tulokset;
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

static int lisaa_jakso(PakattuTulos *tulos, int maara, unsigned char merkki) {
    if (tulos->jaksojen_maara == tulos->kapasiteetti) {
        size_t uusi_kapasiteetti = tulos->kapasiteetti == 0 ? 1024 : tulos->kapasiteetti * 2;
        Jakso *uudet_jaksot = realloc(tulos->jaksot, uusi_kapasiteetti * sizeof(Jakso));

        if (uudet_jaksot == NULL) {
            return -1;
        }

        tulos->jaksot = uudet_jaksot;
        tulos->kapasiteetti = uusi_kapasiteetti;
    }

    tulos->jaksot[tulos->jaksojen_maara].maara = maara;
    tulos->jaksot[tulos->jaksojen_maara].merkki = merkki;
    tulos->jaksojen_maara++;

    return 0;
}

static int pakkaa_pala(const unsigned char *data, size_t pituus, PakattuTulos *tulos) {
    if (pituus == 0) {
        return 0;
    }

    unsigned char edellinen = data[0];
    int maara = 1;

    for (size_t i = 1; i < pituus; i++) {
        if (data[i] == edellinen && maara < INT_MAX) {
            maara++;
        } else {
            if (lisaa_jakso(tulos, maara, edellinen) != 0) {
                return -1;
            }

            edellinen = data[i];
            maara = 1;
        }
    }

    if (lisaa_jakso(tulos, maara, edellinen) != 0) {
        return -1;
    }

    return 0;
}

static void *pakkaaja_saie(void *argumentti) {
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
        const unsigned char *alku = tiedosto->data + tyo->alku;

        if (pakkaa_pala(alku, tyo->pituus, &tila->tulokset[tyon_indeksi]) != 0) {
            fprintf(stderr, "pzip: memory allocation failed\n");
            exit(1);
        }
    }

    return NULL;
}

static void kirjoita_jakso(long long maara, unsigned char merkki) {
    while (maara > 0) {
        int kirjoitettava = maara > INT_MAX ? INT_MAX : (int)maara;
        fwrite(&kirjoitettava, sizeof(int), 1, stdout);
        fwrite(&merkki, sizeof(unsigned char), 1, stdout);
        maara -= kirjoitettava;
    }
}

static int mapita_tiedosto(const char *polku, MapattuTiedosto *kohde) {
    int tiedostokuvaaja = open(polku, O_RDONLY);

    if (tiedostokuvaaja < 0) {
        fprintf(stderr, "pzip: cannot open file\n");
        return -1;
    }

    struct stat tiedot;
    if (fstat(tiedostokuvaaja, &tiedot) != 0) {
        fprintf(stderr, "pzip: cannot stat file\n");
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
        fprintf(stderr, "pzip: file too large\n");
        close(tiedostokuvaaja);
        return -1;
    }

    kohde->koko = (size_t)tiedot.st_size;
    kohde->data = mmap(NULL, kohde->koko, PROT_READ, MAP_PRIVATE, tiedostokuvaaja, 0);
    close(tiedostokuvaaja);

    if (kohde->data == MAP_FAILED) {
        kohde->data = NULL;
        kohde->koko = 0;
        fprintf(stderr, "pzip: mmap failed\n");
        return -1;
    }

    return 0;
}

static int laske_tyot(MapattuTiedosto *tiedostot, int tiedostojen_maara, size_t *tyojen_maara) {
    *tyojen_maara = 0;

    for (int i = 0; i < tiedostojen_maara; i++) {
        if (tiedostot[i].koko == 0) {
            continue;
        }

        size_t paloja = (tiedostot[i].koko + PALAN_KOKO - 1) / PALAN_KOKO;
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
        size_t sijainti = 0;

        while (sijainti < tiedostot[i].koko) {
            size_t jaljella = tiedostot[i].koko - sijainti;
            size_t pituus = jaljella < PALAN_KOKO ? jaljella : PALAN_KOKO;

            tyot[tyo_indeksi].tiedosto_indeksi = i;
            tyot[tyo_indeksi].alku = sijainti;
            tyot[tyo_indeksi].pituus = pituus;

            tyo_indeksi++;
            sijainti += pituus;
        }
    }
}

int main(int argumenttien_maara, char *argumentit[]) {
    if (argumenttien_maara < 2) {
        printf("pzip: file1 [file2 ...]\n");
        return 1;
    }

    int tiedostojen_maara = argumenttien_maara - 1;
    MapattuTiedosto *tiedostot = calloc((size_t)tiedostojen_maara, sizeof(MapattuTiedosto));

    if (tiedostot == NULL) {
        fprintf(stderr, "pzip: memory allocation failed\n");
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
        fprintf(stderr, "pzip: too many work items\n");
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
    PakattuTulos *tulokset = calloc(tyojen_maara, sizeof(PakattuTulos));

    if (tyot == NULL || tulokset == NULL) {
        fprintf(stderr, "pzip: memory allocation failed\n");
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
        fprintf(stderr, "pzip: memory allocation failed\n");
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
        if (pthread_create(&saikeet[i], NULL, pakkaaja_saie, &tila) != 0) {
            fprintf(stderr, "pzip: pthread_create failed\n");
            return 1;
        }
    }

    for (int i = 0; i < saikeiden_maara; i++) {
        pthread_join(saikeet[i], NULL);
    }

    pthread_mutex_destroy(&tila.lukko);

    int onko_odottava = 0;
    unsigned char odottava_merkki = 0;
    long long odottava_maara = 0;

    /* Tulokset kirjoitetaan alkuperäisessä järjestyksessä ja reunakohdat yhdistetään. */
    for (size_t i = 0; i < tyojen_maara; i++) {
        for (size_t j = 0; j < tulokset[i].jaksojen_maara; j++) {
            Jakso jakso = tulokset[i].jaksot[j];

            if (!onko_odottava) {
                odottava_merkki = jakso.merkki;
                odottava_maara = jakso.maara;
                onko_odottava = 1;
            } else if (jakso.merkki == odottava_merkki) {
                odottava_maara += jakso.maara;
            } else {
                kirjoita_jakso(odottava_maara, odottava_merkki);
                odottava_merkki = jakso.merkki;
                odottava_maara = jakso.maara;
            }
        }
    }

    if (onko_odottava) {
        kirjoita_jakso(odottava_maara, odottava_merkki);
    }

    for (size_t i = 0; i < tyojen_maara; i++) {
        free(tulokset[i].jaksot);
    }

    free(saikeet);
    free(tyot);
    free(tulokset);
    vapauta_tiedostot(tiedostot, tiedostojen_maara);
    free(tiedostot);

    return 0;
}

