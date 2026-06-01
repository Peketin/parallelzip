#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <limits.h>
#include <stdint.h>



/*
 * Vakioarvot
 *
 * JAKSOJA_PER_TYO määrittää, kuinka monta pakattua RLE-jaksoa yksi työ purkaa.
 * Tämä pitää työnjaon riittävän hienojakoisena useille säikeille.
 */
#define JAKSON_KOKO (sizeof(int) + sizeof(unsigned char))
#define JAKSOJA_PER_TYO 65536

/*
 * Tietorakenne muistimapattua pakattua tiedostoa varten
 *
 * Jokainen syötetiedosto mapataan muistiin. Työt viittaavat näihin muistialueisiin.
 */
typedef struct {
    int tiedostokuvaaja;
    unsigned char *data;
    size_t koko;
} MapattuTiedosto;

/*
 * Tietorakenne purkutyölle
 *
 * Tyo sisältää osoittimen pakattuihin jaksoihin ja tiedon siitä, kuinka monta
 * jaksoa tämän työn pitää purkaa. Jokainen säie muodostaa oman tulospuskurin.
 */
typedef struct {
    unsigned char *alku;
    size_t jaksojen_maara;
    unsigned char *tulos;
    size_t tuloksen_koko;
} Tyo;

/*
 * Yhteinen työjono säikeille
 *
 * seuraava_tyo on yhteinen laskuri. Mutex varmistaa, että jokainen työ annetaan
 * vain yhdelle säikeelle.
 */
typedef struct {
    Tyo *tyot;
    size_t toiden_maara;
    size_t seuraava_tyo;
    pthread_mutex_t lukko;
} TyoKonteksti;

/*
 * Apufunktio: turvallinen muistivaraus
 *
 * Jos muistivaraus epäonnistuu, ohjelma lopetetaan selkeällä virheellä.
 */
static void *varaa_muistia(size_t koko) {
    void *osoitin = malloc(koko);

    if (osoitin == NULL && koko != 0) {
        fprintf(stderr, "punzip: memory allocation failed\n");
        exit(1);
    }

    return osoitin;
}

/*
 * Yhden 5 tavun RLE-jakson lukeminen
 *
 * Kokonaislukua ei lueta struct-castilla, koska structissa voisi olla
 * täyttötavuja. memcpy lukee täsmälleen tiedostossa olevat 4 tavua int-muuttujaan.
 */
static void lue_jakso(unsigned char *osoitin, int *maara, unsigned char *merkki) {
    memcpy(maara, osoitin, sizeof(int));
    *merkki = osoitin[sizeof(int)];
}

/*
 * Yhden työn purkaminen
 *
 * Ensin lasketaan, kuinka suuri tulospuskuri tarvitaan. Sen jälkeen puskuri
 * täytetään memset()-kutsuilla. Tämä on tehokkaampaa kuin merkin tulostaminen
 * yksi kerrallaan säikeiden sisällä.
 */
static void pura_tyo(Tyo *tyo) {
    size_t tuloksen_koko = 0;
    size_t sijainti = 0;

    for (size_t indeksi = 0; indeksi < tyo->jaksojen_maara; indeksi++) {
        int maara;
        unsigned char merkki;

        lue_jakso(tyo->alku + indeksi * JAKSON_KOKO, &maara, &merkki);

        if (maara < 0) {
            fprintf(stderr, "punzip: invalid compressed data\n");
            exit(1);
        }

        if ((size_t)maara > SIZE_MAX - tuloksen_koko) {
            fprintf(stderr, "punzip: output too large\n");
            exit(1);
        }

        tuloksen_koko += (size_t)maara;
    }

    tyo->tulos = varaa_muistia(tuloksen_koko);
    tyo->tuloksen_koko = tuloksen_koko;

    for (size_t indeksi = 0; indeksi < tyo->jaksojen_maara; indeksi++) {
        int maara;
        unsigned char merkki;

        lue_jakso(tyo->alku + indeksi * JAKSON_KOKO, &maara, &merkki);
        memset(tyo->tulos + sijainti, merkki, (size_t)maara);
        sijainti += (size_t)maara;
    }
}

