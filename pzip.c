#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <fcntl.h>
#include <unistd.h>
#include <limits.h>


/*
 * Vakioarvot
 *
 * PALA_KOKO määrittää, kuinka suuri osa syötteestä annetaan yhdelle
 * työlle. Pienempi koko parantaa kuormantasausta, mutta kasvattaa
 * hallinnollista työtä. Suurempi koko vähentää hallintaa, mutta voi jakaa
 * työn epätasaisemmin säikeiden kesken.
 */
#define PALA_KOKO (1024 * 1024)

/*
 * Tietorakenteet RLE-jaksoille ja töille
 *
 * RleJakso vastaa yhtä pakattua jaksoa: sama merkki toistuu monta kertaa.
 * Tyo vastaa yhtä syötteen palaa, jonka joku säie pakkaa.
 */
typedef struct {
    int maara;
    unsigned char merkki;
} RleJakso;

typedef struct {
    unsigned char *alku;
    size_t pituus;
    RleJakso *jaksot;
    size_t jaksojen_maara;
    size_t kapasiteetti;
} Tyo;

/*
 * Tietorakenne muistimapatuista tiedostoista
 *
 * Tiedostot pidetään muistimapattuina niin kauan, että kaikki säikeet ovat
 * pakannet niihin viittaavat palat. Tämän jälkeen muistialueet vapautetaan.
 */
typedef struct {
    int tiedostokuvaaja;
    unsigned char *data;
    size_t koko;
} MapattuTiedosto;

/*
 * Yhteinen työjono säikeille
 *
 * Kaikki säikeet käyttävät samaa TyoKonteksti-rakennetta. seuraava_tyo kertoo,
 * mikä työ annetaan seuraavaksi. Mutex suojaa tätä indeksiä, jotta kaksi
 * säiettä ei voi ottaa samaa työtä.
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
 * Jos muistia ei saada varattua, ohjelma lopettaa virheeseen. Tämä pitää
 * varsinaisen pakkauskoodin yksinkertaisempana.
 */
static void *varaa_muistia(size_t koko) {
    void *osoitin = malloc(koko);

    if (osoitin == NULL && koko != 0) {
        fprintf(stderr, "pzip: memory allocation failed\n");
        exit(1);
    }

    return osoitin;
}

/*
 * Apufunktio: kasvatetaan RLE-jaksotaulukkoa
 *
 * Jokainen työ tuottaa oman RLE-jaksotaulukkonsa. Taulukkoa kasvatetaan
 * tarvittaessa dynaamisesti.
 */
static void lisaa_jakso(Tyo *tyo, int maara, unsigned char merkki) {
    if (tyo->jaksojen_maara == tyo->kapasiteetti) {
        size_t uusi_kapasiteetti;
        RleJakso *uudet_jaksot;

        if (tyo->kapasiteetti == 0) {
            uusi_kapasiteetti = 128;
        } else {
            uusi_kapasiteetti = tyo->kapasiteetti * 2;
        }

        uudet_jaksot = realloc(tyo->jaksot, uusi_kapasiteetti * sizeof(RleJakso));

        if (uudet_jaksot == NULL) {
            fprintf(stderr, "pzip: memory allocation failed\n");
            exit(1);
        }

        tyo->jaksot = uudet_jaksot;
        tyo->kapasiteetti = uusi_kapasiteetti;
    }

    tyo->jaksot[tyo->jaksojen_maara].maara = maara;
    tyo->jaksot[tyo->jaksojen_maara].merkki = merkki;
    tyo->jaksojen_maara++;
}

/*
 * Yhden palan pakkaus
 *
 * Tämä funktio ei käytä lukkoa, koska kukin säie käsittelee vain omaa
 * Tyo-rakennettaan. Lukkoa tarvitaan vain uuden työn hakemiseen.
 */
static void pakkaa_tyo(Tyo *tyo) {
    unsigned char nykyinen_merkki;
    int maara;

    if (tyo->pituus == 0) {
        return;
    }

    nykyinen_merkki = tyo->alku[0];
    maara = 1;

    for (size_t indeksi = 1; indeksi < tyo->pituus; indeksi++) {
        if (tyo->alku[indeksi] == nykyinen_merkki) {
            maara++;
        } else {
            lisaa_jakso(tyo, maara, nykyinen_merkki);
            nykyinen_merkki = tyo->alku[indeksi];
            maara = 1;
        }
    }

    lisaa_jakso(tyo, maara, nykyinen_merkki);
}

/*
 * Säikeen suorittama työntekijäfunktio
 *
 * Säie hakee aina seuraavan vapaan työn yhteisestä indeksistä. Tämä on
 * dynaamista työnjakoa: jos yksi säie saa vaikeamman tai hitaamman palan,
 * muut säikeet voivat jatkaa uusien palojen käsittelyä.
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

        pakkaa_tyo(&konteksti->tyot[tyon_indeksi]);
    }

    return NULL;
}

/*
 * Pakatun jakson kirjoittaminen
 *
 * Tehtävänannon formaatti vaatii binäärisen kokonaisluvun ja merkin.
 * fwrite() kirjoittaa kokonaisluvun binäärimuodossa, ei tekstinä.
 */
static void kirjoita_jakso(int maara, unsigned char merkki) {
    fwrite(&maara, sizeof(int), 1, stdout);
    fwrite(&merkki, sizeof(unsigned char), 1, stdout);
}