/*
 * Säikeen työntekijäfunktio
 *
 * Säie hakee töitä dynaamisesti yhteisestä jonosta. Varsinainen purku tapahtuu
 * ilman lukkoa, koska jokainen työ kirjoittaa omaan tulospuskuriinsa.
 */
static void *tyontekija(void *argumentti) {
    TyoKonteksti *konteksti = argumentti;

    while (1) {
        size_t tyon_indeksi;

        pthread_mutex_lock(&konteksti->lukko);

        if (konteksti->seuraava_tyo >= konteksti->toiden_maara) {
            pthread_mutex_unlock(&konteksti->lukko);
            break;
        }

        tyon_indeksi = konteksti->seuraava_tyo;
        konteksti->seuraava_tyo++;

        pthread_mutex_unlock(&konteksti->lukko);

        pura_tyo(&konteksti->tyot[tyon_indeksi]);
    }

    return NULL;
}

/*
 * Pakattujen tiedostojen lukeminen ja töiden muodostaminen
 *
 * Jokainen tiedosto avataan ja mapataan muistiin. Tiedosto jaetaan töihin niin,
 * että yksi työ sisältää JAKSOJA_PER_TYO kappaletta 5 tavun RLE-jaksoja.
 */
static void lue_tiedostot_ja_luo_tyot(int argumenttien_maara,
                                      char *argumentit[],
                                      MapattuTiedosto **mapatut_ulos,
                                      size_t *mapattujen_maara_ulos,
                                      Tyo **tyot_ulos,
                                      size_t *toiden_maara_ulos) {
    MapattuTiedosto *mapatut;
    Tyo *tyot = NULL;
    size_t toiden_maara = 0;
    size_t toiden_kapasiteetti = 0;
    size_t mapattujen_maara = 0;

    mapatut = varaa_muistia((size_t)(argumenttien_maara - 1) * sizeof(MapattuTiedosto));

    for (int indeksi = 1; indeksi < argumenttien_maara; indeksi++) {
        int tiedostokuvaaja;
        struct stat tiedot;
        unsigned char *data;
        size_t koko;
        size_t jaksojen_maara;

        tiedostokuvaaja = open(argumentit[indeksi], O_RDONLY);

        if (tiedostokuvaaja < 0) {
            fprintf(stderr, "punzip: cannot open file\n");
            exit(1);
        }

        if (fstat(tiedostokuvaaja, &tiedot) != 0) {
            fprintf(stderr, "punzip: cannot stat file\n");
            close(tiedostokuvaaja);
            exit(1);
        }

        koko = (size_t)tiedot.st_size;

        if (koko == 0) {
            close(tiedostokuvaaja);
            continue;
        }

        if (koko % JAKSON_KOKO != 0) {
            fprintf(stderr, "punzip: invalid compressed file\n");
            close(tiedostokuvaaja);
            exit(1);
        }

        data = mmap(NULL, koko, PROT_READ, MAP_PRIVATE, tiedostokuvaaja, 0);

        if (data == MAP_FAILED) {
            fprintf(stderr, "punzip: mmap failed\n");
            close(tiedostokuvaaja);
            exit(1);
        }

        mapatut[mapattujen_maara].tiedostokuvaaja = tiedostokuvaaja;
        mapatut[mapattujen_maara].data = data;
        mapatut[mapattujen_maara].koko = koko;
        mapattujen_maara++;

        jaksojen_maara = koko / JAKSON_KOKO;

        for (size_t alku = 0; alku < jaksojen_maara; alku += JAKSOJA_PER_TYO) {
            size_t maaran_pituus = JAKSOJA_PER_TYO;

            if (alku + maaran_pituus > jaksojen_maara) {
                maaran_pituus = jaksojen_maara - alku;
            }

            if (toiden_maara == toiden_kapasiteetti) {
                Tyo *uudet_tyot;

                if (toiden_kapasiteetti == 0) {
                    toiden_kapasiteetti = 128;
                } else {
                    toiden_kapasiteetti *= 2;
                }

                uudet_tyot = realloc(tyot, toiden_kapasiteetti * sizeof(Tyo));

                if (uudet_tyot == NULL) {
                    fprintf(stderr, "punzip: memory allocation failed\n");
                    exit(1);
                }

                tyot = uudet_tyot;
            }

            tyot[toiden_maara].alku = data + alku * JAKSON_KOKO;
            tyot[toiden_maara].jaksojen_maara = maaran_pituus;
            tyot[toiden_maara].tulos = NULL;
            tyot[toiden_maara].tuloksen_koko = 0;
            toiden_maara++;
        }
    }

    *mapatut_ulos = mapatut;
    *mapattujen_maara_ulos = mapattujen_maara;
    *tyot_ulos = tyot;
    *toiden_maara_ulos = toiden_maara;
}

/*
 * Tulosten kirjoittaminen alkuperäisessä järjestyksessä
 *
 * Säikeet voivat valmistua eri järjestyksessä, mutta puretun datan pitää tulla
 * ulos täsmälleen alkuperäisessä järjestyksessä. Siksi kirjoitus tehdään tässä
 * pääsäikeessä työnumeron mukaisessa järjestyksessä.
 */
static void kirjoita_tulokset(Tyo *tyot, size_t toiden_maara) {
    for (size_t indeksi = 0; indeksi < toiden_maara; indeksi++) {
        fwrite(tyot[indeksi].tulos, 1, tyot[indeksi].tuloksen_koko, stdout);
    }
}

/*
 * Resurssien vapauttaminen
 *
 * Vapautetaan puretut tulospuskurit, mmap-alueet ja tiedostokuvaajat.
 */
static void vapauta_resurssit(MapattuTiedosto *mapatut,
                              size_t mapattujen_maara,
                              Tyo *tyot,
                              size_t toiden_maara) {
    for (size_t indeksi = 0; indeksi < toiden_maara; indeksi++) {
        free(tyot[indeksi].tulos);
    }

    for (size_t indeksi = 0; indeksi < mapattujen_maara; indeksi++) {
        munmap(mapatut[indeksi].data, mapatut[indeksi].koko);
        close(mapatut[indeksi].tiedostokuvaaja);
    }

    free(tyot);
    free(mapatut);
}

/*
 * main-funktio
 *
 * main tarkistaa argumentit, muodostaa purkutyöt, käynnistää säikeet ja
 * kirjoittaa valmiit tulokset standard outputiin.
 */
int main(int argumenttien_maara, char *argumentit[]) {
    MapattuTiedosto *mapatut;
    Tyo *tyot;
    size_t mapattujen_maara;
    size_t toiden_maara;
    int saikeiden_maara;
    pthread_t *saikeet;
    TyoKonteksti konteksti;

    if (argumenttien_maara < 2) {
        printf("punzip: file1 [file2 ...]\n");
        return 1;
    }

    lue_tiedostot_ja_luo_tyot(argumenttien_maara,
                              argumentit,
                              &mapatut,
                              &mapattujen_maara,
                              &tyot,
                              &toiden_maara);

    if (toiden_maara == 0) {
        vapauta_resurssit(mapatut, mapattujen_maara, tyot, toiden_maara);
        return 0;
    }

    saikeiden_maara = get_nprocs();

    if (saikeiden_maara < 1) {
        saikeiden_maara = 1;
    }

    if ((size_t)saikeiden_maara > toiden_maara) {
        saikeiden_maara = (int)toiden_maara;
    }

    konteksti.tyot = tyot;
    konteksti.toiden_maara = toiden_maara;
    konteksti.seuraava_tyo = 0;
    pthread_mutex_init(&konteksti.lukko, NULL);

    saikeet = varaa_muistia((size_t)saikeiden_maara * sizeof(pthread_t));

    for (int indeksi = 0; indeksi < saikeiden_maara; indeksi++) {
        if (pthread_create(&saikeet[indeksi], NULL, tyontekija, &konteksti) != 0) {
            fprintf(stderr, "punzip: pthread_create failed\n");
            exit(1);
        }
    }

    for (int indeksi = 0; indeksi < saikeiden_maara; indeksi++) {
        pthread_join(saikeet[indeksi], NULL);
    }

    kirjoita_tulokset(tyot, toiden_maara);

    pthread_mutex_destroy(&konteksti.lukko);
    free(saikeet);
    vapauta_resurssit(mapatut, mapattujen_maara, tyot, toiden_maara);

    return 0;
}