/*
 * Pitkän jakson kirjoittaminen turvallisesti
 *
 * RLE-jakso voi kasvaa palojen yhdistämisen takia suuremmaksi kuin yksittäinen
 * pala. Formaatti käyttää int-arvoa, joten erittäin pitkä jakso jaetaan
 * tarvittaessa useaksi kirjoitettavaksi jaksoksi.
 */
static void kirjoita_pitka_jakso(long long maara, unsigned char merkki) {
    while (maara > INT_MAX) {
        kirjoita_jakso(INT_MAX, merkki);
        maara -= INT_MAX;
    }

    if (maara > 0) {
        kirjoita_jakso((int)maara, merkki);
    }
}

/*
 * Tulosten yhdistäminen ja kirjoitus oikeassa järjestyksessä
 *
 * Säikeet voivat valmistua missä järjestyksessä tahansa, mutta pakattu tulos
 * täytyy kirjoittaa alkuperäisessä syötejärjestyksessä. Tässä vaiheessa myös
 * yhdistetään vierekkäiset RLE-jaksot, joiden merkki on sama.
 */
static void kirjoita_tulokset(Tyo *tyot, size_t toiden_maara) {
    int onko_odottava = 0;
    unsigned char odottava_merkki = 0;
    long long odottava_maara = 0;

    for (size_t tyon_indeksi = 0; tyon_indeksi < toiden_maara; tyon_indeksi++) {
        Tyo *tyo = &tyot[tyon_indeksi];

        for (size_t jakson_indeksi = 0; jakson_indeksi < tyo->jaksojen_maara; jakson_indeksi++) {
            RleJakso *jakso = &tyo->jaksot[jakson_indeksi];

            if (onko_odottava && jakso->merkki == odottava_merkki) {
                odottava_maara += jakso->maara;
            } else {
                if (onko_odottava) {
                    kirjoita_pitka_jakso(odottava_maara, odottava_merkki);
                }

                odottava_merkki = jakso->merkki;
                odottava_maara = jakso->maara;
                onko_odottava = 1;
            }
        }
    }

    if (onko_odottava) {
        kirjoita_pitka_jakso(odottava_maara, odottava_merkki);
    }
}

/*
 * Tiedostojen muistimappaus ja töiden muodostaminen
 *
 * Jokainen tiedosto avataan, sen koko selvitetään fstat()-kutsulla ja sisältö
 * mapataan muistiin mmap()-kutsulla. Tämän jälkeen tiedostosta muodostetaan
 * yksi tai useampi työ PALA_KOKO-vakion perusteella.
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

        tiedostokuvaaja = open(argumentit[indeksi], O_RDONLY);

        if (tiedostokuvaaja < 0) {
            fprintf(stderr, "pzip: cannot open file\n");
            exit(1);
        }

        if (fstat(tiedostokuvaaja, &tiedot) != 0) {
            fprintf(stderr, "pzip: cannot stat file\n");
            close(tiedostokuvaaja);
            exit(1);
        }

        koko = (size_t)tiedot.st_size;

        if (koko == 0) {
            close(tiedostokuvaaja);
            continue;
        }

        data = mmap(NULL, koko, PROT_READ, MAP_PRIVATE, tiedostokuvaaja, 0);

        if (data == MAP_FAILED) {
            fprintf(stderr, "pzip: mmap failed\n");
            close(tiedostokuvaaja);
            exit(1);
        }

        mapatut[mapattujen_maara].tiedostokuvaaja = tiedostokuvaaja;
        mapatut[mapattujen_maara].data = data;
        mapatut[mapattujen_maara].koko = koko;
        mapattujen_maara++;

        for (size_t alku = 0; alku < koko; alku += PALA_KOKO) {
            size_t pituus = PALA_KOKO;

            if (alku + pituus > koko) {
                pituus = koko - alku;
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
                    fprintf(stderr, "pzip: memory allocation failed\n");
                    exit(1);
                }

                tyot = uudet_tyot;
            }

            tyot[toiden_maara].alku = data + alku;
            tyot[toiden_maara].pituus = pituus;
            tyot[toiden_maara].jaksot = NULL;
            tyot[toiden_maara].jaksojen_maara = 0;
            tyot[toiden_maara].kapasiteetti = 0;
            toiden_maara++;
        }
    }

    *mapatut_ulos = mapatut;
    *mapattujen_maara_ulos = mapattujen_maara;
    *tyot_ulos = tyot;
    *toiden_maara_ulos = toiden_maara;
}

/*
 * Resurssien vapauttaminen
 *
 * Lopuksi vapautetaan jokaisen työn RLE-taulukko sekä mmap()-kutsulla
 * varatut tiedostoalueet.
 */
static void vapauta_resurssit(MapattuTiedosto *mapatut,
                              size_t mapattujen_maara,
                              Tyo *tyot,
                              size_t toiden_maara) {
    for (size_t indeksi = 0; indeksi < toiden_maara; indeksi++) {
        free(tyot[indeksi].jaksot);
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
 * main tarkistaa argumentit, muodostaa työt, käynnistää säikeet, odottaa niiden
 * valmistumista ja kirjoittaa lopuksi pakatun tuloksen standard outputiin.
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
        printf("pzip: file1 [file2 ...]\n");
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
            fprintf(stderr, "pzip: pthread_create failed\n");
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

